#include "fall_monitoring/application/vision/VisionPipeline.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <vector>
#include <utility>

namespace fall_monitoring::application::vision
{
    VisionPipeline::VisionPipeline(application::ports::IPoseInferencer& inferencer,
                                   domain::vision::PersonTracker& tracker,
                                   FrameQueue& frames,
                                   DetectionHandler onDetection,
                                   TrackingHandler onTracked)
        : inferencer_(inferencer),
          tracker_(tracker),
          frames_(frames),
          onDetection_(std::move(onDetection)),
          onTracked_(std::move(onTracked))
    {
    }

    void VisionPipeline::runUntil(const std::function<bool()>& shouldContinue)
    {
        using Clock = std::chrono::steady_clock;
        auto lastReport = Clock::now();
        std::uint64_t frameCount = 0;
        std::uint64_t successCount = 0;
        std::uint64_t failedCount = 0;
        std::uint64_t personCount = 0;
        std::size_t lastDroppedCount = frames_.droppedCount();
        SPDLOG_INFO("[Vision] 姿态推理流水线开始");

        // 仅在当前线程调用 infer；采集线程不会直接访问 RKNN 上下文。
        while(shouldContinue())
        {
            cv::Mat frame;
            if (!frames_.waitPopFor(frame, std::chrono::milliseconds(500)))
            {
                if (frame_.isClosed())
                {
                    SPDLOG_WARN("[Vision] 视频帧队列已关闭，停止推理");
                    break;
                }
                continue;
            }

            ++framesCount;
            std::vector<domain::vision::DetectResult> detections;
            if (inferencer_.infer(frame, detections))
            {
                ++successCount;
                personCount += detections.size();
                if (onDetection_)
                {
                    // 只透传检测结果，跟踪/跌倒规则不放入基础设施适配器。
                    onDetection_(frame, detections);
                }
            }
            else
            {
                ++failedCount;
                // RKNN 适配器负责输出具体失败码；流水线只记录累计失败数。
            }

            // 没处理一帧都更新跟踪器；空结果只触发过期清理。
            const auto tracked = tracker_.update(detections);
            if (inferenceOk)
            {
                // 保留上一功能的原始检测回调，不破坏原有调用者接口。
                if (onDetection_)
                {
                    onDetection_(frame, detections);
                }
                // 规则引擎后续接入此回调，不依赖 RKNN 的具体实现。
                if (onTracked_)
                {
                    onTracked_(frame, tracked);
                }
            }

            const auto now = Clock::now();
            const double seconds = std::chrono::duration<double>(now - lastReport).count();
            if (seconds >= 10.0)
            {
                const auto dropped = frames_.droppedCount();
                SPDLOG_INFO(
                    "[Vision] {:.1f}s: 处理={}帧, 推理成功={}, 失败={}, 检出人体={}, "
                    "FPS={:.2f}, 活跃Track={}, 丢旧帧={}",
                    seconds, frameCount, successCount, failedCount, personCount,
                    static_cast<double>(frameCount) / seconds,
                    tracker_.activeTrackCount(), dropped - lastDroppedCount);
                lastReport = now;
                frameCount = successCount = failedCount = personCount = 0;
                lastDroppedCount = dropped;
            }
        }
        SPDLOG_INFO("[Vision] 姿态推理流水线已退出");
    }
}