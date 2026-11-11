#include "fall_monitoring/infrastructure/system/SystemdNotifier.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fall_monitoring::infrastructure::system
{
    SystemdNotifier::SystemdNotifier()
    {
        if (const char* socket = std::getenv("NOTIFY_SOCKET"))
            socketPath_ = socket;

        if (const char* usec = std::getenv("WATCHDOG_USEC"))
        {
            errno = 0;
            char* end = nullptr;
            const unsigned long long period = std::strtoull(usec, &end, 10);
            if (errno == 0 && end != usec && *end == '\0' &&
                period >= 100000ULL &&
                period < static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
                watchdogPeriod_ = std::chrono::microseconds(static_cast<long long>(period));
        }
    }

    bool SystemdNotifier::send(const std::string& message) noexcept
    {
        if (socketPath_.empty()) return false;

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const bool abstract = socketPath_[0] == '@';
        const std::size_t nameLen = socketPath_.size();
        if (nameLen >= sizeof(address.sun_path)) return false;
        if (abstract)
        {
            address.sun_path[0] = '\0';
            std::memcpy(address.sun_path + 1, socketPath_.data() + 1, nameLen - 1);
        }
        else
        {
            std::memcpy(address.sun_path, socketPath_.c_str(), nameLen + 1);
        }
        const auto addressLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + nameLen + (abstract ? 0U : 1U));
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        const auto sent = ::sendto(fd, message.data(), message.size(), MSG_NOSIGNAL,
                                   reinterpret_cast<const sockaddr*>(&address), addressLen);
        const int savedError = errno;
        ::close(fd);
        if (sent != static_cast<ssize_t>(message.size()))
        {
            if (!warned_)
            {
                warned_ = true;
                SPDLOG_WARN("[Runtime] systemd 通知失败：{}", std::strerror(savedError));
            }
            return false;
        }
        return true;
    }

    void SystemdNotifier::ready() noexcept
    {
        if (readySent_ || socketPath_.empty()) return;
        if (send("READY=1\nSTATUS=Camera frames flowing; gateway ready"))
        {
            readySent_ = true;
            SPDLOG_INFO("[Runtime] 已向 systemd 报告 READY=1");
        }
    }

    void SystemdNotifier::heartbeat() noexcept
    {
        if (!readySent_ || watchdogPeriod_.count() <= 0 || stoppingSent_) return;
        const auto now = Clock::now();
        // 心跳间隔小于 WatchdogSec/2，留出调度余量。
        if (lastHeartbeat_ != Clock::time_point{} && now - lastHeartbeat_ < watchdogPeriod_ / 3)
            return;
        if (send("WATCHDOG=1")) lastHeartbeat_ = now;
    }

    void SystemdNotifier::stopping() noexcept
    {
        if (stoppingSent_) return;
        stoppingSent_ = true;
        if (readySent_) (void)send("STOPPING=1\nSTATUS=Gateway shutting down");
    }
}
