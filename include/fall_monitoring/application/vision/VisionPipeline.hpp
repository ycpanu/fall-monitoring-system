#pragma once

#include "fall_monitoring/application/ports/IPoseInferencer.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"
#include "fall_monitoring/domain/vision/FallRuleEngine.hpp"
#include "fall_monitoring/domain/vision/PersonTracker.hpp"

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
        using DetectionHandler = std::function<void(const cv::Mat&, const std::vector<domain::vision::DetectResult>&)>;
        using TrackingHandler = std::function<void(const cv::Mat&, const std::vector<domain::vision::TrackedPerson>&)>;
        using FallEventHandler = std::function<void(domain::event::AlertEvent)>;

        VisionPipeline(application::ports::IPoseInferencer& inferencer,
                       domain::vision::PersonTracker& tracker,
                       domain::vision::FallRuleEngine& ruleEngine,
                       FrameQueue& frames,
                       DetectionHandler onDetection = {},
                       TrackingHandler onTracked = {},
                       FallEventHandler onFallEvent = {});

        /**
         * @brief 在调用者线程持续消费帧，直到停止请求或队列关闭。
         * @note 两种回调均在本线程同步执行，不可长期保存 frame 引用。
         */
        void runUntil(const std::function<bool()>& shouldContinue);

    private:
        application::ports::IPoseInferencer& inferencer_;
        domain::vision::PersonTracker& tracker_;
        domain::vision::FallRuleEngine& ruleEngine_;
        FrameQueue& frames_;
        DetectionHandler onDetection_;
        TrackingHandler onTracked_;
        FallEventHandler onFallEvent_;
    };
}