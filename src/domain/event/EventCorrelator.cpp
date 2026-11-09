#include "fall_monitoring/domain/event/EventCorrelator.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fall_monitoring::domain::event
{
    namespace
    {
        [[nodiscard]] std::int64_t nowUnixMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

    EventCorrelator::EventCorrelator(std::string deviceId, std::string deploymentArea, std::string videoOutputDir, std::int64_t fusionWindowMs) : deviceId_(std::move(deviceId)), deploymentArea_(std::move(deploymentArea)), videoOutputDir_(std::move(videoOutputDir)), fusionWindowMs_(fusionWindowMs)
    {
        if (deviceId_.empty() || fusionWindowMs_ < 0)
        {
            throw std::invalid_argument("EventCorrelator: deviceId 不能为空，融合窗口不能为负数");
        }
    }

    bool EventCorrelator::hasSource(const AlertEvent& event, EventSource source) noexcept
    {
        return std::find(event.source.begin(), event.source.end(), source) != event.source.end();
    }

    std::string EventCorrelator::makeEventId(std::int64_t timestamp)
    {
        // 设备 ID + Unix 毫秒 + 进程内递增序号。
        return deviceId_ + "_"+std::to_string(timestamp) + "_" + std::to_string(nextSequence_++);
    }

    AlertEvent EventCorrelator::prepare(AlertEvent event)
    {
        if (event.timestamp <= 0)
        {
            event.timestamp = nowUnixMilliseconds();
        }
        event.deviceId = deviceId_;
        event.deploymentArea = deploymentArea_;
        event.status = EventStatus::NEW;
        if (event.eventId.empty())
        {
            event.eventId = makeEventId(event.timestamp);
        }

        const bool incomingVision = hasSource(event, EventSource::VISION);
        const bool incomingVoice = hasSource(event, EventSource::VOICE);
        event.captureVideo = incomingVision;

        bool canFuse = false;
        if (hasActiveEvent_)
        {
            // 沿用首次事件时间戳作为融合窗口锚点。
            const auto first = activeEvent_.timestamp;
            const auto second = event.timestamp;
            const auto diff = first > second ? first - second : second - first;
            const auto crossModal = (incomingVision && hasSource(activeEvent_, EventSource::VISION)) || (incomingVoice && hasSource(activeEvent_, EventSource::VISION));
            canFuse = diff <= fusionWindowMs_ && crossModal;
        }

        if (canFuse)
        {
            // 同一逻辑报警通过同一个 event_id 做后续 SQLite UPSERT / MQTT 幂等更新。
            event.eventId = activeEvent_.eventId;
            event.eventType = activeEvent_.eventType;
            event.timestamp = activeEvent_.timestamp;
            event.isFusionUpdate = true;
            event.source = activeEvent_.source;

            if (incomingVision && !hasSource(event, EventSource::VISION))
                event.source.push_back(EventSource::VISION);
            if (incomingVoice && !hasSource(event, EventSource::VOICE))
                event.source.push_back(EventSource::VOICE);

            if (event.keyword.empty())
                event.keyword = activeEvent_.keyword;
            if (!incomingVision)
            {
                event.personTrackId = activeEvent_.personTrackId;
                event.triggerBoxX = activeEvent_.triggerBoxX;
                event.triggerBoxY = activeEvent_.triggerBoxY;
                event.videoPath = activeEvent_.videoPath;
            }
            if (incomingVision && event.videoPath.empty())
            {
                event.videoPath = (std::filesystem::path(videoOutputDir_) /
                                   ("fall_" + event.eventId + ".mp4")).string();
            }
            activeEvent_ = event;
            return event;
        }

        event.isFusionUpdate = false;
        if (incomingVision)
        {
            event.videoPath = (std::filesystem::path(videoOutputDir_) / ("fall_" + event.eventId + ".mp4")).string();
        }
        activeEvent_ = event;
        hasActiveEvent_ = true;
        return event;
    }
}
