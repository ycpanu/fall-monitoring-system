#pragma once

#include <string>
#include <vector>

namespace fall_monitoring::domain::vision
{
    /**
     * @brief 摄像头画面中的归一化坐标，x/y 范围为 [0, 1]。
     *
     * 使用归一化坐标，使区域配置不依赖摄像头输出分辨率。
     */
    struct NormalizedPoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    /**
     * @brief 允许安全躺卧的多边形区域。
     *
     * points 按多边形边界顺序排列，至少需要三个点。
     */
    struct SafeLieZone
    {
        std::string name;
        std::vector<NormalizedPoint> points;
    };
} // namespace fall_monitoring::domain::vision
