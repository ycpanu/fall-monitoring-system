#pragma once

#include "fall_monitoring/application/ports/IPoseInferencer.hpp"

#include <memory>
#include <string>

namespace fall_monitoring::infrastructure::rknn
{
    /**
     * @brief RK3566 NPU 姿态推理适配器
     * 
     * 使用 PImpl 隔离 rknn_api.h: 业务层不需要引用板端 RKNN SDK 头文件。
     * NPU 上下文、输出缓冲资源由 RAII 管理
     */
    class RknnPoseInferencer final : public application::ports::IPoseInferencer
    {
    public:
        RknnPoseInferencer(std::string modelPath, float confidenceThreshold, float nmsThreshold);
        ~RknnPoseInferencer() override;

        RknnPoseInferencer(const RknnPoseInferencer&) = delete;
        RknnPoseInferencer& operator=(const RknnPoseInferencer&) = delete;
        RknnPoseInferencer(RknnPoseInferencer&&) = delete;
        RknnPoseInferencer& operator=(RknnPoseInferencer&&) = delete;

        [[nodiscard]] bool initialize() override;
        [[nodiscard]] bool infer(const cv::Mat& frame, std::vector<domain::vision::DetectResult>& results) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}