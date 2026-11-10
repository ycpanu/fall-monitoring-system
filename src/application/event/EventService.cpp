#include "fall_monitoring/application/event/EventService.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fall_monitoring::application::event
{
    EventService::EventService(EventServiceOptions options, EventHandler onPreparedEvent) : options_(std::move(options)), queue_(options_.queueCapacity), correlator_(options_.deviceId, options_.deploymentArea, options_.videoOutputDir, options_.fusionWindowMs), handler_(std::move(onPreparedEvent))
    {
        if (options_.submitTimeout.count() < 0)
        {
            throw std::invalid_argument("EventService: submitTimeout 不能为负数");
        }
    }

    EventService::~EventService()
    {
        stop();
    }

    bool EventService::start()
    {
        if (!handler_)
        {
            SPDLOG_ERROR("[EventService] 未设置下游事件处理器，拒绝启动");
            return false;
        }
        if (startedOnce_.exchange(true))
        {
            SPDLOG_ERROR("[EventService] 事件队列不可重复启动，请由 Application 管理生命周期");
            return false;
        }

        stopping_.store(false);
        accepting_.store(true);
        try
        {
            worker_ = std::thread(&EventService::consumeLoop, this);
        }
        catch (const std::exception& e)
        {
            accepting_.store(false);
            SPDLOG_ERROR("[EventService] 创建消费线程失败：{}", e.what());
            return false;
        }
        SPDLOG_INFO("[EventService] 已启动：device={}，窗口={}ms，队列容量={}，提交超时={}ms", options_.deviceId, options_.fusionWindowMs, options_.queueCapacity, options_.submitTimeout.count());
        return true;
    }

    bool EventService::submit(domain::event::AlertEvent event)
    {
        if (!accepting_.load())
        {
            SPDLOG_ERROR("[EventService] 事件服务未接收新事件：type={}，track={}", domain::event::toString(event.eventType), event.personTrackId);
            return false;
        }
        // 有界等待：不能让 RKNN 或 ALSA 采集线程无限期等待报警消费者。
        // pushFor 返回 false 时不会移动事件，且会显式记录错误。
        const auto type = event.eventType;
        const int trackId = event.personTrackId;
        const bool accepted = queue_.pushFor(std::move(event), options_.submitTimeout);
        if (!accepted)
        {
            SPDLOG_ERROR("[EventService] 报警事件入队失败（满队列/关闭/超时）：type={}，track={}，queued={}", domain::event::toString(type), trackId, queue_.size());
        }
        return accepted;
    }

    void EventService::consumeLoop() noexcept
    {
        using namespace std::chrono_literals;
        SPDLOG_INFO("[EventService] 事件消费线程启动");
        std::uint64_t persisted = 0;
        std::uint64_t fused = 0;
        std::uint64_t saveFailures = 0;
        domain::event::AlertEvent incoming;

        // 仅消费线程更新 EventCorrelator，保证同一事件的初始记录先于融合更新落库。
        while (queue_.waitPop(incoming))
        {
            domain::event::AlertEvent prepared;
            try
            {
                prepared = correlator_.prepare(std::move(incoming));
                incoming = {};
                if (prepared.isFusionUpdate) ++fused;
            }
            catch (const std::exception& e)
            {
                // 事件本身无法准备，记录严重错误；不能将失败伪装成持久化成功。
                SPDLOG_CRITICAL("[EventService] 事件融合异常，无法落库：{}", e.what());
                continue;
            }
            catch (...)
            {
                SPDLOG_CRITICAL("[EventService] 事件融合发生未知异常，无法落库");
                continue;
            }

            // 仓储不在推理/ALSA 线程写入：仅此消费线程同步等待 SQLite 提交。
            // 落盘失败时不取走下一条事件，队列背压会限制上游继续产生报警。
            while (true)
            {
                bool committed = false;
                try
                {
                    committed = handler_(prepared);
                }
                catch (const std::exception& e)
                {
                    SPDLOG_ERROR("[EventService] 持久化回调异常：event_id={}，error={}", prepared.eventId, e.what());
                }
                catch (...)
                {
                    SPDLOG_ERROR("[EventService] 持久化回调发生未知异常：event_id={}", prepared.eventId);
                }

                if (committed)
                {
                    ++persisted;
                    SPDLOG_INFO("[EventService] 事件持久化完成：event_id={}，融合更新={}", prepared.eventId, prepared.isFusionUpdate);
                    break;
                }

                ++saveFailures;
                if (stopping_.load())
                {
                    // 停机时不能无限阻塞进程退出；磁盘故障下无法保证本事件不丢失。
                    SPDLOG_CRITICAL("[EventService] 停止期间仍无法持久化事件：event_id={}，存在报警丢失风险", prepared.eventId);
                    break;
                }

                SPDLOG_ERROR("[EventService] SQLite 未确认提交，1 秒后重试：event_id={}，失败次数={}", prepared.eventId, saveFailures);
                std::unique_lock<std::mutex> lock(retryMutex_);
                retryCv_.wait_for(lock, 1s, [this] { return stopping_.load(); });
            }
        }
        SPDLOG_INFO("[EventService] 事件消费线程退出：成功持久化={}，融合更新={}，写入失败次数={}", persisted, fused, saveFailures);
    }

    void EventService::stop() noexcept
    {
        accepting_.store(false);
        stopping_.store(true);
        queue_.close();
        retryCv_.notify_all();
        if (worker_.joinable())
        {
            // 生命周期由管理线程控制。不得在该消费线程内停止自身。
            if (worker_.get_id() == std::this_thread::get_id())
            {
                SPDLOG_CRITICAL("[EventService] 不允许从事件回调线程调用 stop()");
                std::terminate();
            }
            worker_.join();
            SPDLOG_INFO("[EventService] 已停止，队列已排空");
        }
    }
}
