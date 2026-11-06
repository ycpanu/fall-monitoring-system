#pragma once

#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace fall_monitoring::infrastructure::camera
{
    /**
     * @brief 采集配置，只描述采集方式
     */
    struct CameraOptions
    {
        int deviceId = 0;
        std::string videoPath;
        int cameraFrameIntervalMs = 33;
    };

    /**
     * @brief 独立线程的视频采集适配器
     * OpenCV/V4L2 打开视频源 -> 读取画面 -> 送入 LatestFrameQueue。
     */
    class CameraCapture final
    {
    public:
        using FrameQueue = concurrency::LatestFrameQueue<cv::Mat>;
        using FrameObserver = std::function<void(const cv::Mat&)>;

        CameraCapture(CameraOptions options, FrameQueue& output, FrameObserver onFrame = {});
        ~CameraCapture();

        CameraCapture(const CameraCapture&) = delete;
        CameraCapture& operator=(const CameraCapture&) = delete;
        CameraCapture(CameraCapture&&) = delete;
        CameraCapture& operator=(CameraCapture&&) = delete;

        [[nodiscard]] bool start();
        void stop() noexcept;

        [[nodiscard]] bool isRunning() const noexcept
        {
            return running_.load();
        }

        [[nodiscard]] std::unit64_t capturedCount() const noexcept
        {
            return capturedCount_.load();
        }

    private:
        void captureLoop() noexcept;

        CameraOptions options_;
        FrameQueue& output_;
        FrameObserver onFrame_; //由应用层连接录像缓存/推流帧分发器
        cv::VideoCapture capture_;
        std::thread worker_;
        std::atomic<bool> running_{false};
        std::atomic<std::uint64_t> capturedCount_{0};
    };
}