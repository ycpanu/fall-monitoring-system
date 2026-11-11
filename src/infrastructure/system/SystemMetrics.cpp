#include "fall_monitoring/infrastructure/system/SystemMetrics.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace fall_monitoring::infrastructure::system
{
    namespace
    {
        int ratio(double used, double total)
        {
            if (total <= 0) return 0;
            return static_cast<int>(std::clamp(100.0 * used / total, 0.0, 100.0));
        }
    }

    SystemMetrics::SystemMetrics(std::string mountPoint) : mountPoint_(std::move(mountPoint)) {}

    std::optional<UsageMetrics> SystemMetrics::sample()
    {
        std::ifstream stat("/proc/stat");
        std::string label;
        unsigned long long user=0, nice=0, system=0, idle=0, iowait=0;
        unsigned long long irq=0, softirq=0, steal=0;
        if (!(stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || label != "cpu")
        {
            return std::nullopt;
        }
        const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
        const auto idleTotal = idle + iowait;

        std::ifstream mem("/proc/meminfo");
        unsigned long long memTotal = 0, memAvailable = 0;
        std::string key, unit;
        unsigned long long value = 0;
        while (mem >> key >> value >> unit)
        {
            if (key == "MemTotal:") memTotal = value;
            else if (key == "MemAvailable:") memAvailable = value;
            if (memTotal > 0 && memAvailable > 0) break;
        }
        if (memTotal == 0 || memAvailable > memTotal) return std::nullopt;

        struct statvfs disk{};
        if (::statvfs(mountPoint_.c_str(), &disk) != 0 || disk.f_blocks == 0) return std::nullopt;
        UsageMetrics metrics;
        if (sampledCpu_ && total > prevTotal_)
        {
            const auto dt = total - prevTotal_;
            const auto di = idleTotal >= prevIdle_ ? idleTotal - prevIdle_ : 0;
            metrics.cpu = ratio(static_cast<double>(dt - std::min(dt, di)), static_cast<double>(dt));
        }
        prevTotal_ = total;
        prevIdle_ = idleTotal;
        sampledCpu_ = true;
        metrics.memory = ratio(static_cast<double>(memTotal - memAvailable), static_cast<double>(memTotal));
        metrics.storage = ratio(static_cast<double>(disk.f_blocks - disk.f_bfree), static_cast<double>(disk.f_blocks));
        return metrics;
    }
}
