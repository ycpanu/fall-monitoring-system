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
     * @brief 采集源配置。只描述采集方式，不含 NPU、录像、网络配置。
     */
    struct CameraOptions
    {
        int deviceId = 0;               // 摄像头模式：/dev/video0 等设备索引
        std::string videoPath;          // 非空则使用本地视频文件，播放到末尾自动循环
        int cameraFrameIntervalMs = 33; // 保留旧工程的约 30FPS 软件限速策略
        int failureExitAfterMs = 10000; // 连续空帧达到阈值时退出采集线程，让监管器重建 V4L2 句柄。
    };

    /**
     * @brief 独立线程的视频采集适配器。
     *
     * 职责：OpenCV/V4L2 打开视频源 -> 读取画面 -> 送入 LatestFrameQueue。
     * 不负责：人体姿态推理、跌倒判定、录像和 RTMP 推流。
     *
     * 生命周期：调用者必须保证输出队列比 CameraCapture 存活更久；
     *           必须在队列销毁前调用 stop()。析构函数也会 stop()（RAII）。
     * 线程约束：start()/stop() 由同一个管理线程调用，不能并发调用。
     *           onFrame 回调执行在采集线程，不允许长期阻塞或保存帧引用。
     *
     * 适配器模式：隔离 OpenCV VideoCapture 的设备接口。
     * 依赖注入：队列及可选的帧观察回调在构造时传入。
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

        [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
        [[nodiscard]] std::uint64_t capturedCount() const noexcept { return capturedCount_.load(); }

    private:
        void captureLoop() noexcept;

        CameraOptions options_;
        FrameQueue& output_;             // 非拥有引用；由 Application 管理其生命周期
        FrameObserver onFrame_;          // 将来由应用层连接录像缓存/推流帧分发器
        cv::VideoCapture capture_;       // start() 打开，stop() 在 join 后释放
        std::thread worker_;
        std::atomic<bool> running_{false};
        std::atomic<std::uint64_t> capturedCount_{0};
    };
}
