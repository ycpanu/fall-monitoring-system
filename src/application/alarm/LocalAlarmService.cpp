#include "fall_monitoring/application/alarm/LocalAlarmService.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace fall_monitoring::application::alarm
{
    LocalAlarmService::LocalAlarmService(ports::ILocalAlarm& alarm, int durationMs, std::size_t maxRecentIds) : alarm_(alarm), durationMs_(durationMs), maxRecentIds_(maxRecentIds)
    {
        if (durationMs <= 0 || durationMs > 60000 || maxRecentIds == 0)
            throw std::invalid_argument("LocalAlarmService: 鸣叫时长必须在 1..60000ms 且去重容量大于 0");
    }

    void LocalAlarmService::onPreparedEvent(const domain::event::AlertEvent& event) noexcept
    {
        if (event.isFusionUpdate) return;
        if (event.eventType != domain::event::EventType::FALL && event.eventType != domain::event::EventType::HELP_REQUEST) return;
        if (event.eventId.empty())
        {
            SPDLOG_ERROR("[LocalAlarm] 报警 event_id 为空，拒绝触发以避免重复鸣叫");
            return;
        }

        try
        {
            // EventService 通常单线程调用，此锁确保未来其他入口仍保持去重语义。
            std::lock_guard<std::mutex> lock(mutex_);
            if (recentIds_.find(event.eventId) != recentIds_.end()) return;

            if (!alarm_.trigger(std::chrono::milliseconds(durationMs_)))
            {
                SPDLOG_ERROR("[LocalAlarm] 本地蜂鸣请求未被接收：event_id={}", event.eventId);
                return;
            }

            recentIds_.insert(event.eventId);
            recentOrder_.push_back(event.eventId);
            while (recentOrder_.size() > maxRecentIds_)
            {
                recentIds_.erase(recentOrder_.front());
                recentOrder_.pop_front();
            }
            SPDLOG_WARN("[LocalAlarm] 本地报警触发：event_id={}, type={}, duration={}ms", event.eventId, domain::event::toString(event.eventType), durationMs_);
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[LocalAlarm] 处理报警失败：{}", e.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("[LocalAlarm] 处理报警发生未知异常");
        }
    }
}
