#pragma once

#include "fall_monitoring/application/ports/ILocalAlarm.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

namespace fall_monitoring::application::alarm
{
    /**
     * @brief 统一报警事件的本地蜂鸣路由服务。
     *
     * 首次报警鸣叫；EventCorrelator 的融合更新不重复鸣叫。
     * event_id 去重能避免 SQLite 写入失败引起的 EventService 重试重复鸣叫。
     * 只保留最近 maxRecentIds 条 ID，避免长期运行内存无限增长。
     * 本对象不负责 MQTT/SQLite，断网也能执行本地报警。
     */
    class LocalAlarmService final
    {
    public:
        explicit LocalAlarmService(ports::ILocalAlarm& alarm, int durationMs = 3000, std::size_t maxRecentIds = 512);
        LocalAlarmService(const LocalAlarmService&) = delete;
        LocalAlarmService& operator=(const LocalAlarmService&) = delete;

        void onPreparedEvent(const domain::event::AlertEvent& event) noexcept;

    private:
        ports::ILocalAlarm& alarm_;
        int durationMs_;
        std::size_t maxRecentIds_;
        std::mutex mutex_;
        std::deque<std::string> recentOrder_;
        std::unordered_set<std::string> recentIds_;
    };
}
