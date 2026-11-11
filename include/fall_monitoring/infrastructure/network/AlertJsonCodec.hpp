#pragma once

#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <string>

namespace fall_monitoring::infrastructure::network
{
    /** @brief 云端接收服务的 MQTT 报警 JSON 字段协议。 */
    [[nodiscard]] std::string encodeAlertJson(const domain::event::AlertEvent& event);
}
