#pragma once

#include <optional>
#include <string>

namespace fall_monitoring::infrastructure::system
{
    struct UsageMetrics
    {
        int cpu = 0;
        int memory = 0;
        int storage = 0;
    };

    /** @brief Linux /proc 与 statvfs 资源采样，仅用于设备 MQTT 状态心跳。 */
    class SystemMetrics final
    {
    public:
        explicit SystemMetrics(std::string mountPoint = "/");
        [[nodiscard]] std::optional<UsageMetrics> sample();

    private:
        std::string mountPoint_;
        unsigned long long prevTotal_ = 0;
        unsigned long long prevIdle_ = 0;
        bool sampledCpu_ = false;
    };
}
