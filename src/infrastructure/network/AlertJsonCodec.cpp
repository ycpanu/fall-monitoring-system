#include "fall_monitoring/infrastructure/network/AlertJsonCodec.hpp"

#include <nlohmann/json.hpp>
#include <utility>

namespace fall_monitoring::infrastructure::network
{
    std::string encodeAlertJson(const domain::event::AlertEvent& event)
    {
        nlohmann::json data;
        data["person_track_id"] = event.personTrackId;
        data["trigger_x"] = event.triggerBoxX;
        data["trigger_y"] = event.triggerBoxY;
        if (!event.keyword.empty()) data["keyword"] = event.keyword;
        // 录像文件经 HTTP 独立上传。
        data["video_url"] = "";

        nlohmann::json sources = nlohmann::json::array();
        for (const auto source : event.source)
            sources.push_back(domain::event::toString(source));

        nlohmann::json message;
        message["event_id"] = event.eventId;
        message["revision"] = event.revision;
        message["device_id"] = event.deviceId;
        message["deployment_area"] = event.deploymentArea;
        message["event_type"] = domain::event::toString(event.eventType);
        message["timestamp"] = event.timestamp;
        message["status"] = domain::event::toString(event.status);
        message["sources"] = std::move(sources);
        message["data"] = std::move(data);
        return message.dump();
    }
}
