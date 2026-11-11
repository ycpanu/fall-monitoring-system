#pragma once

#include "fall_monitoring/domain/vision/SafeLieZone.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 场景配置写端口。控制指令不直接依赖 JSON、OpenCV 或文件系统。
     * @note 实现必须保证校验和写盘成功后，才对视觉线程公开新区域。
     */
    class ISafeZoneConfigurator
    {
    public:
        virtual ~ISafeZoneConfigurator() = default;
        [[nodiscard]] virtual bool replaceZonesAndSave(const std::vector<domain::vision::SafeLieZone>& zones, const std::string& configPath) = 0;
        [[nodiscard]] virtual std::size_t getZoneCount() const = 0;
    };
}
