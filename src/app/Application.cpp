#include "fall_monitoring/app/Application.hpp"
#include "fall_monitoring/application/audio/AudioPipeline.hpp"
#include "fall_monitoring/application/commands/DeviceCommandService.hpp"
#include "fall_monitoring/application/event/EventService.hpp"
#include "fall_monitoring/application/network/AlertOutboxService.hpp"
#include "fall_monitoring/application/network/VideoUploadService.hpp"
#include "fall_monitoring/application/vision/VisionPipeline.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"
#include "fall_monitoring/domain/vision/FallRuleEngine.hpp"
#include "fall_monitoring/domain/vision/PersonTracker.hpp"
#include "fall_monitoring/infrastructure/audio/AlsaAudioCapture.hpp"
#include "fall_monitoring/infrastructure/audio/SherpaKeywordSpotter.hpp"
#include "fall_monitoring/infrastructure/camera/CameraCapture.hpp"
#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"
#include "fall_monitoring/infrastructure/logging/Logger.hpp"
#include "fall_monitoring/infrastructure/network/CurlVideoUploadTransport.hpp"
#include "fall_monitoring/infrastructure/network/PahoMqttClient.hpp"
#include "fall_monitoring/infrastructure/rknn/RknnPoseInferencer.hpp"
#include "fall_monitoring/infrastructure/scene/SafeZoneManager.hpp"
#include "fall_monitoring/infrastructure/storage/SqliteAlertRepository.hpp"
#include "fall_monitoring/infrastructure/system/SystemMetrics.hpp"
#include "fall_monitoring/infrastructure/video/FfmpegEventRecorder.hpp"
#include "fall_monitoring/infrastructure/video/FfmpegLiveStreamer.hpp"

#include <opencv2/core.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace
{
    // 信号回调仅修改 sig_atomic_t，不在信号上下文中执行 join、日志或 I/O。
    volatile std::sig_atomic_t keepRunning = 1;
    void onStopSignal(int) noexcept { keepRunning = 0; }
}

