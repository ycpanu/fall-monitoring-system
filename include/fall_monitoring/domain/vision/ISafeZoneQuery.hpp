#pragma once

namespace fall_monitoring::domain::vision
{
    /**
     * @brief 安全躺卧区域查询端口（只读）。
     * 领域规则不依赖 JSON/OpenCV/MQTT；下个安全区域功能可直接实现此接口。
     * 返回值的坐标单位：点为 0~1 归一化坐标，框为原图像素。
     */
    class ISafeZoneQuery
    {
    public:
        virtual ~ISafeZoneQuery() = default;

        [[nodiscard]] virtual bool isPointInSafeZone(float normalizedX, float normalizedY) const = 0;

        [[nodiscard]] virtual float calculateBoxOverlapRatio(int boxX, int boxY, int boxWidth, int boxHeight, int frameWidth, int frameHeight) const = 0;
    };
}