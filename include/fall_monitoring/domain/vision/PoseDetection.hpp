#pragma once

#include <vector>

namespace fall_monitoring::domain::vision
{
    /**
     * @brief 单个骨骼关键点，坐标位于原图像素空间
     */
    struct KeyPoint
    {
        int x = 0;
        int y = 0;
        float confidence = 0.0f;
    };

    /**
     * @brief 单个人体的姿态监测结果
     * 
     * classId 对姿态模型始终为 0(person)；跌倒动作类别由后续规则引擎判断。
     */
    struct DetectResult
    {
        int classId = 0;
        float confidence = 0.0f;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<KeyPoint> keyPoints;
    };
}