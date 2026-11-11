#include "fall_monitoring/application/vision/VisionPipeline.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace fall_monitoring::application::vision
{
    VisionPipeline::VisionPipeline(application::ports::IPoseInferencer& inferencer,
                                   domain::vision::PersonTracker& tracker,
                                   domain::vision::FallRuleEngine& ruleEngine,
                                   FrameQueue& frames,
                                   DetectionHandler onDetection,
                                   TrackingHandler onTracked,
                                   FallEventHandler onFallEvent)
        : inferencer_(inferencer),
          tracker_(tracker),
          ruleEngine_(ruleEngine),
          frames_(frames),
          onDetection_(std::move(onDetection)),
          onTracked_(std::move(onTracked)),
          onFallEvent_(std::move(onFallEvent))
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
        std::uint64_t fallCount = 0;
        std::size_t lastDroppedCount = frames_.droppedCount();

        SPDLOG_INFO("[Vision] 姿态推理、目标跟踪和跌倒规则流水线启动");
        // 主线程串行操作 RKNN、Tracker 和 RuleEngine，避免共享状态跨线程读写。
        while (shouldContinue())
        {
            cv::Mat frame;
            if (!frames_.waitPopFor(frame, std::chrono::milliseconds(500)))
            {
                if (frames_.isClosed())
                {
                    SPDLOG_INFO("[Vision] 帧队列已关闭，结束视觉处理");
                    break;
                }
                continue;
            }

            ++frameCount;
            std::vector<domain::vision::DetectResult> detections;
            const bool inferenceOk = inferencer_.infer(frame, detections);
            if (inferenceOk)
            {
                ++successCount;
                personCount += detections.size();
            }
            else
            {
                ++failedCount;
            }

            // 模型暂时推理失败，也使用空结果推进轨迹过期清理。
            const auto tracked = tracker_.update(detections);
            if (inferenceOk)
            {
                if (onDetection_) onDetection_(frame, detections);
                if (onTracked_) onTracked_(frame, tracked);

                // 规则状态机仅在推理成功的帧推进，与旧项目主循环一致。
                auto events = ruleEngine_.processFrame(tracked, frame.cols, frame.rows);
                fallCount += events.size();
                for (auto& event : events)
                {
                    if (onFallEvent_)
                    {
                        // 报警路由必须立即移交后续事件服务，不能在这里做网络/磁盘 I/O。
                        onFallEvent_(std::move(event));
                    }
                    else
                    {
                        SPDLOG_ERROR("[Vision] 检测到跌倒但未配置事件处理器：Track={}，事件未保存", event.personTrackId);
                    }
                }
            }

            const auto now = Clock::now();
            const double seconds = std::chrono::duration<double>(now - lastReport).count();
            if (seconds >= 10.0)
            {
                const auto dropped = frames_.droppedCount();
                SPDLOG_INFO(
                    "[Vision] {:.1f}s: 处理={}帧，成功={}，失败={}，人体={}，跌倒事件={}，"
                    "FPS={:.2f}，活跃Track={}，丢旧帧={}",
                    seconds, frameCount, successCount, failedCount, personCount, fallCount,
                    static_cast<double>(frameCount) / seconds,
                    tracker_.activeTrackCount(), dropped - lastDroppedCount);
                lastReport = now;
                frameCount = successCount = failedCount = personCount = fallCount = 0;
                lastDroppedCount = dropped;
            }
        }
        SPDLOG_INFO("[Vision] 视觉流水线已退出");
    }
} // namespace fall_monitoring::application::vision
