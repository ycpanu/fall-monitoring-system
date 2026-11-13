#include "fall_monitoring/app/Application.hpp"

#include "fall_monitoring/application/alarm/LocalAlarmService.hpp"
#include "fall_monitoring/application/audio/AudioPipeline.hpp"
#include "fall_monitoring/application/commands/DeviceCommandService.hpp"
#include "fall_monitoring/application/event/EventService.hpp"
#include "fall_monitoring/application/network/AlertOutboxService.hpp"
#include "fall_monitoring/application/network/VideoUploadService.hpp"
#include "fall_monitoring/application/runtime/RuntimeSupervisor.hpp"
#include "fall_monitoring/application/vision/VisionPipeline.hpp"
#include "fall_monitoring/concurrency/LatestFrameQueue.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"
#include "fall_monitoring/domain/vision/FallRuleEngine.hpp"
#include "fall_monitoring/domain/vision/PersonTracker.hpp"
#include "fall_monitoring/infrastructure/audio/AlsaAudioCapture.hpp"
#include "fall_monitoring/infrastructure/audio/SherpaKeywordSpotter.hpp"
#include "fall_monitoring/infrastructure/camera/CameraCapture.hpp"
#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"
#include "fall_monitoring/infrastructure/hardware/SysfsBuzzer.hpp"
#include "fall_monitoring/infrastructure/logging/Logger.hpp"
#include "fall_monitoring/infrastructure/network/CurlVideoUploadTransport.hpp"
#include "fall_monitoring/infrastructure/network/PahoMqttClient.hpp"
#include "fall_monitoring/infrastructure/rknn/RknnPoseInferencer.hpp"
#include "fall_monitoring/infrastructure/scene/SafeZoneManager.hpp"
#include "fall_monitoring/infrastructure/storage/SqliteAlertRepository.hpp"
#include "fall_monitoring/infrastructure/system/SystemdNotifier.hpp"
#include "fall_monitoring/infrastructure/system/SystemMetrics.hpp"
#include "fall_monitoring/infrastructure/video/FfmpegEventRecorder.hpp"
#include "fall_monitoring/infrastructure/video/FfmpegLiveStreamer.hpp"

