#pragma once

namespace fall_monitoring::application::ports
{
    /** @brief 运行状态通知端口。应用层不依赖 systemd 或 Unix Socket。 */
    class IRuntimeNotifier
    {
    public:
        virtual ~IRuntimeNotifier() = default;
        virtual void ready() noexcept = 0;
        virtual void heartbeat() noexcept = 0;
        virtual void stopping() noexcept = 0;
    };
}
