#include "fall_monitoring/app/Application.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"
#include "fall_monitoring/infrastructure/camera/CameraCapture.hpp"
#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"
#include "fall_monitoring/infrastructure/logging/Logger.hpp"

#include <opencv2/core.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>

namespace
{
    // 信号处理函数中只修改 sig_atomic_t，禁止在这里执行日志/I/O/join
    volatile std::sig_atomic_t keepRunning = 1;
    void onStopSignal(int) noexcept
    {
        keepRunning = 0;
    }
}

namespace fall_monitoring::app
{
    int Application::run(int argc, char* argv[])
    {
        infrastructure::config::ConfigManager config;
        if (!config.load("configs/config.json"))
        {
            std::cerr << "[APP] 配置加载失败\n";
            return 1;
        }

        infrastructure::logging::Logger logger;
        if (!logger.initialize(config.getString("system.log_file_path", "logs/gateway.log"), config.getString("system.log_level", "INFO")))
        {
            return 1;
        }

        SPDLOG_INFO("[APP] 启动：device_id={}, area={}", config.getString("device.device_id", "unknown"), config.getString("device.deployment_area", "unknown"));

        if (argc > 2)
        {
            SPDLOG_ERROR("[APP] 参数错误，用法：{}[本地视频路径]",,argv[0]);
            return 1;
        }

        const int queueSize = config.getInt("vision.frame_queue_size", 3);
        if (queueSize <= 0)
        {
            SPDLOG_ERROR("[App] vision.frame_queue_size 必须大于 0");
            return 1;
        }

        try
        {
            // Application 创建队列和设备适配器，按声明顺序保证生命周期
            concurrency::LatestFrameQueue<cv::Mat> frames(static_cast<std::size_t>(queueSize));
            infrastructure::camera::CameraOptions cameraOptions;
            cameraOptions.deviceId = config.getInt("vision.camera_device_id", 0);

            if (argc == 2)
            {
                cameraOptions.videoPath = argv[1];
            }

            infrastructure::camera::CameraCapture camera(cameraOptions, frames);

            // 在启动采集线程前注册退出信号
            keepRunning = 1;
            std::signal(SIGINT, onStopSignal);
            std::signal(SIGTERM, onStopSignal);
            if (!camera.start())
            {
                SPDLOG_ERROR("[App] 视频采集模块启动失败");
                return 1;
            }

            using clock = std::chrono::steady_clock;
            auto lastReport = clock::now();
            std::uint64_t consumed = 0;
            std::uint64_t lastConsumed = 0;
            std::uint64_t lastCaptured = 0;
            std::size_t lastDropped = 0;
            while (keepRunning != 0 && camera.isRunning())
            {
                cv::Mat frame;
                if (frames.waitPopFor(frame, std::chrono::milliseconds(500)))
                {
                    ++consumed;
                }
                const auto now = clock::now();
                const auto elapsed = std::chrono::duration<double>(now - lastReport).count();
                if (elapsed >= 10.0)
                {
                    const auto captured = camera.capturedCount();
                    const auto dropped = frames.droppedCount();
                    SPDLOG_INFO("[Camera] 近 {:.1f}s 采集={}帧, 消费={}帧, 丢旧帧={}, 队列剩余={}",
                                elapsed, captured - lastCaptured, consumed - lastConsumed,
                                dropped - lastDropped, frames.size());
                    lastReport = now;
                    lastCaptured = captured;
                    lastConsumed = consumed;
                    lastDropped = dropped;
                }
            }

            SPDLOG_INFO("[App] 收到退出请求或采集已结束，开始关闭");
            camera.stop();      // 先回收生产者，避免其访问已经销毁的队列。
            frames.close();     // 唤醒潜在消费者；不静默覆盖报警队列。
            SPDLOG_INFO("[App] 安全退出，总采集帧数={}", camera.capturedCount());
            return 0;
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[App] 运行异常：{}", e.what());
            return 1;
        }
    }
}