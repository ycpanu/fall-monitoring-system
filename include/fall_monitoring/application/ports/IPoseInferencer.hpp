#pragma once

#include "fall_monitoring/domain/vision/PoseDetection.hpp"

#include <opencv2/core/mat.hpp>
#include <vector>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 姿态推理端口（依赖倒置）。
     * 
     * 应用流水线只依赖此抽象，不依赖 RKNN C API 和平台 SDK。
     * initialize/infer 由同一条推理线程调用；不承诺并发推理安全。
     */
    class IPoseInferencer
    {
    public:
        virtual ~IPoseInferencer() = default;
        [[nodiscard]] virtual bool initialize() = 0;
        [[nodiscard]] virtual bool infer(const cv::Mat& frame, std::vector<domain::vision::DetectResult>& results) = 0;
    };
}