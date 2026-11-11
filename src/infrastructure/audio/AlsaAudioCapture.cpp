#include "fall_monitoring/infrastructure/audio/AlsaAudioCapture.hpp"

#include <alsa/asoundlib.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fall_monitoring::infrastructure::audio
{
    struct AlsaAudioCapture::Impl final
    {
        using Options = application::ports::AudioCaptureOptions;
        using Handler = application::ports::IAudioCapture::SamplesHandler;

        ~Impl() { stop(); }

        [[nodiscard]] bool start(const Options& options, Handler onSamples)
        {
            if (running_.load() || worker_.joinable())
            {
                // worker_ 即使自然退出也必须 join，才能安全地重新启动。
                SPDLOG_ERROR("[Audio] 上次采集线程未完全回收，请先调用 stop()");
                return false;
            }
            if (!onSamples || options.device.empty() || options.sampleRate <= 0 || options.channels != 1 || options.framesPerChunk <= 0 || static_cast<std::int64_t>(options.framesPerChunk) > static_cast<std::int64_t>(options.sampleRate) * 2)
            {
                SPDLOG_ERROR("[Audio] 音频采集参数非法：device={} rate={} channels={} chunk={}", options.device, options.sampleRate, options.channels, options.framesPerChunk);
                return false;
            }

            const int rc = snd_pcm_open(&pcm_, options.device.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
            if (rc < 0)
            {
                pcm_ = nullptr;
                SPDLOG_ERROR("[Audio] 无法打开麦克风 {}：{}", options.device, snd_strerror(rc));
                return false;
            }

            // 旧项目保持不变：S16_LE、interleaved、允许 plug 重采样、100ms 缓冲延迟。
            const int setup = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, static_cast<unsigned int>(options.channels), static_cast<unsigned int>(options.sampleRate), 1, 100000);
            if (setup < 0)
            {
                SPDLOG_ERROR("[Audio] 配置 PCM 设备失败：{}", snd_strerror(setup));
                closePcm();
                return false;
            }

            options_ = options;
            handler_ = std::move(onSamples);
            running_.store(true);
            try
            {
                worker_ = std::thread(&Impl::captureLoop, this);
            }
            catch (const std::exception& ex)
            {
                running_.store(false);
                SPDLOG_ERROR("[Audio] 创建采集线程失败：{}", ex.what());
                closePcm();
                handler_ = {};
                return false;
            }

            SPDLOG_INFO("[Audio] 麦克风已启动：device={}, rate={}Hz, channels={}, chunk={}帧", options_.device, options_.sampleRate, options_.channels, options_.framesPerChunk);
            return true;
        }

        void stop() noexcept
        {
            running_.store(false);
            // ALSA 调用只在采集线程中发生；先 join，再在调用线程关闭句柄。
            // NONBLOCK + 100ms 的 snd_pcm_wait 超时使采集线程能周期性观察停止标志。
            if (worker_.joinable())
            {
                if (worker_.get_id() != std::this_thread::get_id())
                {
                    worker_.join();
                }
                else
                {
                    // 不允许采集回调在本线程调用 stop()；由 Application 负责生命周期。
                    SPDLOG_CRITICAL("[Audio] 禁止从采集回调线程调用 stop()；请在管理线程停止音频");
                    std::terminate();
                }
            }
            if (pcm_ != nullptr)
            {
                closePcm();
                SPDLOG_INFO("[Audio] 麦克风已关闭");
            }
            handler_ = {};
        }

        [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }

    private:
        void closePcm() noexcept
        {
            if (pcm_ != nullptr)
            {
                snd_pcm_close(pcm_);
                pcm_ = nullptr;
            }
        }

        void captureLoop() noexcept
        {
            using Clock = std::chrono::steady_clock;
            using namespace std::chrono_literals;

            // 回收由 stop() 统一进行。PCM 缓冲只归采集线程使用。
            const auto requested = static_cast<snd_pcm_uframes_t>(options_.framesPerChunk);
            std::vector<std::int16_t> pcmSamples(static_cast<std::size_t>(requested));
            std::vector<float> normalizedSamples;
            normalizedSamples.reserve(static_cast<std::size_t>(requested));

            std::uint64_t chunks = 0;
            std::uint64_t sampleCount = 0;
            std::uint64_t xruns = 0;
            auto lastReport = Clock::now();
            auto lastXrunWarn = Clock::time_point{};

            try
            {
                while (running_.load())
                {
                    // 非阻塞模式避免设备异常时线程永久卡在 readi()，利于安全退出。
                    const int ready = snd_pcm_wait(pcm_, 100);
                    if (!running_.load()) break;
                    if (ready == 0 || ready == -EINTR) continue;
                    if (ready < 0)
                    {
                        const int recovered = snd_pcm_recover(pcm_, ready, 1);
                        if (recovered < 0)
                        {
                            SPDLOG_ERROR("[Audio] 等待音频帧失败，无法恢复：{}", snd_strerror(recovered));
                            break;
                        }
                        continue;
                    }

                    const snd_pcm_sframes_t frames = snd_pcm_readi(pcm_, pcmSamples.data(), requested);
                    if (frames == -EAGAIN || frames == -EINTR) continue;
                    if (frames < 0)
                    {
                        if (frames == -EPIPE)
                        {
                            ++xruns;
                            const auto now = Clock::now();
                            if (lastXrunWarn == Clock::time_point{} || now - lastXrunWarn >= 5s)
                            {
                                SPDLOG_WARN("[Audio] ALSA XRUN：采集缓冲溢出，尝试恢复");
                                lastXrunWarn = now;
                            }
                        }
                        // 尝试修复 XRUN/挂起/常规设备错误。
                        const int recovered = snd_pcm_recover(pcm_, static_cast<int>(frames), 1);
                        if (recovered < 0)
                        {
                            SPDLOG_ERROR("[Audio] PCM 读取失败，恢复失败：{}", snd_strerror(recovered));
                            break;
                        }
                        continue;
                    }
                    if (frames == 0) continue;

                    normalizedSamples.resize(static_cast<std::size_t>(frames));
                    for (snd_pcm_sframes_t i = 0; i < frames; ++i)
                    {
                        // int16 [-32768,32767] -> float [-1.0,1.0)，与原 sherpa-onnx 输入一致。
                        normalizedSamples[static_cast<std::size_t>(i)] = static_cast<float>(pcmSamples[static_cast<std::size_t>(i)]) / 32768.0f;
                    }

                    // 在采集线程同步调用。不得跨线程持有 normalizedSamples 引用。
                    handler_(normalizedSamples);
                    ++chunks;
                    sampleCount += static_cast<std::uint64_t>(frames);

                    const auto now = Clock::now();
                    const auto elapsed = std::chrono::duration<double>(now - lastReport).count();
                    if (elapsed >= 10.0)
                    {
                        SPDLOG_INFO("[Audio] {:.1f}s: PCM批次={}, 样本数={}, XRUN次数={}", elapsed, chunks, sampleCount, xruns);
                        chunks = sampleCount = xruns = 0;
                        lastReport = now;
                    }
                }
            }
            catch (const std::exception& ex)
            {
                SPDLOG_ERROR("[Audio] 采集线程异常：{}", ex.what());
            }
            catch (...)
            {
                SPDLOG_ERROR("[Audio] 采集线程发生未知异常");
            }

            running_.store(false);
            SPDLOG_INFO("[Audio] 采集线程退出");
        }

        Options options_;
        Handler handler_;
        snd_pcm_t* pcm_ = nullptr;
        std::thread worker_;
        std::atomic<bool> running_{false};
    };

    AlsaAudioCapture::AlsaAudioCapture() : impl_(std::make_unique<Impl>()) {}
    AlsaAudioCapture::~AlsaAudioCapture() = default;

    bool AlsaAudioCapture::start(const application::ports::AudioCaptureOptions& options, SamplesHandler onSamples)
    {
        return impl_->start(options, std::move(onSamples));
    }

    void AlsaAudioCapture::stop() noexcept { impl_->stop(); }
    bool AlsaAudioCapture::isRunning() const noexcept { return impl_->isRunning(); }
} // namespace fall_monitoring::infrastructure::audio