#include <opencv2/core.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace fall_monitoring::app
{
    using domain::event::AlertEvent;

    /**
     * 按依赖顺序持有所有对象。各服务只保存对前序对象的非拥有引用。
     * Application::stop() 先停止全部生产者/消费者，再通过 impl_.reset() 逆序析构。
     */
    struct Application::Impl final
    {
        infrastructure::config::ConfigManager config;
        infrastructure::logging::Logger logger;

        std::string deviceId;
        std::string deploymentArea;
        std::string scenePath;
        std::string videoInputPath;
        int alertDurationMs = 3000;
        int audioCooldownMs = 5000;
        bool localAlarmReady = false;
        bool audioExpected = false;
        application::runtime::RuntimeSupervisorOptions runtimeOptions;
        bool startedOnce = false;
        bool started = false;
        bool stopped = false;
        std::chrono::steady_clock::time_point nextServiceRestart{};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> visionExited{false};
        std::atomic<bool> visionFailed{false};
        std::thread visionWorker;

        std::unique_ptr<concurrency::LatestFrameQueue<cv::Mat>> frames;
        std::unique_ptr<infrastructure::scene::SafeZoneManager> safeZones;
        std::unique_ptr<domain::vision::FallRuleEngine> ruleEngine;
        std::unique_ptr<domain::vision::PersonTracker> tracker;
        std::unique_ptr<infrastructure::rknn::RknnPoseInferencer> inferencer;
        std::unique_ptr<infrastructure::storage::SqliteAlertRepository> alertRepository;
        std::unique_ptr<infrastructure::hardware::SysfsBuzzer> buzzer;
        std::unique_ptr<application::alarm::LocalAlarmService> localAlarm;
        std::unique_ptr<infrastructure::network::PahoMqttClient> mqttClient;
        std::unique_ptr<infrastructure::system::SystemMetrics> systemMetrics;
        std::unique_ptr<infrastructure::video::FfmpegLiveStreamer> liveStreamer;
        std::unique_ptr<application::commands::DeviceCommandService> commands;
        std::unique_ptr<application::network::AlertOutboxService> outbox;
        std::unique_ptr<infrastructure::network::CurlVideoUploadTransport> videoTransport;
        std::unique_ptr<application::network::VideoUploadService> videoUploader;
        std::unique_ptr<infrastructure::video::FfmpegEventRecorder> eventRecorder;
        std::unique_ptr<application::event::EventService> eventService;
        std::unique_ptr<infrastructure::audio::AlsaAudioCapture> audioCapture;
        std::unique_ptr<infrastructure::audio::SherpaKeywordSpotter> keywordSpotter;
        std::unique_ptr<application::audio::AudioPipeline> audioPipeline;
        std::unique_ptr<application::vision::VisionPipeline> visionPipeline;
        std::unique_ptr<infrastructure::camera::CameraCapture> camera;
        std::unique_ptr<infrastructure::system::SystemdNotifier> systemdNotifier;
        std::unique_ptr<application::runtime::RuntimeSupervisor> supervisor;

        [[nodiscard]] bool initialize(int argc, char* argv[])
        {
            if (argc < 1 || argc > 2)
            {
                std::cerr << "用法：fall_monitoring_gateway [本地视频文件]\n";
                return false;
            }
            if (!config.load("configs/config.json"))
            {
                std::cerr << "[APP] 无法加载 configs/config.json\n";
                return false;
            }
            if (!logger.initialize(config.getString("system.log_file_path", "logs/gateway.log"), config.getString("system.log_level", "INFO")))
                return false;

            const int frameCapacity = config.getInt("vision.frame_queue_size", 3);
            const int alertCapacity = config.getInt("network.alert_queue_size", 10);
            const int eventSubmitTimeout = config.getInt("event.submit_timeout_ms", 50);
            const int videoBatch = config.getInt("video_upload.batch_size", 100);
            const int liveCapacity = config.getInt("live_stream.frame_queue_size", 3);
            const int maxRecordings = config.getInt("video_recording.max_outstanding", 2);
            alertDurationMs = config.getInt("hardware.alarm_duration_ms", 3000);
            audioCooldownMs = config.getInt("kws.cooldown_ms", 5000);
            if (frameCapacity <= 0 || alertCapacity <= 0 || eventSubmitTimeout < 0 ||
                videoBatch <= 0 || liveCapacity <= 0 || maxRecordings <= 0 ||
                alertDurationMs <= 0 || alertDurationMs > 60000 || audioCooldownMs < 0)
            {
                SPDLOG_ERROR("[APP] 事件/视频队列、蜂鸣时长或语音冷却配置非法");
                return false;
            }

            deviceId = config.getString("device.device_id", "OrangePi_Gateway_001");
            deploymentArea = config.getString("device.deployment_area", "");
            scenePath = config.getString("scene.config_path", "configs/scene_config.json");
            videoInputPath = argc == 2 ? argv[1] : "";
            audioExpected = config.getBool("audio.enabled", true) && config.getBool("kws.enabled", true);

            frames = std::make_unique<concurrency::LatestFrameQueue<cv::Mat>>(
                static_cast<std::size_t>(frameCapacity));
            safeZones = std::make_unique<infrastructure::scene::SafeZoneManager>();
            if (!safeZones->load(scenePath))
                SPDLOG_WARN("[APP] 安全区域配置不可用：{}；继续监测", scenePath);

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
            ruleEngine = std::make_unique<domain::vision::FallRuleEngine>(rules, safeZones.get());
            tracker = std::make_unique<domain::vision::PersonTracker>();
            inferencer = std::make_unique<infrastructure::rknn::RknnPoseInferencer>(
                config.getString("model.rknn_model_path", "./models/yolov8n-pose-512-hybrid.rknn"),
                static_cast<float>(config.getDouble("model.confidence_threshold", 0.5)),
                static_cast<float>(config.getDouble("model.nms_threshold", 0.45)));
            if (!inferencer->initialize())
            {
                SPDLOG_CRITICAL("[APP] RKNN 姿态推理初始化失败");
                return false;
            }

            alertRepository = std::make_unique<infrastructure::storage::SqliteAlertRepository>(
                config.getString("storage.sqlite_db_path", "fall_detection_gateway.db"));
            if (!alertRepository->initialize())
            {
                SPDLOG_CRITICAL("[APP] SQLite 报警仓储初始化失败");
                return false;
            }

            infrastructure::hardware::SysfsBuzzerOptions buzzerOptions;
            buzzerOptions.gpioPin = config.getInt("hardware.buzzer_gpio_pin", 138);
            buzzerOptions.activeHigh = config.getBool("hardware.buzzer_active_high", true);
            buzzer = std::make_unique<infrastructure::hardware::SysfsBuzzer>(buzzerOptions);
            if (config.getBool("hardware.buzzer_enabled", true))
            {
                localAlarmReady = buzzer->initialize();
                if (!localAlarmReady)
                    SPDLOG_ERROR("[APP] GPIO 蜂鸣器不可用，保持视觉/语音及网络报警");
            }
            localAlarm = std::make_unique<application::alarm::LocalAlarmService>(*buzzer, alertDurationMs);

            infrastructure::network::MqttOptions mqttOptions;
            mqttOptions.broker = config.getString("network.mqtt_broker", "tcp://10.48.212.22:1883");
            mqttOptions.clientId = config.getString("network.mqtt_client_id", deviceId);
            mqttOptions.keepAliveSeconds = config.getInt("network.keep_alive_seconds", 20);
            mqttClient = std::make_unique<infrastructure::network::PahoMqttClient>(std::move(mqttOptions));
            systemMetrics = std::make_unique<infrastructure::system::SystemMetrics>("/");

            infrastructure::video::LiveStreamOptions liveOptions;
            liveOptions.width = config.getInt("live_stream.width", 640);
            liveOptions.height = config.getInt("live_stream.height", 480);
            liveOptions.fps = config.getInt("live_stream.fps", 30);
            liveOptions.bitrateKbps = config.getInt("live_stream.bitrate_kbps", 1000);
            liveOptions.queueCapacity = static_cast<std::size_t>(liveCapacity);
            liveOptions.ffmpegBinary = config.getString("live_stream.ffmpeg_binary", "ffmpeg");
            liveOptions.encoder = config.getString("live_stream.encoder", "h264_rkmpp");
            liveStreamer = std::make_unique<infrastructure::video::FfmpegLiveStreamer>(std::move(liveOptions));

            application::commands::DeviceCommandOptions commandOptions;
            commandOptions.deviceId = deviceId;
            commandOptions.sceneConfigPath = scenePath;
            commands = std::make_unique<application::commands::DeviceCommandService>(std::move(commandOptions), *safeZones, *liveStreamer, *mqttClient);

            application::network::AlertOutboxOptions outboxOptions;
            outboxOptions.deviceId = deviceId;
            outboxOptions.deploymentArea = deploymentArea;
            outboxOptions.alertTopic = config.getString("network.alert_topic", "fall_detection/alerts");
            outboxOptions.retrySeconds = config.getInt("network.alert_retry_seconds", 60);
            outboxOptions.reconnectSeconds = config.getInt("network.reconnect_seconds", 10);
            outboxOptions.heartbeatSeconds = config.getInt("network.heartbeat_seconds", 10);
            outbox = std::make_unique<application::network::AlertOutboxService>(std::move(outboxOptions), *alertRepository, *mqttClient,
                [this]() -> std::optional<application::network::DeviceUsage>
                {
                    const auto metrics = systemMetrics->sample();
                    if (!metrics) return std::nullopt;
                    return application::network::DeviceUsage{metrics->cpu, metrics->memory, metrics->storage};
                },
                [this](const std::string& topic, const std::string& payload)
                {
                    if (!commands->enqueue(topic, payload))
                        SPDLOG_ERROR("[Commands] 指令入队失败，topic={}", topic);
                });

            infrastructure::network::CurlVideoUploadOptions curlOptions;
            curlOptions.apiBaseUrl = config.getString("network.api_base_url", "http://10.48.212.22:8000");
            curlOptions.connectTimeoutSeconds = config.getInt("video_upload.connect_timeout_seconds", 5);
            curlOptions.timeoutSeconds = config.getInt("video_upload.timeout_seconds", 60);
            videoTransport = std::make_unique<infrastructure::network::CurlVideoUploadTransport>(std::move(curlOptions));

            application::network::VideoUploadOptions uploadOptions;
            uploadOptions.retryInitialSeconds = config.getInt("video_upload.retry_initial_seconds", 5);
            uploadOptions.retryMaxSeconds = config.getInt("video_upload.retry_max_seconds", 300);
            uploadOptions.scanSeconds = config.getInt("video_upload.scan_seconds", 2);
            uploadOptions.batchSize = static_cast<std::size_t>(videoBatch);
            videoUploader = std::make_unique<application::network::VideoUploadService>(
                std::move(uploadOptions), *alertRepository, *videoTransport,
                // HTTP 与 MQTT 是独立链路；是否可上传由实际 HTTP 结果和数据库 ACK 判定。
                [] { return true; });

            infrastructure::video::EventVideoOptions recordingOptions;
            recordingOptions.preFrames = config.getInt("vision.video_cache_frames", 90);
            recordingOptions.postFrames = config.getInt("video_recording.post_frames", 90);
            recordingOptions.fps = config.getInt("vision.video_save_fps", 30);
            recordingOptions.width = config.getInt("video_recording.width", 640);
            recordingOptions.height = config.getInt("video_recording.height", 480);
            recordingOptions.bitrateKbps = config.getInt("video_recording.bitrate_kbps", 1000);
            recordingOptions.maxOutstanding = static_cast<std::size_t>(maxRecordings);
            recordingOptions.ffmpegBinary = config.getString("video_recording.ffmpeg_binary", "ffmpeg");
            recordingOptions.encoder = config.getString("video_recording.encoder", "h264_rkmpp");
            eventRecorder = std::make_unique<infrastructure::video::FfmpegEventRecorder>(
                std::move(recordingOptions),
                [this](const std::string& eventId, const std::string& path, bool success)
                {
                    const bool saved = success
                        ? alertRepository->markVideoPending(eventId, path)
                        : alertRepository->markVideoFailed(eventId);
                    if (!saved)
                        SPDLOG_CRITICAL("[VideoRecord] 录像状态无法入库：event_id={}, success={}", eventId, success);
                    else if (success)
                        videoUploader->notifyNewTask();
                });

            application::event::EventServiceOptions eventOptions;
            eventOptions.deviceId = deviceId;
            eventOptions.deploymentArea = deploymentArea;
            eventOptions.videoOutputDir = config.getString("vision.video_output_dir", "videos");
            eventOptions.fusionWindowMs = config.getInt("event.fusion_window_ms", 10000);
            eventOptions.queueCapacity = static_cast<std::size_t>(alertCapacity);
            eventOptions.submitTimeout = std::chrono::milliseconds(eventSubmitTimeout);
            eventService = std::make_unique<application::event::EventService>(
                std::move(eventOptions), [this](const AlertEvent& event) -> bool
                {
                    // 本地报警先于 SQLite；事件重试与多模态融合按 event_id 去重。
                    if (localAlarmReady && !event.localAlarmAlreadyRequested)
                        localAlarm->onPreparedEvent(event);
                    if (!alertRepository->saveAlert(event)) return false;
                    outbox->notifyPersisted();
                    if (event.captureVideo && !event.videoPath.empty() &&
                        !eventRecorder->requestRecording(event.eventId, event.videoPath))
                    {
                        SPDLOG_ERROR("[VideoRecord] 无法接收录像任务：{}", event.eventId);
                        if (!alertRepository->markVideoFailed(event.eventId))
                            SPDLOG_CRITICAL("[VideoRecord] 无法记录录像失败状态：{}", event.eventId);
                    }
                    return true;
                }, *alertRepository,
                [this]() -> bool
                {
                    return localAlarmReady && buzzer->trigger(std::chrono::milliseconds(alertDurationMs));
                });

            audioCapture = std::make_unique<infrastructure::audio::AlsaAudioCapture>();
            keywordSpotter = std::make_unique<infrastructure::audio::SherpaKeywordSpotter>();
            audioPipeline = std::make_unique<application::audio::AudioPipeline>(*audioCapture, *keywordSpotter);
            visionPipeline = std::make_unique<application::vision::VisionPipeline>(
                *inferencer, *tracker, *ruleEngine, *frames,
                application::vision::VisionPipeline::DetectionHandler{},
                application::vision::VisionPipeline::TrackingHandler{},
                [this](AlertEvent event)
                {
                    const int trackId = event.personTrackId;
                    if (!eventService->submit(std::move(event)))
                    {
                        if (localAlarmReady)
                            (void)buzzer->trigger(std::chrono::milliseconds(alertDurationMs));
                        SPDLOG_CRITICAL("[Vision] 跌倒事件入队及溢出持久化均失败，已请求应急本地蜂鸣，track={}", trackId);
                    }
                });

            infrastructure::camera::CameraOptions cameraOptions;
            cameraOptions.deviceId = config.getInt("vision.camera_device_id", 0);
            cameraOptions.failureExitAfterMs = config.getInt("vision.camera_failure_exit_ms", 10000);
            cameraOptions.videoPath = videoInputPath;
            camera = std::make_unique<infrastructure::camera::CameraCapture>(
                std::move(cameraOptions), *frames,
                [this](const cv::Mat& frame)
                {
                    eventRecorder->observeFrame(frame);
                    liveStreamer->pushFrame(frame);
                });

            runtimeOptions.audioExpected = audioExpected;
            runtimeOptions.checkIntervalMs = config.getInt("runtime.check_interval_ms", 500);
            runtimeOptions.cameraStallMs = config.getInt("runtime.camera_stall_ms", 15000);
            runtimeOptions.cameraRestartDelayMs = config.getInt("runtime.camera_restart_delay_ms", 3000);
            runtimeOptions.maxCameraRestarts = config.getInt("runtime.max_camera_restarts", 5);
            runtimeOptions.audioRetryInitialMs = config.getInt("runtime.audio_retry_initial_ms", 3000);
            runtimeOptions.audioRetryMaxMs = config.getInt("runtime.audio_retry_max_ms", 60000);
            runtimeOptions.reportIntervalMs = config.getInt("runtime.report_interval_ms", 10000);

            systemdNotifier = std::make_unique<infrastructure::system::SystemdNotifier>();

            SPDLOG_INFO("[APP] 初始化完成：device_id={}, area={}", deviceId, deploymentArea);
            return true;
        }

        /** 主线程调用；在重建 KWS Stream 前必须先 stop() ALSA，等待采集线程退出。 */
        [[nodiscard]] bool restartAudio()
        {
            audioPipeline->stop();
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

            application::ports::AudioCaptureOptions opts;
            opts.device = config.getString("audio.device", opts.device);
            opts.sampleRate = config.getInt("audio.sample_rate", opts.sampleRate);
            opts.channels = config.getInt("audio.channels", opts.channels);
            opts.framesPerChunk = config.getInt("audio.frames_per_chunk", opts.framesPerChunk);
            if (opts.sampleRate != kws.sampleRate)
            {
                SPDLOG_ERROR("[Audio] ALSA 与 KWS 采样率不一致，继续纯视觉监测");
                return false;
            }
            if (!keywordSpotter->initialize(kws)) return false;
            return audioPipeline->start(opts, audioCooldownMs, [this](AlertEvent event)
            {
                const std::string keyword = event.keyword;
                if (!eventService->submit(std::move(event)))
                {
                    if (localAlarmReady)
                        (void)buzzer->trigger(std::chrono::milliseconds(alertDurationMs));
                    SPDLOG_CRITICAL("[Audio] 求救入队及溢出持久化均失败，已请求应急本地蜂鸣：keyword={}", keyword);
                }
            });
        }

        [[nodiscard]] bool startServices()
        {
            // 启动失败属于不可恢复错误；Application::start() 会调用 stop() 回收已启动的部分。
            if (!commands->start())
            {
                SPDLOG_CRITICAL("[APP] 指令服务启动失败");
                return false;
            }
            if (!outbox->start())
                SPDLOG_ERROR("[APP] MQTT Outbox 不可用；报警留存于 SQLite");
            if (config.getBool("video_upload.enabled", true) && !videoUploader->start())
                SPDLOG_ERROR("[APP] 视频上传线程不可用；录像继续保留于 SQLite");
            if (!eventService->start())
            {
                SPDLOG_CRITICAL("[APP] 事件服务启动失败");
                return false;
            }
            if (audioExpected && !restartAudio())
                SPDLOG_WARN("[APP] ALSA/KWS 初次启动失败，监管器将在后台重试");
            if (!audioExpected)
                SPDLOG_INFO("[APP] 语音功能已禁用");
            if (!camera->start())
                SPDLOG_WARN("[APP] 摄像头初次启动失败，交由 RuntimeSupervisor 恢复");

            // 采集启动后才初始化监管器的时间基准，避免较慢的 KWS 初始化导致误判。
            application::runtime::RuntimeSupervisorCallbacks callbacks;
            callbacks.cameraRunning = [this] { return camera->isRunning(); };
            callbacks.cameraFrames = [this] { return camera->capturedCount(); };
            callbacks.restartCamera = [this]
            {
                camera->stop();
                return camera->start();
            };
            callbacks.audioRunning = [this] { return audioPipeline->isRunning(); };
            callbacks.restartAudio = [this] { return restartAudio(); };
            callbacks.mqttConnected = [this] { return outbox->isConnected(); };
            callbacks.queuedEvents = [this] { return eventService->pendingCount(); };
            supervisor = std::make_unique<application::runtime::RuntimeSupervisor>(runtimeOptions, std::move(callbacks), *systemdNotifier);

            // start() 不阻塞在视觉推理；主线程继续负责运行监管与信号处理。
            stopRequested.store(false);
            visionExited.store(false);
            visionFailed.store(false);
            try
            {
                visionWorker = std::thread([this]
                {
                    try
                    {
                        visionPipeline->runUntil([this] { return !stopRequested.load(); });
                    }
                    catch (const std::exception& error)
                    {
                        SPDLOG_CRITICAL("[Vision] 未处理异常：{}", error.what());
                        visionFailed.store(true);
                    }
                    catch (...)
                    {
                        SPDLOG_CRITICAL("[Vision] 未知异常导致视觉线程退出");
                        visionFailed.store(true);
                    }
                    visionExited.store(true);
                });
            }
            catch (const std::exception& error)
            {
                SPDLOG_CRITICAL("[APP] 无法启动视觉线程：{}", error.what());
                return false;
            }
            started = true;
            SPDLOG_INFO("[APP] 采集、视觉、事件、网络和运行监管已启动");
            return true;
        }

        void restoreBackgroundServices()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextServiceRestart) return;
            nextServiceRestart = now + std::chrono::seconds(10);
            if (outbox && !outbox->isRunning() && !outbox->start())
                SPDLOG_ERROR("[APP] MQTT Outbox 后台线程恢复失败；SQLite 仍保留待发事件");
            if (config.getBool("video_upload.enabled", true) && videoUploader && !videoUploader->isRunning() && !videoUploader->start())
                SPDLOG_ERROR("[APP] HTTP 上传后台线程恢复失败；保留待传录像");
        }

        void shutdown() noexcept
        {
            if (stopped) return;
            stopped = true;
            stopRequested.store(true);

            if (supervisor) supervisor->stopping();
            // 先停产生事件的线程；采集线程不得再访问录像器或直播器。
            if (audioPipeline) audioPipeline->stop();
            if (camera) camera->stop();
            if (frames) frames->close();
            if (visionWorker.joinable())
            {
                if (visionWorker.get_id() == std::this_thread::get_id())
                    std::terminate(); // 禁止从视觉工作线程 stop()，main 负责释放。
                visionWorker.join();
            }
            // 然后排空已产生的报警和录像；最后关闭网络/指令/GPIO。
            if (eventService) eventService->stop();
            if (eventRecorder) eventRecorder->stopAndDrain();
            if (videoUploader) videoUploader->stop();
            if (commands) commands->stop();
            if (outbox) outbox->stop();
            if (liveStreamer) liveStreamer->stop();
            if (buzzer) buzzer->stop();
            SPDLOG_INFO("[APP] 网关已停止，摄像头累计采集帧={}", camera ? camera->capturedCount() : 0);
            started = false;
        }
    };

    Application::Application() = default;
    Application::~Application() { stop(); }

    bool Application::init(int argc, char* argv[])
    {
        if (impl_) return false; // 不允许在已初始化的实例上重复 init。
        try
        {
            auto next = std::make_unique<Impl>();
            if (!next->initialize(argc, argv)) return false;
            impl_ = std::move(next);
            return true;
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[APP] 初始化异常：{}", error.what());
            return false;
        }
        catch (...)
        {
            SPDLOG_CRITICAL("[APP] 初始化发生未知异常");
            return false;
        }
    }

    bool Application::start()
    {
        if (!impl_ || impl_->startedOnce || impl_->stopped) return false;
        impl_->startedOnce = true;
        try
        {
            if (impl_->startServices()) return true;
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[APP] 启动异常：{}", error.what());
        }
        catch (...)
        {
            SPDLOG_CRITICAL("[APP] 启动发生未知异常");
        }
        stop();
        return false;
    }

    bool Application::tick()
    {
        if (!impl_ || !impl_->started || impl_->stopped) return false;
        if (impl_->visionExited.load() || impl_->visionFailed.load())
        {
            SPDLOG_CRITICAL("[APP] 视觉处理线程已意外退出，终止进程交由外部监管器恢复");
            return false;
        }
        try
        {
            impl_->restoreBackgroundServices();
            return impl_->supervisor->tick();
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[APP] 运行监管异常：{}", error.what());
            return false;
        }
        catch (...)
        {
            SPDLOG_CRITICAL("[APP] 运行监管发生未知异常");
            return false;
        }
    }

    void Application::stop() noexcept
    {
        if (!impl_) return;
        impl_->shutdown();
        impl_.reset(); // 所有线程已收尾，按依赖逆序析构 PImpl 持有的对象。
    }
} // namespace fall_monitoring::app
