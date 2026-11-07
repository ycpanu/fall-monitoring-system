#pragma once

#include "fall_monitoring/application/ports/IPoseInferencer.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>
#include <opencv2/core/mat.hpp>

namespace fall_monitoring::application::vision
{
    /**
     * @brief 视频帧消费者与姿态推理编排器
     * 
     * 回调在推理线程同步执行。
     */
    class VisionPipeline final 
    {
    public:
        using FrameQueue = concurrency::LatestFrameQueue<cv::Mat>;
        using DetectionHandler = std::function<void(const cv::Mat&, const std::vector<domain::vision::DetectResult>&)>

        VisionPipeline(application::ports::IPoseInferencer& inferencer, FrameQueue& frame, DetectionHandler onDetection = {});

        /**
         * @brief 在当前线程消费并推理，在 shouldContinue() 返回 true 时持续运行，返回 false 退出。
         * @note 帧队列为非拥有引用，必须比此对象存活更久。
         */
        void runUntil(const std::function<bool()>& stopRequested);

    private:
        application::ports::IPoseInferencer& inferencer_;
        FrameQueue& frames_;
        DetectionHandler onDetection_;
    };
}