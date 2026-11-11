#pragma once

#include <chrono>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 本地硬件告警端口。
     * 应用层不感知 GPIO Sysfs 的具体接口。
     * trigger() 只登记一次异步鸣叫请求，不在事件消费线程执行 GPIO I/O。
     */
    class ILocalAlarm
    {
    public:
        virtual ~ILocalAlarm() = default;
        [[nodiscard]] virtual bool trigger(std::chrono::milliseconds duration) noexcept = 0;
    };
}
