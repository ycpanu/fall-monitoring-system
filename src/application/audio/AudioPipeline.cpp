#include "fall_monitoring/application/audio/AudioPipeline.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace fall_monitoring::application::audio
{
    AudioPipeline::AudioPipeline(ports::IAudioCapture& capture, ports::IKeywordSpotter& spotter) noexcept : capture_(capture), spotter_(spotter) {}

    AudioPipeline::~AudioPipeline()
    {
        stop(); // RAII：应用异常返回时也停止采集线程。
    }

    bool AudioPipeline::start(const ports::AudioCaptureOptions& options, int cooldownMs, VoiceEventHandler onVoiceEvent)
    {
        if (!spotter_.isInitialized() || !onVoiceEvent || cooldownMs < 0)
        {
            SPDLOG_ERROR("[AudioPipeline] KWS 未初始化、事件处理回调为空或冷却参数非法");
            return false;
        }
        if (capture_.isRunning())
        {
            SPDLOG_WARN("[AudioPipeline] 音频采集已启动，忽略重复请求");
            return false;
        }
        cooldownMs_ = cooldownMs;
        lastTrigger_ = {};

        return capture_.start(options,[this, handler = std::move(onVoiceEvent)](const std::vector<float>& samples){
            const auto keyword = spotter_.processSamples(samples);
            if (!keyword) return;

            // 冷却逻辑只在 ALSA 采集线程执行，因此不需要额外加锁。
            const auto now = std::chrono::steady_clock::now();
            if (lastTrigger_.time_since_epoch().count() != 0 && now - lastTrigger_ < std::chrono::milliseconds(cooldownMs_))
            {
                SPDLOG_DEBUG("[KWS] 冷却期内忽略重复求救关键词：{}", *keyword);
                return;
            }
            lastTrigger_ = now;
            SPDLOG_WARN("[KWS] 检测到求救关键词：{}", *keyword);

            domain::event::AlertEvent event;
            event.eventType = domain::event::EventType::HELP_REQUEST;
            event.source = {domain::event::EventSource::VOICE};
            event.status = domain::event::EventStatus::NEW;
            event.keyword = *keyword;
            event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            // 不在采集线程内执行数据库/网络操作；事件消费者应尽快返回。
            // eventId、deviceId 及融合状态由后续统一事件服务补齐。
            handler(std::move(event));
        });
    }

    void AudioPipeline::stop() noexcept
    {
        capture_.stop();
    }

    bool AudioPipeline::isRunning() const noexcept
    {
        return capture_.isRunning();
    }
}