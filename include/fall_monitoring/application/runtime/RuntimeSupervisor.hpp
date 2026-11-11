#pragma once

#include "fall_monitoring/application/ports/IRuntimeNotifier.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace fall_monitoring::application::runtime
{
    /**
     * @brief 运行监管参数；均在 Application 主线程生效。
     * 摄像头连续失效视为核心能力故障，达到重启上限时要求进程非零退出。
     * 音频作为可降级通道，重试不影响视觉、SQLite 和 MQTT。
     */
    struct RuntimeSupervisorOptions
    {
        bool audioExpected = false;
        int checkIntervalMs = 500;
        int cameraStallMs = 15000;
        int cameraRestartDelayMs = 3000;
        int maxCameraRestarts = 5;
        int audioRetryInitialMs = 3000;
        int audioRetryMaxMs = 60000;
        int reportIntervalMs = 10000;
    };

    /**
     * @brief 对现有模块的回调端口，应用层不依赖 V4L2/ALSA/Paho 的具体类型。
     * 所有回调在主线程执行；绝不能在采集回调或 EventService 消费线程中调用 tick()。
     */
    struct RuntimeSupervisorCallbacks
    {
        std::function<bool()> cameraRunning;
        std::function<std::uint64_t()> cameraFrames;
        std::function<bool()> restartCamera;
        std::function<bool()> audioRunning;
        std::function<bool()> restartAudio;
        std::function<bool()> mqttConnected;
        std::function<std::size_t()> queuedEvents;
    };

    class RuntimeSupervisor final
    {
    public:
        RuntimeSupervisor(RuntimeSupervisorOptions options, RuntimeSupervisorCallbacks callbacks, ports::IRuntimeNotifier& notifier);
        ~RuntimeSupervisor();
        RuntimeSupervisor(const RuntimeSupervisor&) = delete;
        RuntimeSupervisor& operator=(const RuntimeSupervisor&) = delete;

        /**
         * @brief 由 VisionPipeline::runUntil 的谓词在主线程定期调用。
         * @return false 表示摄像头重启次数耗尽，应结束主循环并返回非零退出码。
         * @note 不在本类创建额外工作线程，所有线程生命周期仍由 Application 管理。
         */
        [[nodiscard]] bool tick();
        void stopping() noexcept;

    private:
        using Clock = std::chrono::steady_clock;
        [[nodiscard]] bool attemptCameraRestart(Clock::time_point now);
        void recoverAudio(Clock::time_point now);
        void report(Clock::time_point now, bool cameraOnline, bool audioOnline, bool mqttOnline);

        RuntimeSupervisorOptions options_;
        RuntimeSupervisorCallbacks callbacks_;
        ports::IRuntimeNotifier& notifier_;
        Clock::time_point lastCheck_{};
        Clock::time_point lastFrameAt_{};
        Clock::time_point lastCameraAttempt_{};
        Clock::time_point nextAudioAttempt_{};
        Clock::time_point lastReport_{};
        std::uint64_t lastFrames_ = 0;
        std::uint64_t totalCameraRestarts_ = 0;
        std::uint64_t totalAudioRestarts_ = 0;
        int consecutiveCameraRestarts_ = 0;
        int consecutiveAudioFailures_ = 0;
        bool seenFrame_ = false;
        bool observedMqtt_ = false;
        bool previousMqtt_ = false;
        bool fatal_ = false;
        bool stopping_ = false;
    };
}
