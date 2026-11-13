#include "fall_monitoring/application/runtime/RuntimeSupervisor.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace fall_monitoring::application::runtime
{
    RuntimeSupervisor::RuntimeSupervisor(RuntimeSupervisorOptions options, RuntimeSupervisorCallbacks callbacks, ports::IRuntimeNotifier& notifier)
        : options_(std::move(options)), callbacks_(std::move(callbacks)), notifier_(notifier)
    {
        if (options_.checkIntervalMs < 100 || options_.cameraStallMs < 1000 ||
            options_.cameraRestartDelayMs < 0 || options_.maxCameraRestarts <= 0 ||
            options_.audioRetryInitialMs < 100 ||
            options_.audioRetryMaxMs < options_.audioRetryInitialMs ||
            options_.reportIntervalMs < options_.checkIntervalMs ||
            !callbacks_.cameraRunning || !callbacks_.cameraFrames ||
            !callbacks_.restartCamera || !callbacks_.mqttConnected ||
            !callbacks_.queuedEvents ||
            (options_.audioExpected && (!callbacks_.audioRunning || !callbacks_.restartAudio)))
            throw std::invalid_argument("RuntimeSupervisor: 参数或模块回调无效");

        const auto now = Clock::now();
        lastFrameAt_ = now;
        lastReport_ = now;
        nextAudioAttempt_ = now;
        SPDLOG_INFO("[Runtime] 监管器已就绪：cameraStall={}ms，cameraRetries={}，audioRecovery={}", options_.cameraStallMs, options_.maxCameraRestarts, options_.audioExpected);
    }

    RuntimeSupervisor::~RuntimeSupervisor() { stopping(); }

    bool RuntimeSupervisor::attemptCameraRestart(Clock::time_point now)
    {
        if (consecutiveCameraRestarts_ >= options_.maxCameraRestarts)
        {
            SPDLOG_CRITICAL("[Runtime] 摄像头连续重启 {} 次仍没有稳定视频帧，结束进程并请求外部管理器恢复", consecutiveCameraRestarts_);
            fatal_ = true;
            return false;
        }
        if (lastCameraAttempt_ != Clock::time_point{} && now - lastCameraAttempt_ < std::chrono::milliseconds(options_.cameraRestartDelayMs))
            return true;

        lastCameraAttempt_ = now;
        ++consecutiveCameraRestarts_;
        ++totalCameraRestarts_;
        SPDLOG_WARN("[Runtime] 摄像头失效，执行重启：第 {}/{} 次；停止前可能等待 V4L2 读取返回", consecutiveCameraRestarts_, options_.maxCameraRestarts);
        try
        {
            const bool started = callbacks_.restartCamera();
            if (started)
            {
                // start() 不代表有视频帧。只有计数真正前进才重置连续失败次数。
                lastFrames_ = callbacks_.cameraFrames();
                lastFrameAt_ = Clock::now();
                seenFrame_ = false; // start() 不是健康帧，禁止在新帧出现前发送 READY/Watchdog。
                SPDLOG_INFO("[Runtime] 摄像头采集线程已重新启动，等待实际视频帧");
            }
            else
            {
                SPDLOG_ERROR("[Runtime] 摄像头重启失败，将按配置再次尝试");
            }
        }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("[Runtime] 摄像头重启异常：{}", error.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("[Runtime] 摄像头重启出现未知异常");
        }
        return true;
    }

    void RuntimeSupervisor::recoverAudio(Clock::time_point now)
    {
        if (!options_.audioExpected || callbacks_.audioRunning() || now < nextAudioAttempt_)
        {
            if (options_.audioExpected && callbacks_.audioRunning())
                consecutiveAudioFailures_ = 0;
            return;
        }

        bool recovered = false;
        try { recovered = callbacks_.restartAudio(); }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("[Runtime] ALSA/KWS 恢复异常：{}", error.what());
        }
        catch (...) { SPDLOG_ERROR("[Runtime] ALSA/KWS 恢复出现未知异常"); }

        if (recovered)
        {
            ++totalAudioRestarts_;
            consecutiveAudioFailures_ = 0;
            nextAudioAttempt_ = now + std::chrono::milliseconds(options_.audioRetryInitialMs);
            SPDLOG_INFO("[Runtime] 音频采集已恢复，累计恢复次数={}", totalAudioRestarts_);
            return;
        }

        // 指数退避上限，避免设备未插入时反复打开 ALSA 与加载模型。
        ++consecutiveAudioFailures_;
        const auto exponent = std::min(consecutiveAudioFailures_ - 1, 8);
        long long delay = options_.audioRetryInitialMs;
        delay <<= exponent;
        delay = std::min(delay, static_cast<long long>(options_.audioRetryMaxMs));
        nextAudioAttempt_ = Clock::now() + std::chrono::milliseconds(delay);
        SPDLOG_WARN("[Runtime] 音频仍不可用，{}ms 后再尝试；视觉报警继续运行", delay);
    }

    void RuntimeSupervisor::report(Clock::time_point now, bool cameraOnline, bool audioOnline, bool mqttOnline)
    {
        if (now - lastReport_ < std::chrono::milliseconds(options_.reportIntervalMs)) return;
        lastReport_ = now;
        const auto frameAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameAt_).count();
        SPDLOG_INFO("[Runtime] 健康状态：camera={}，frameAge={}ms，audio={}，mqtt={}，eventQueue={}，cameraRestarts={}，audioRestarts={}",
                    cameraOnline, frameAge, options_.audioExpected ? (audioOnline ? "RUNNING" : "DEGRADED") : "DISABLED",
                    mqttOnline ? "ONLINE" : "OFFLINE", callbacks_.queuedEvents(),
                    totalCameraRestarts_, totalAudioRestarts_);
    }

    bool RuntimeSupervisor::tick()
    {
        if (stopping_ || fatal_) return false;
        const auto now = Clock::now();
        if (lastCheck_ != Clock::time_point{} && now - lastCheck_ < std::chrono::milliseconds(options_.checkIntervalMs))
            return true;
        lastCheck_ = now;

        const auto frames = callbacks_.cameraFrames();
        if (frames != lastFrames_)
        {
            lastFrames_ = frames;
            lastFrameAt_ = now;
            if (!seenFrame_ || consecutiveCameraRestarts_ != 0)
                SPDLOG_INFO("[Runtime] 已收到摄像头视频帧，采集正常");
            seenFrame_ = true;
            consecutiveCameraRestarts_ = 0;
        }
        const bool cameraOnline = callbacks_.cameraRunning();
        const bool cameraStalled = now - lastFrameAt_ >= std::chrono::milliseconds(options_.cameraStallMs);
        if (!cameraOnline || cameraStalled)
        {
            if (!attemptCameraRestart(now)) return false;
        }

        recoverAudio(now);
        const bool audioOnline = options_.audioExpected && callbacks_.audioRunning();
        const bool mqttOnline = callbacks_.mqttConnected();
        if (!observedMqtt_ || previousMqtt_ != mqttOnline)
        {
            if (mqttOnline)
                SPDLOG_INFO("[Runtime] MQTT 已连接，SQLite Outbox 正常补传");
            else
                SPDLOG_WARN("[Runtime] MQTT 未连接：报警保留于 SQLite，等待 Outbox 恢复");
            observedMqtt_ = true;
            previousMqtt_ = mqttOnline;
        }
        report(now, callbacks_.cameraRunning(), audioOnline, mqttOnline);

        if (callbacks_.cameraRunning() && now - lastFrameAt_ < std::chrono::milliseconds(options_.cameraStallMs))
        {
            if (seenFrame_)
            {
                notifier_.ready();
                notifier_.heartbeat();
            }
        }
        return true;
    }

    void RuntimeSupervisor::stopping() noexcept
    {
        if (stopping_) return;
        stopping_ = true;
        notifier_.stopping();
        SPDLOG_INFO("[Runtime] 进入停止状态，停止发送 systemd 健康心跳");
    }
}
