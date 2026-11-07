#include "fall_monitoring/app/Application.hpp"
#include "fall_monitoring/application/vision/VisionPipeline.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"
#include "fall_monitoring/domain/vision/PersonTracker.hpp"
#include "fall_monitoring/infrastructure/camera/CameraCapture.hpp"
#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"
#include "fall_monitoring/infrastructure/logging/Logger.hpp"
#include "fall_monitoring/infrastructure/rknn/RknnPoseInferencer.hpp"

#include <opencv2/core.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
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

        const std::string modelPath = config.getString("model.rknn_model_path", "./models/yolov8n-pose-512-hybrid.rknn");
        const float confidenceThreshold = static_cast<float>(config.getDouble("model.confidence_threshold", 0.5));
        const float nmsThreshold = static_cast<float>(config.getDouble("model.nms_threshold", 0.45));

        try
        {
            // 组合根：Application 按依赖顺序创建队列、NPU、流水线与摄像头。
            concurrency::LatestFrameQueue<cv::Mat> frames(static_cast<std::size_t>(queueSize));
            infrastructure::rknn::RknnPoseInferencer inferencer(modelPath, confidenceThreshold, nmsThreshold);

            if (!inferencer.initialize())
            {
                SPDLOG_ERROR("[App] NPU 姿态推理初始化失败，停止启动");
                return 1;
            }

            domain::vision::PersonTrackerConfig trackerConfig;
            domain::vision::PersonTracker tracker(trackerConfig);
            SPDLOG_INFO("[Tracker] 初始化: IoU={:.2f}, 中心距离={:.2f}, 保留={}ms",
                        trackerConfig.iouThreshold,
                        trackerConfig.centerDistanceThreshold,
                        trackerConfig.maxMissingMs);

            application::vision::VisionPipeline pipeline(inferencer, tracker, frames);

            infrastructure::camera::CameraOptions cameraOptions;
            cameraOptions.deviceId = config.getInt("vision.camera_device_id", 0);
            if (argc == 2) cameraOptions.videoPath = argv[1];
            infrastructure::camera::CameraCapture camera(cameraOptions, frames);

            keepRunning = 1;
            std::signal(SIGINT, onStopSignal);
            std::signal(SIGTERM, onStopSignal);
            if (!camera.start())
            {
                SPDLOG_ERROR("[App] 摄像头采集启动失败");
                return 1;
            }

            // 原始检测在 VisionPipeline 内完成 Track ID 关联。
            pipeline.runUntil([&](){
                return keepRunning !=0 && camera.isRunning();
            });
            
            // 停止生产者并回收线程，再关闭队列；析构逆序释放 NPU。
            SPDLOG_INFO("[App] 开始关闭摄像头与视觉流水线");
            camera.stop();
            frames.close();
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
