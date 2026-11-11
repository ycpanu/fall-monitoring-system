#pragma once

#include "fall_monitoring/application/ports/IAudioCapture.hpp"

#include <memory>

namespace fall_monitoring::infrastructure::audio
{
    /**
     * @brief ALSA 采集适配器：S16_LE PCM -> 单声道 float [-1, 1)。
     *
     * PImpl 隐藏 snd_pcm_t、线程和 ALSA 头文件，防止驱动依赖传入业务层。
     * 所有 PCM 读、恢复和关闭都在采集线程退出后进行，不与 stop() 并发关闭。
     */
    class AlsaAudioCapture final : public application::ports::IAudioCapture
    {
    public:
        AlsaAudioCapture();
        ~AlsaAudioCapture() override;

        AlsaAudioCapture(const AlsaAudioCapture&) = delete;
        AlsaAudioCapture& operator=(const AlsaAudioCapture&) = delete;
        AlsaAudioCapture(AlsaAudioCapture&&) = delete;
        AlsaAudioCapture& operator=(AlsaAudioCapture&&) = delete;

        [[nodiscard]] bool start(const application::ports::AudioCaptureOptions& options, SamplesHandler onSamples) override;
        void stop() noexcept override;
        [[nodiscard]] bool isRunning() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
