#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 关键词识别端口：应用层不依赖 sherpa-onnx SDK
     * 
     * 线程约定：processSamples() 由 ALSA 采集线程串行调用。
     * start() 之前由 Application 完成具体识别器初始化。
     */
    class IKeywordSpotter
    {
    public:
        virtual ~IKeywordSpotter() = default;
        [[nodiscard]] virtual bool isInitialized() const noexcept = 0;
        [[nodiscard]] virtual std::optional<std::string> processSamples(const std::vector<float>& samples) = 0;
    };
}