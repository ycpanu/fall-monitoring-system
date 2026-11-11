#pragma once

#include "fall_monitoring/application/ports/IEventVideoRecorder.hpp"

#include <opencv2/core/mat.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fall_monitoring::infrastructure::video
{
    struct EventVideoOptions
    {
        int width = 640;
        int height = 480;
        int fps = 30;
        int bitrateKbps = 1000;
        int preFrames = 90;
        int postFrames = 90;
        std::size_t maxOutstanding = 2;
        std::string ffmpegBinary = "ffmpeg";
        std::string encoder = "h264_rkmpp";
    };

    /**
     * @brief FFmpeg 事件录像：环形预录 + 事后帧 + 单线程顺序编码。
     *
     * 采集线程只缓存目标分辨率 BGR 帧，不负责编码。
     * EventService 在报警成功落库后调用 requestRecording()。
     * CompletionHandler 在编码线程调用：数据库仓储必须比本对象存活更久。
     */
    class FfmpegEventRecorder final : public application::ports::IEventVideoRecorder
    {
    public:
        using CompletionHandler = std::function<void(const std::string& eventId, const std::string& outputPath, bool success)>;

        FfmpegEventRecorder(EventVideoOptions options, CompletionHandler onCompleted);
        ~FfmpegEventRecorder() override;

        FfmpegEventRecorder(const FfmpegEventRecorder&) = delete;
        FfmpegEventRecorder& operator=(const FfmpegEventRecorder&) = delete;

        void observeFrame(const cv::Mat& frame) noexcept override;
        [[nodiscard]] bool requestRecording(const std::string& eventId, const std::string& outputPath) override;
        void stopAndDrain() noexcept override;

    private:
        struct Task
        {
            std::string eventId;
            std::string outputPath;
            std::deque<cv::Mat> frames;
        };

        struct Pending
        {
            Task task;
            int remaining = 0;
        };

        void encodingLoop() noexcept;
        [[nodiscard]] bool encodeToMp4(const Task& task) const;
        void reportCompletion(const Task& task, bool success) noexcept;

        EventVideoOptions options_;
        CompletionHandler onCompleted_;
        std::mutex mutex_;
        std::condition_variable available_;
        std::deque<cv::Mat> history_;
        std::vector<Pending> pending_;
        std::deque<Task> ready_;
        std::unordered_set<std::string> outstandingIds_;
        std::unordered_set<std::string> completedIds_;
        std::deque<std::string> completedOrder_;
        std::thread worker_;
        bool closing_ = false;
    };
}
