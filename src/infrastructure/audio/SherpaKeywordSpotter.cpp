#include "fall_monitoring/infrastructure/audio/SherpaKeywordSpotter.hpp"

#include <sherpa-onnx/c-api/c-api.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fall_monitoring::infrastructure::audio
{
    struct SherpaKeywordSpotter::Impl final
    {
        ~Impl() {release();}

        void release() noexcept
        {
            // sherpa-onnx 要求先释放在线流，再释放模型对象。
            if (stream_ != nullptr)
            {
                SherpaOnnxDestroyOnlineStream(stream_);
                stream_ = nullptr;
            }
            if (spotter_ != nullptr)
            {
                SherpaOnnxDestroyKeywordSpotter(spotter_);
                spotter_ = nullptr;
            }
            initialized_ = false;
        }

        [[nodiscard]] bool initialize(const KeywordSpotterOptions& options)
        {
            release();

            // 只检查部署文件是否存在
            const std::string* paths[] = {&options.encoderPath, &options.decoderPath, &options.joinerPath, &options.tokensPath, &options.keywordsPath};
            for (const auto* path : paths)
            {
                if (path->empty() || !std::filesystem::is_regular_file(*path))
                {
                    SPDLOG_ERROR("[KWS] 模型或词表文件不存在：{}", *path);
                    return false;
                }
            }
            if (options.sampleRate <= 0 || options.featureDim <= 0 || options.numThreads <= 0 || options.maxActivePaths <= 0 || options.numTrailingBlanks < 0)
            {
                SPDLOG_ERROR("[KWS] 识别参数无效，取消初始化");
                return false;
            }

            options_ = options;
            SherpaOnnxKeywordSpotterConfig config{};
            config.feat_config.sample_rate = options_.sampleRate;
            config.feat_config.feature_dim = options_.featureDim;
            config.model_config.transducer.encoder = options_.encoderPath.c_str();
            config.model_config.transducer.decoder = options_.decoderPath.c_str();
            config.model_config.transducer.joiner = options_.joinerPath.c_str();
            config.model_config.tokens = options_.tokensPath.c_str();
            config.model_config.provider = "cpu"; // RK3566 上保持原有 CPU KWS 模型。
            config.model_config.num_threads = options_.numThreads;
            config.model_config.debug = 0;
            config.keywords_file = options_.keywordsPath.c_str();
            config.max_active_paths = options_.maxActivePaths;
            config.num_trailing_blanks = options_.numTrailingBlanks;
            config.keywords_score = options_.keywordsScore;
            config.keywords_threshold = options_.keywordsThreshold;

            spotter_ = SherpaOnnxCreateKeywordSpotter(&config);
            if (spotter_ == nullptr)
            {
                SPDLOG_ERROR("[KWS] 创建 sherpa-onnx KeywordSpotter 失败");
                release();
                return false;
            }
            stream_ = SherpaOnnxCreateKeywordStream(spotter_);
            if (stream_ == nullptr)
            {
                SPDLOG_ERROR("[KWS] 创建识别 Stream 失败");
                release();
                return false;
            }
            initialized_ = true;
            SPDLOG_INFO("[KWS] 初始化成功：sampleRate={}Hz, threads={}, keywords_file={}", options_.sampleRate, options_.numThreads, options_.keywordsPath);
            return true;
        }

        [[nodiscard]] std::optional<std::string> processSamples(const std::vector<float>& samples)
        {
            if (!initialized_ || samples.empty()) return std::nullopt;

            // 输入来自 ALSA: float PCM，幅值约 [-1, 1]，单通道，原采样率 16 kHz。
            SherpaOnnxOnlineStreamAcceptWaveform(stream_, options_.sampleRate, samples.data(), static_cast<int>(samples.size()));

            // 单次 PCM 输入可能对应多次 decode；不能只用 if 判断。
            while (SherpaOnnxIsKeywordStreamReady(spotter_, stream_))
            {
                SherpaOnnxDecodeKeywordStream(spotter_, stream_);
                const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(spotter_, stream_);
                if (result == nullptr) continue;

                std::optional<std::string> keyword;
                if (result->keyword != nullptr && std::strlen(result->keyword) > 0)
                {
                    keyword = std::string(result->keyword);
                    // 检测到关键词后立即重置流，避免重复识别同一次发声。
                    SherpaOnnxResetKeywordStream(spotter_, stream_);
                }
                // SDK 返回的结果由调用方负责销毁，拷贝字符串后才能释放。
                SherpaOnnxDestroyKeywordResult(result);
                if (keyword) return keyword;
            }
            return std::nullopt;
        }

        KeywordSpotterOptions options_;
        const SherpaOnnxKeywordSpotter* spotter_ = nullptr;
        const SherpaOnnxOnlineStream* stream_ = nullptr;
        bool initialized_ = false;
    };

    SherpaKeywordSpotter::SherpaKeywordSpotter() : impl_(std::make_unique<Impl>()) {}
    SherpaKeywordSpotter::~SherpaKeywordSpotter() = default;

    bool SherpaKeywordSpotter::initialize(const KeywordSpotterOptions& options)
    {
        return impl_->initialize(options);
    }

    bool SherpaKeywordSpotter::isInitialized() const noexcept
    {
        return impl_->initialized_;
    }

    std::optional<std::string> SherpaKeywordSpotter::processSamples(const std::vector<float>& samples)
    {
        return impl_->processSamples(samples);
    }
}