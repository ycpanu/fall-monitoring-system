#pragma once

#include "fall_monitoring/application/ports/IKeywordSpotter.hpp"

#include <memory>
#include <string>

namespace fall_monitoring::infrastructure::audio
{
    /**
     * @brief sherpa-onnx KWS 模型兼容的配置。
     * 模型文件及关键词列表由设备部署时提供，不纳入源码仓库。
     */
    struct KeywordSpotterOptions
    {
        std::string encoderPath = "models/kws/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
        std::string decoderPath = "models/kws/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
        std::string joinerPath = "models/kws/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
        std::string tokensPath = "models/kws/tokens.txt";
        std::string keywordsPath = "models/kws/keywords_custom.txt";
        int sampleRate = 16000;
        int featureDim = 80;
        int numThreads = 2;
        int maxActivePaths = 4;
        int numTrailingBlanks = 1;
        float keywordsScore = 1.0f;
        float keywordsThreshold = 0.25f;
    };

    /**
     * @brief sherpa-onnx C API 的具体适配器。
     * 
     * 使用 PImpl 隔离第三方头文件；析构先销毁 stream，再销毁 Spotter。
     * 禁止初始化/析构并发调用 processSamples()。
     */
    class SherpaKeywordSpotter final : public application::ports::IKeywordSpotter
    {
    public:
        SherpaKeywordSpotter();
        ~SherpaKeywordSpotter() override;
        SherpaKeywordSpotter(const SherpaKeywordSpotter&) = delete;
        SherpaKeywordSpotter& operator=(const SherpaKeywordSpotter&) = delete;
        SherpaKeywordSpotter(SherpaKeywordSpotter&&) = delete;
        SherpaKeywordSpotter& operator=(SherpaKeywordSpotter&&) = delete;

        [[nodiscard]] bool initialize(const KeywordSpotterOptions& options);
        [[nodiscard]] bool isInitialized() const noexcept override;
        [[nodiscard]] std::optional<std::string> processSamples(const std::vector<float>& samples) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}