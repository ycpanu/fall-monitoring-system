#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 采集参数：16 kHz、单声道、每批约 100 ms 的默认设置。
     * 
     * framesPerChunk 表示每次期望读取的“单声道采样帧”数量，而非字节数。
     * 限制单声道是为了与后续 sherpa-onnx KWS 输入语义保持一致。
     */
    struct AudioCaptureOptions
    {
        std::string device = "plughw:CARD=Device,DEV=0";
        int sampleRate = 16000;
        int channels = 1;
        int framesPerChunk = 1600;
    };

    /**
     * @brief 音频采集端口。应用层只知道 PCM 数据，不知道 ALSA API。
     * 
     * SamplesHandler 在采集线程中同步执行；返回前 samples 引用有效。
     * 交给异步消费者时必须复制或移动到其自有缓冲区，不能保存该引用。
     */
    class IAudioCapture
    {
    public:
        using SamplesHandler = std::function<void(const std::vector<float>&)>;
        
        virtual ~IAudioCapture() = default;
        virtual bool start(const AudioCaptureOptions& options, SamplesHandler onSamples) = 0;
        virtual void stop() noexcept = 0;
        [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    };
}