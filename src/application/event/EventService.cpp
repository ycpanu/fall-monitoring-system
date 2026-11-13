#include "fall_monitoring/application/event/EventService.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fall_monitoring::application::event
{
    using namespace std::chrono_literals;

    EventService::EventService(EventServiceOptions options, EventHandler handler, ports::IAlertRepository& repository, EmergencyAlarmHandler emergencyAlarm)
        : options_(std::move(options)), queue_(options_.queueCapacity),
          correlator_(options_.deviceId, options_.deploymentArea, options_.videoOutputDir, options_.fusionWindowMs),
          handler_(std::move(handler)), repository_(repository),
          emergencyAlarm_(std::move(emergencyAlarm))
    {
        if (options_.submitTimeout.count() < 0)
            throw std::invalid_argument("EventService: submitTimeout 不能为负数");
    }

    EventService::~EventService() { stop(); }

    bool EventService::start()
    {
        if (!handler_ || startedOnce_.exchange(true)) return false;
        stopping_.store(false);
        accepting_.store(true);
        try { worker_ = std::thread(&EventService::consumeLoop, this); }
        catch (const std::exception& error)
        {
            accepting_.store(false);
            SPDLOG_CRITICAL("[EventService] 消费线程启动失败：{}", error.what());
            return false;
        }
        SPDLOG_INFO("[EventService] 已启动：内存容量={}，持久化溢出缓存已启用", options_.queueCapacity);
        return true;
    }

    bool EventService::submit(domain::event::AlertEvent event)
    {
        if (!accepting_.load()) return false;
        if (queue_.pushFor(std::move(event), options_.submitTimeout)) return true;
        // pushFor() 仅在真正入队时移动 event；队列拥塞后仍能读取原始事件。
        if (!accepting_.load()) return false;
        // 拥塞时不等磁盘消费者，先触发非阻塞 GPIO 本地报警。
        if (emergencyAlarm_)
        {
            try { event.localAlarmAlreadyRequested = emergencyAlarm_(); }
            catch (...) { SPDLOG_ERROR("[EventService] 应急本地蜂鸣回调异常"); }
        }
        const bool saved = repository_.stashIncoming(event);
        if (saved)
        {
            SPDLOG_WARN("[EventService] 内存事件队列满：事件已持久化到 SQLite 溢出缓存");
            retryCv_.notify_one();
            return true;
        }
        SPDLOG_CRITICAL("[EventService] 内存队列和 SQLite 溢出缓存同时无法接受事件：type={}，track={}", domain::event::toString(event.eventType), event.personTrackId);
        return false;
    }

    bool EventService::handlePrepared(domain::event::AlertEvent raw, std::uint64_t& persisted, std::uint64_t& fused) noexcept
    {
        domain::event::AlertEvent prepared;
        try
        {
            prepared = correlator_.prepare(std::move(raw));
            if (prepared.isFusionUpdate) ++fused;
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[EventService] 事件融合失败，原始数据需要人工核查：{}", error.what());
            return false;
        }
        catch (...)
        {
            SPDLOG_CRITICAL("[EventService] 事件融合失败：未知异常");
            return false;
        }
        while (true)
        {
            bool committed = false;
            try { committed = handler_(prepared); }
            catch (const std::exception& error)
            {
                SPDLOG_ERROR("[EventService] 事件处理异常：event_id={}，error={}", prepared.eventId, error.what());
            }
            catch (...)
            {
                SPDLOG_ERROR("[EventService] 事件处理未知异常：event_id={}", prepared.eventId);
            }
            if (committed)
            {
                ++persisted;
                SPDLOG_INFO("[EventService] 事件提交成功：event_id={}, revision={}, fusion={}", prepared.eventId, prepared.revision, prepared.isFusionUpdate);
                return true;
            }
            if (stopping_.load())
            {
                SPDLOG_CRITICAL("[EventService] 停机期间无法提交事件：{}；若来自溢出缓存将保留原始记录", prepared.eventId);
                return false;
            }
            SPDLOG_ERROR("[EventService] 本地数据库不可写：{}，1 秒后重试", prepared.eventId);
            std::unique_lock<std::mutex> lock(retryMutex_);
            retryCv_.wait_for(lock, 1s, [this] { return stopping_.load(); });
        }
    }

    void EventService::consumeLoop() noexcept
    {
        SPDLOG_INFO("[EventService] 消费线程启动，先恢复已有 SQLite 溢出事件");
        std::uint64_t persisted = 0;
        std::uint64_t fused = 0;
        std::uint64_t overflow = 0;
        std::int64_t awaitingDelete = 0;
        while (true)
        {
            // 保存已成功落库但尚未从溢出队列删除的行号，避免单进程内重复融合。
            if (awaitingDelete != 0)
            {
                if (repository_.eraseIncoming(awaitingDelete))
                {
                    awaitingDelete = 0;
                    ++overflow;
                }
                else
                {
                    if (stopping_.load()) break;
                    std::unique_lock<std::mutex> lock(retryMutex_);
                    retryCv_.wait_for(lock, 1s, [this] { return stopping_.load(); });
                    continue;
                }
            }
            // 必须检查持久化溢出队列：即使上一进程在重启前有遗留记录也能恢复。
            std::vector<ports::PendingIncomingEvent> batch;
            const bool scanned = repository_.loadIncoming(batch, 1);
            if (scanned && !batch.empty())
            {
                const auto& item = batch.front();
                auto raw = item.event;
                // 为持久化溢出记录分配可重复推导的 ID，避免重试生成新的逻辑报警。
                if (raw.eventId.empty())
                    raw.eventId = options_.deviceId + "_overflow_" + std::to_string(item.rowId);
                if (!handlePrepared(std::move(raw), persisted, fused))
                {
                    if (stopping_.load()) break;
                    continue;
                }
                awaitingDelete = item.rowId;
                // 在下一轮先确认删除，不重复执行已经成功提交的事件。
            }
            else if (!scanned)
            {
                SPDLOG_ERROR("[EventService] SQLite 溢出队列暂不可读，等待恢复");
                if (stopping_.load()) break;
                std::unique_lock<std::mutex> lock(retryMutex_);
                retryCv_.wait_for(lock, 1s, [this] { return stopping_.load(); });
            }

            domain::event::AlertEvent incoming;
            if (queue_.waitPopFor(incoming, 200ms))
                (void)handlePrepared(std::move(incoming), persisted, fused);
            else if (stopping_.load() && queue_.size() == 0)
            {
                // 退出前排空仍可正常读取的磁盘溢出队列。
                if (awaitingDelete != 0) continue;
                std::vector<ports::PendingIncomingEvent> remaining;
                if (repository_.loadIncoming(remaining, 1) && !remaining.empty()) continue;
                // 数据库不可读时保留残留记录，供下次启动恢复。
                break;
            }
        }
        SPDLOG_INFO("[EventService] 退出：成功提交={}，跨模态融合={}，恢复溢出={}", persisted, fused, overflow);
    }

    void EventService::stop() noexcept
    {
        accepting_.store(false);
        stopping_.store(true);
        queue_.close();
        retryCv_.notify_all();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id()) std::terminate();
            worker_.join();
        }
    }
}