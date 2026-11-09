#pragma once

#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <cstdint>
#include <string>

namespace fall_monitoring::domain::event
{
    /**
     * @brief 视觉/语音事件业务关联器
     * 
     * 1.仅合并位于时间窗内、包含不同检测来源的事件。
     * 2.融合沿用首次事件的 ID、类型和时间戳。
     * 3.语音后到保留人体位置，视觉后到时补充事件录像路径。
     * 
     * 本对象由 EventService 的单一消费者线程独占使用。
     * 不对外提供并发安全保证，避免在领域算法引入锁
     */
    class EventCorrelator final
    {
    public:
        EventCorrelator(std::string deviceId, std::string deploymentArea, std::string videoOutputDir, std::int64_t fusionWindowMs = 10000);
        [[nodiscard]] AlertEvent prepare(AlertEvent event);

    private:
        [[nodiscard]] std::string makeEventId(std::int64_t timestamp);
        [[nodiscard]] static bool hasSource(const AlertEvent& event, EventSource source) noexcept;

        std::string deviceId_;
        std::string deviceId_;
        std::string deploymentArea_;
        std::string videoOutputDir_;
        std::int64_t fusionWindowMs_ = 10000;
        std::uint64_t nextSequence_ = 0;
        AlertEvent activeEvent_;
        bool hasActiveEvent_ = false;
    };
}