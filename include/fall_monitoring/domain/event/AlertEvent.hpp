#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fall_monitoring::domain::event
{
    // JSON 序列化、MQTT 发送与 SQLite 入库不在领域层实现。
    enum class EventType { FALL, HELP_REQUEST };
    enum class EventSource { VISION, VOICE };
    enum class EventStatus { NEW, ACKNOWLEDGED, RESOLVED };

    struct AlertEvent
    {
        std::string eventId;          
        std::string deviceId;      
        std::string deploymentArea;  
        EventType eventType = EventType::FALL;
        std::vector<EventSource> source;
        EventStatus status = EventStatus::NEW;
        std::int64_t timestamp = 0; 
        int personTrackId = -1;
        int triggerBoxX = 0;
        int triggerBoxY = 0;
        std::string keyword;
        std::string videoPath;
        bool isFusionUpdate = false;
        bool captureVideo = false;
    };

    inline const char* toString(EventType value) noexcept
    {
        switch (value)
        {
            case EventType::FALL: return "FALL";
            case EventType::HELP_REQUEST: return "HELP_REQUEST";
        }
        return "UNKNOWN";
    }

    inline const char* toString(EventSource value) noexcept
    {
        switch (value)
        {
            case EventSource::VISION: return "VISION";
            case EventSource::VOICE: return "VOICE";
        }
        return "UNKNOWN";
    }

    inline const char* toString(EventStatus value) noexcept
    {
        switch (value)
        {
            case EventStatus::NEW: return "NEW";
            case EventStatus::ACKNOWLEDGED: return "ACKNOWLEDGED";
            case EventStatus::RESOLVED: return "RESOLVED";
        }
        return "UNKNOWN";
    }
} 