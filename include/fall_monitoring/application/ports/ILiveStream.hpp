#pragma once

#include <string>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 实时推流控制端口；设备指令不依赖 FFmpeg 的启动方式。
     * start() 返回成功仅表示编码子进程已创建，不代表 RTMP 服务端已接收视频。
     */
    class ILiveStream
    {
    public:
        virtual ~ILiveStream() = default;
        [[nodiscard]] virtual bool start(const std::string& rtmpUrl) = 0;
        virtual void stop() noexcept = 0;
        [[nodiscard]] virtual bool isStreaming() const noexcept = 0;
    };
}
