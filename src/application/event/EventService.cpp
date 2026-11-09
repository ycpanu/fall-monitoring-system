#include "fall_monitoring/application/event/EventService.hpp"

#include <spdlog/spdlog.h>

#include <exception>
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

        accepting_.store(true);
        try 
        {
            worker_= std::thread(&EventService::consumeLoop, this);
        }
        catch(const std::exception& e)
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
        SPDLOG_INFO("[EventService] 事件消费者线程启动");
        std::uint64_t processed = 0;
        std::uint64_t fused = 0;
        std::uint64_t failed = 0;
        domain::event::AlertEvent incoming;

        // close() 后仍会消费队列剩余事件再退出。
        while (queue_.waitPop(incoming))
        {
            try 
            {
                auto prepared = correlator_.prepare(std::move(incoming));
                incoming= {}; // 清除上一轮事件负载，避免复用旧字段。
                if (prepared.isFusionUpdate)
                {
                    ++fused;
                    SPDLOG_INFO("[EventService] 跨模态融合：event_id={}，sources={}，keyword={}", prepared.eventId, prepared.source.size(), prepared.keyword);
                }
                else 
                {
                    SPDLOG_INFO("[EventService] 新报警事件：event_id={}，type={}，track={}", prepared.eventId, domain::event::toString(prepared.eventType), prepared.personTrackId);
                }

                // 下游需要按 eventId 幂等合并；回调本线程在内顺序执行。
                handler_(std::move(prepared));
                ++processed;
            }
            catch (const std::exception& e)
            {
                ++failed;
                SPDLOG_ERROR("[EventService] 事件处理回调异常，事件可能未成功交付：{}", e.what());
            }
            catch (...)
            {
                ++failed;
                SPDLOG_ERROR("[EventService] 事件处理发生未知异常，事件可能未成功交付");
            }
        }
        SPDLOG_INFO("[EventService] 消费线程退出：处理={}，融合更新={}，失败={}", processed, fused, failed);
    }

    void EventService::stop() noexcept 
    {
        accepting_.store(false);
        queue_.close();
        if (worker_.joinable())
        {
            // 生命周期由管理线程控制，不得在该消费者线程内停止自身。
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