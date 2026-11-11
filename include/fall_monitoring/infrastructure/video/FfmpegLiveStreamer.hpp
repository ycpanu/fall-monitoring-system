#pragma once

#include "fall_monitoring/application/ports/ILiveStream.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"

#include <opencv2/core/mat.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fall_monitoring::infrastructure::video
{
    struct LiveStreamOptions
    {
        int width = 640;
        int height = 480;
        int fps = 30;
        int bitrateKbps = 1000;
        std::size_t queueCapacity = 3;
        std::string ffmpegBinary = "ffmpeg";
        std::string encoder = "h264_rkmpp";
    };

    /**
     * @brief FFmpeg H.264 硬件编码 + RTMP 实时推流适配器。
     *
     * 采集线程调用 pushFrame() 时只进行有界队列入队，不参与编码与网络 I/O。
     * 每次启动创建独立队列和编码进程；停止先结束线程，再回收子进程。
     * 子进程通过 posix_spawnp 参数数组启动，不使用 shell 命令。
     */
    class FfmpegLiveStreamer final : public application::ports::ILiveStream
    {
    public:
        explicit FfmpegLiveStreamer(LiveStreamOptions options = {});
        ~FfmpegLiveStreamer() override;
        FfmpegLiveStreamer(const FfmpegLiveStreamer&) = delete;
        FfmpegLiveStreamer& operator=(const FfmpegLiveStreamer&) = delete;

        [[nodiscard]] bool start(const std::string& rtmpUrl) override;
        void stop() noexcept override;
        [[nodiscard]] bool isStreaming() const noexcept override;
        void pushFrame(const cv::Mat& frame) noexcept;

    private:
        using FrameQueue = concurrency::LatestFrameQueue<cv::Mat>;
        void streamLoop(std::shared_ptr<FrameQueue> queue, int writeFd, int childPid) noexcept;

        LiveStreamOptions options_;
        mutable std::mutex mutex_;
        std::shared_ptr<FrameQueue> queue_;
        std::thread worker_;
        std::string activeUrl_;
        std::atomic<bool> running_{false};
    };
}