namespace fall_monitoring::app
{
    int Application::run(int argc, char* argv[])
    {
        infrastructure::config::ConfigManager config;
        if (!config.load("configs/config.json"))
        {
            std::cerr << "[APP] 加载 configs/config.json 失败\n";
            return 1;
        }

        infrastructure::logging::Logger logger;
        if (!logger.initialize(config.getString("system.log_file_path", "logs/gateway.log"), config.getString("system.log_level", "INFO")))
            return 1;

        if (argc > 2)
        {
            SPDLOG_ERROR("[APP] 参数错误，用法：{} [本地视频路径]", argv[0]);
            return 1;
        }
        const int frameQueueCapacity = config.getInt("vision.frame_queue_size", 3);
        const int alertQueueCapacity = config.getInt("network.alert_queue_size", 10);
        const int submitTimeoutMs = config.getInt("event.submit_timeout_ms", 50);
        const int uploadBatchSize = config.getInt("video_upload.batch_size", 100);
        const int liveQueueCapacity = config.getInt("live_stream.frame_queue_size", 3);
        const int maxRecordingTasks = config.getInt("video_recording.max_outstanding", 2);
        if (frameQueueCapacity <= 0 || alertQueueCapacity <= 0 || submitTimeoutMs < 0 || uploadBatchSize <= 0 || liveQueueCapacity <= 0 || maxRecordingTasks <= 0)
        {
            SPDLOG_ERROR("[APP] 队列容量、提交超时或上传批大小配置无效");
            return 1;
        }

        try
        {
            using domain::event::AlertEvent;
            const std::string deviceId = config.getString("device.device_id", "OrangePi_Gateway_001");
            const std::string deploymentArea = config.getString("device.deployment_area", "");
            SPDLOG_INFO("[APP] 启动：device_id={}, area={}", deviceId, deploymentArea);

            concurrency::LatestFrameQueue<cv::Mat> frames(static_cast<std::size_t>(frameQueueCapacity));

            // 场景查询归规则引擎使用，区域热更新由 MQTT 命令线程使用。
            infrastructure::scene::SafeZoneManager safeZones;
            const std::string scenePath = config.getString("scene.config_path", "configs/scene_config.json");
            if (!safeZones.load(scenePath))
                SPDLOG_WARN("[APP] 场景配置不可用：{}，继续监测但安全躺卧过滤可能不可用", scenePath);

            domain::vision::FallRuleConfig rules;
            rules.kptConfThreshold = static_cast<float>(config.getDouble("rule_engine.kpt_conf_threshold", 0.3));
            rules.fallAngleThreshold = static_cast<float>(config.getDouble("rule_engine.fall_angle_threshold", 60.0));
            rules.recoveryAngleThreshold = static_cast<float>(config.getDouble("rule_engine.recovery_angle_threshold", 35.0));
            rules.normalizedVelocityThreshold = static_cast<float>(config.getDouble("rule_engine.normalized_velocity_threshold", 0.6));
            rules.normalizedCenterVelocityThreshold = static_cast<float>(config.getDouble("rule_engine.normalized_center_velocity_threshold", 0.5));
            rules.suspectConfirmMs = config.getInt("rule_engine.suspect_confirm_ms", 2000);
            rules.staticLieConfirmMs = config.getInt("rule_engine.static_lie_confirm_ms", 5000);
            rules.fallEventWindowMs = config.getInt("rule_engine.fall_event_window_ms", 2500);
            rules.safeZoneOverlapThreshold = static_cast<float>(config.getDouble("rule_engine.safe_zone_overlap_threshold", 0.5));
            domain::vision::FallRuleEngine ruleEngine(rules, &safeZones);

            domain::vision::PersonTracker tracker;
            infrastructure::rknn::RknnPoseInferencer inferencer(
                config.getString("model.rknn_model_path", "./models/yolov8n-pose-512-hybrid.rknn"),
                static_cast<float>(config.getDouble("model.confidence_threshold", 0.5)),
                static_cast<float>(config.getDouble("model.nms_threshold", 0.45)));
            if (!inferencer.initialize())
            {
                SPDLOG_ERROR("[APP] RKNN 初始化失败，拒绝启动摄像头监测");
                return 1;
            }

            // SQLite 是报警真相来源；先初始化才能启动上层事件/网络服务。
            infrastructure::storage::SqliteAlertRepository alertRepository(config.getString("storage.sqlite_db_path", "fall_detection_gateway.db"));
            if (!alertRepository.initialize())
            {
                SPDLOG_CRITICAL("[APP] 本地 SQLite 初始化失败，拒绝启动报警系统");
                return 1;
            }

            infrastructure::network::MqttOptions mqttOptions;
            mqttOptions.broker = config.getString("network.mqtt_broker", "tcp://10.48.212.22:1883");
            mqttOptions.clientId = config.getString("network.mqtt_client_id", deviceId);
            mqttOptions.keepAliveSeconds = config.getInt("network.keep_alive_seconds", 20);
            infrastructure::network::PahoMqttClient mqttClient(std::move(mqttOptions));
            infrastructure::system::SystemMetrics systemMetrics("/");

            infrastructure::video::LiveStreamOptions liveOptions;
            liveOptions.width = config.getInt("live_stream.width", 640);
            liveOptions.height = config.getInt("live_stream.height", 480);
            liveOptions.fps = config.getInt("live_stream.fps", 30);
            liveOptions.bitrateKbps = config.getInt("live_stream.bitrate_kbps", 1000);
            liveOptions.queueCapacity = static_cast<std::size_t>(liveQueueCapacity);
            liveOptions.ffmpegBinary = config.getString("live_stream.ffmpeg_binary", "ffmpeg");
            liveOptions.encoder = config.getString("live_stream.encoder", "h264_rkmpp");
            infrastructure::video::FfmpegLiveStreamer liveStreamer(std::move(liveOptions));

            application::commands::DeviceCommandOptions commandOptions;
            commandOptions.deviceId = deviceId;
            commandOptions.sceneConfigPath = scenePath;
            application::commands::DeviceCommandService commands(std::move(commandOptions), safeZones, liveStreamer, mqttClient);
            if (!commands.start())
            {
                SPDLOG_ERROR("[APP] 无法启动云端指令服务");
                return 1;
            }

            application::network::AlertOutboxOptions outboxOptions;
            outboxOptions.deviceId = deviceId;
            outboxOptions.deploymentArea = deploymentArea;
            outboxOptions.alertTopic = config.getString("network.alert_topic", "fall_detection/alerts");
            outboxOptions.retrySeconds = config.getInt("network.alert_retry_seconds", 60);
            outboxOptions.reconnectSeconds = config.getInt("network.reconnect_seconds", 10);
            outboxOptions.heartbeatSeconds = config.getInt("network.heartbeat_seconds", 10);
            application::network::AlertOutboxService outbox(
                std::move(outboxOptions), alertRepository, mqttClient,
                [&systemMetrics]() -> std::optional<application::network::DeviceUsage>
                {
                    const auto metrics = systemMetrics.sample();
                    if (!metrics) return std::nullopt;
                    return application::network::DeviceUsage{
                        metrics->cpu, metrics->memory, metrics->storage};
                },
                [&commands](const std::string& topic, const std::string& payload)
                {
                    if (!commands.enqueue(topic, payload))
                        SPDLOG_ERROR("[Commands] 下行消息入队失败，topic={}", topic);
                });
            if (!outbox.start())
                SPDLOG_ERROR("[APP] MQTT Outbox 未能启动；继续保存 SQLite 本地报警");

            // HTTP 适配器与视频补传线程不读取摄像头，网络请求仅在后台上传线程执行。
            infrastructure::network::CurlVideoUploadOptions curlOptions;
            curlOptions.apiBaseUrl = config.getString("network.api_base_url", "http://10.48.212.22:8000");
            curlOptions.connectTimeoutSeconds = config.getInt("video_upload.connect_timeout_seconds", 5);
            curlOptions.timeoutSeconds = config.getInt("video_upload.timeout_seconds", 60);
            infrastructure::network::CurlVideoUploadTransport videoTransport(std::move(curlOptions));

            application::network::VideoUploadOptions uploadOptions;
            uploadOptions.retryInitialSeconds = config.getInt("video_upload.retry_initial_seconds", 5);
            uploadOptions.retryMaxSeconds = config.getInt("video_upload.retry_max_seconds", 300);
            uploadOptions.scanSeconds = config.getInt("video_upload.scan_seconds", 2);
            uploadOptions.batchSize = static_cast<std::size_t>(uploadBatchSize);
            application::network::VideoUploadService videoUploader(std::move(uploadOptions), alertRepository, videoTransport,
                [&outbox] { return outbox.isConnected(); });
            if (config.getBool("video_upload.enabled", true) && !videoUploader.start())
                SPDLOG_ERROR("[APP] 无法启动视频上传线程，录像仍保留 SQLite PENDING");

            // 录像器晚于 SQLite/上传器创建、早于 EventService 创建，析构逆序回收。
            infrastructure::video::EventVideoOptions recordingOptions;
            recordingOptions.preFrames = config.getInt("vision.video_cache_frames", 90);
            recordingOptions.postFrames = config.getInt("video_recording.post_frames", 90);
            recordingOptions.fps = config.getInt("vision.video_save_fps", 30);
            recordingOptions.width = config.getInt("video_recording.width", 640);
            recordingOptions.height = config.getInt("video_recording.height", 480);
            recordingOptions.bitrateKbps = config.getInt("video_recording.bitrate_kbps", 1000);
            recordingOptions.maxOutstanding = static_cast<std::size_t>(maxRecordingTasks);
            recordingOptions.ffmpegBinary = config.getString("video_recording.ffmpeg_binary", "ffmpeg");
            recordingOptions.encoder = config.getString("video_recording.encoder", "h264_rkmpp");
            infrastructure::video::FfmpegEventRecorder eventRecorder(
                std::move(recordingOptions),
                [&alertRepository, &videoUploader](const std::string& eventId,const std::string& path, bool success)
                {
                    const bool stored = success ? alertRepository.markVideoPending(eventId, path) : alertRepository.markVideoFailed(eventId);
                    if (!stored)
                        SPDLOG_CRITICAL("[VideoRecord] 无法记录录像状态：event_id={}, success={}", eventId, success);
                    else if (success)
                        videoUploader.notifyNewTask();
                });

            application::event::EventServiceOptions eventOptions;
            eventOptions.deviceId = deviceId;
            eventOptions.deploymentArea = deploymentArea;
            eventOptions.videoOutputDir = config.getString("vision.video_output_dir", "videos");
            eventOptions.fusionWindowMs = config.getInt("event.fusion_window_ms", 10000);
            eventOptions.queueCapacity = static_cast<std::size_t>(alertQueueCapacity);
            eventOptions.submitTimeout = std::chrono::milliseconds(submitTimeoutMs);
            application::event::EventService eventService(
                std::move(eventOptions),
                [&alertRepository, &outbox, &eventRecorder](const AlertEvent& prepared) -> bool
                {
                    const bool saved = alertRepository.saveAlert(prepared);
                    if (!saved) return false;
                    outbox.notifyPersisted();
                    if (prepared.captureVideo && !prepared.videoPath.empty() &&
                        !eventRecorder.requestRecording(prepared.eventId, prepared.videoPath))
                    {
                        SPDLOG_ERROR("[VideoRecord] 报警已落盘，但无法创建录像任务：{}", prepared.eventId);
                        if (!alertRepository.markVideoFailed(prepared.eventId))
                            SPDLOG_CRITICAL("[VideoRecord] 录像失败状态也无法入库：{}", prepared.eventId);
                    }
                    return true;
                });
            if (!eventService.start())
            {
                SPDLOG_CRITICAL("[APP] 事件服务未能启动");
                return 1;
            }

            // 音频故障降级为纯视觉，不影响报警数据库及相机功能。
            infrastructure::audio::AlsaAudioCapture audioCapture;
            infrastructure::audio::SherpaKeywordSpotter keywordSpotter;
            application::audio::AudioPipeline audioPipeline(audioCapture, keywordSpotter);
            if (config.getBool("audio.enabled", true) && config.getBool("kws.enabled", true))
            {
                infrastructure::audio::KeywordSpotterOptions kws;
                kws.encoderPath = config.getString("kws.encoder_path", kws.encoderPath);
                kws.decoderPath = config.getString("kws.decoder_path", kws.decoderPath);
                kws.joinerPath = config.getString("kws.joiner_path", kws.joinerPath);
                kws.tokensPath = config.getString("kws.tokens_path", kws.tokensPath);
                kws.keywordsPath = config.getString("kws.keywords_path", kws.keywordsPath);
                kws.sampleRate = config.getInt("kws.sample_rate", kws.sampleRate);
                kws.featureDim = config.getInt("kws.feature_dim", kws.featureDim);
                kws.numThreads = config.getInt("kws.num_threads", kws.numThreads);
                kws.maxActivePaths = config.getInt("kws.max_active_paths", kws.maxActivePaths);
                kws.numTrailingBlanks = config.getInt("kws.num_trailing_blanks", kws.numTrailingBlanks);
                kws.keywordsScore = static_cast<float>(config.getDouble("kws.keywords_score", kws.keywordsScore));
                kws.keywordsThreshold = static_cast<float>(config.getDouble("kws.keywords_threshold", kws.keywordsThreshold));

                application::ports::AudioCaptureOptions audioOptions;
                audioOptions.device = config.getString("audio.device", audioOptions.device);
                audioOptions.sampleRate = config.getInt("audio.sample_rate", audioOptions.sampleRate);
                audioOptions.channels = config.getInt("audio.channels", audioOptions.channels);
                audioOptions.framesPerChunk = config.getInt("audio.frames_per_chunk", audioOptions.framesPerChunk);
                if (audioOptions.sampleRate != kws.sampleRate)
                    SPDLOG_ERROR("[Audio] PCM 与 KWS 采样率不一致，语音功能降级");
                else if (!keywordSpotter.initialize(kws))
                    SPDLOG_ERROR("[Audio] KWS 模型初始化失败，继续纯视觉监测");
                else if (!audioPipeline.start(audioOptions, config.getInt("kws.cooldown_ms", 5000),
                    [&eventService](AlertEvent event)
                    {
                        const auto keyword = event.keyword;
                        if (!eventService.submit(std::move(event)))
                            SPDLOG_CRITICAL("[Audio] 求救事件入队失败，keyword={}", keyword);
                    }))
                    SPDLOG_ERROR("[Audio] ALSA 启动失败，继续纯视觉监测");
            }
            else
                SPDLOG_INFO("[Audio] 音频/关键词识别已在配置中禁用");

            application::vision::VisionPipeline pipeline(inferencer, tracker, ruleEngine, frames,
                {}, {}, [&eventService](AlertEvent event)
                {
                    const int trackId = event.personTrackId;
                    if (!eventService.submit(std::move(event)))
                        SPDLOG_CRITICAL("[Vision] 跌倒事件入队失败，track={}", trackId);
                });

            infrastructure::camera::CameraOptions cameraOptions;
            cameraOptions.deviceId = config.getInt("vision.camera_device_id", 0);
            if (argc == 2) cameraOptions.videoPath = argv[1];
            infrastructure::camera::CameraCapture camera(cameraOptions, frames,
                [&eventRecorder, &liveStreamer](const cv::Mat& frame)
                {
                    eventRecorder.observeFrame(frame);
                    liveStreamer.pushFrame(frame);
                });

            keepRunning = 1;
            std::signal(SIGINT, onStopSignal);
            std::signal(SIGTERM, onStopSignal);
            if (!camera.start())
            {
                SPDLOG_ERROR("[APP] 摄像头无法打开或采集线程启动失败");
                return 1;
            }

            pipeline.runUntil([&] { return keepRunning != 0 && camera.isRunning(); });

            // 先停止产生事件的线程，再排空事件和录像；最后停止上传/补传及设备指令。
            audioPipeline.stop();
            camera.stop();
            frames.close();
            eventService.stop();
            eventRecorder.stopAndDrain();
            videoUploader.stop();
            outbox.stop();
            commands.stop();
            liveStreamer.stop();
            SPDLOG_INFO("[APP] 安全退出，总采集帧数={}", camera.capturedCount());
            return 0;
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[APP] 未处理的运行异常：{}", error.what());
            return 1;
        }
    }
} // namespace fall_monitoring::app
