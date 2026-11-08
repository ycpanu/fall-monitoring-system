#pragma once

#include "fall_monitoring/application/ports/IAudioCapture.hpp"
#include "fall_monitoring/application/ports/IKeywordSpotter.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <chrono>
#include <functional>

namespace fall_monitoring::application::audio
{
    /**
     * @brief 语音业务流水线：ALSA 采样 -> KWS -> 冷却抑制 -> 语音求救事件。
     * 
     * 采集回调在 ALSA 工作线程同步运行，KWS 不额外创建线程。
     * onVoiceEvent 只负责交付事件；后续 EventService 才负责融合、入库与上报。
     */
    class AudioPipeline final
    {
    public:
        using VoiceEventHandler = std::function<void(domain::event::AlertEvent)>;

        AudioPipeline(ports::IAudioCapture& capture, ports::IKeywordSpotter& spotter) noexcept;
        ~AudioPipeline();

        AudioPipeline(const AudioPipeline&) = delete;
        AudioPipeline& operator=(const AudioPipeline&) = delete;

        [[nodiscard]] bool start(const ports::AudioCaptureOptions& options,
                                 int cooldownMs, VoiceEventHandler onVoiceEvent);
        void stop() noexcept;
        [[nodiscard]] bool isRunning() const noexcept;

    private:
        ports::IAudioCapture& capture_; // 非拥有引用：Application 确保引用存活更久。
        ports::IKeywordSpotter& spotter_;
        int cooldownMs_ = 5000;
        std::chrono::steady_clock::time_point lastTrigger_{};
    };
}