#pragma once

#include "fall_monitoring/application/ports/IRuntimeNotifier.hpp"

#include <chrono>
#include <string>

namespace fall_monitoring::infrastructure::system
{
    /**
     * @brief systemd sd_notify 协议的轻量实现，不引入 libsystemd 链接依赖。
     * 仅当 NOTIFY_SOCKET 存在时发送；WATCHDOG_USEC 未启用时不发送心跳。
     * 一切调用均由 Application 的主线程执行。
     */
    class SystemdNotifier final : public application::ports::IRuntimeNotifier
    {
    public:
        SystemdNotifier();
        SystemdNotifier(const SystemdNotifier&) = delete;
        SystemdNotifier& operator=(const SystemdNotifier&) = delete;

        void ready() noexcept override;
        void heartbeat() noexcept override;
        void stopping() noexcept override;
        [[nodiscard]] bool enabled() const noexcept { return !socketPath_.empty(); }

    private:
        using Clock = std::chrono::steady_clock;
        bool send(const std::string& message) noexcept;

        std::string socketPath_;
        std::chrono::microseconds watchdogPeriod_{0};
        Clock::time_point lastHeartbeat_{};
        bool readySent_ = false;
        bool stoppingSent_ = false;
        bool warned_ = false;
    };
}
