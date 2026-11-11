#pragma once

#include "fall_monitoring/application/ports/ILocalAlarm.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace fall_monitoring::infrastructure::hardware
{
    struct SysfsBuzzerOptions
    {
        int gpioPin = 138;              // 原工程使用的 Linux sysfs GPIO 编号。
        bool activeHigh = true;         // 高电平触发；若驱动是低电平有效则设 false。
    };

    /**
     * @brief Linux GPIO Sysfs 蜂鸣器适配器。
     *
     * initialize() 在主线程导出 GPIO 并置为非活动电平；
     * trigger() 只更新截止时间，真正的 GPIO 开关操作由常驻 worker_ 执行；
     * stop() 通知并 join 工作线程，确保退出时蜂鸣器关闭。
     * 如果 GPIO 是其他进程先导出的，则不会在退出时 unexport 它。
     * 注意：sysfs 是兼容旧项目的 GPIO 接口，需板端内核已启用相应支持并具备访问权限。
     */
    class SysfsBuzzer final : public application::ports::ILocalAlarm
    {
    public:
        explicit SysfsBuzzer(SysfsBuzzerOptions options = {});
        ~SysfsBuzzer() override;
        SysfsBuzzer(const SysfsBuzzer&) = delete;
        SysfsBuzzer& operator=(const SysfsBuzzer&) = delete;

        [[nodiscard]] bool initialize();
        [[nodiscard]] bool trigger(std::chrono::milliseconds duration) noexcept override;
        void stop() noexcept;
        [[nodiscard]] bool isInitialized() const noexcept;

    private:
        using Clock = std::chrono::steady_clock;
        void workerLoop() noexcept;
        [[nodiscard]] bool writeValue(bool enabled) noexcept;
        void releaseExport() noexcept;

        SysfsBuzzerOptions options_;
        std::string gpioDir_;
        std::string valuePath_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::thread worker_;
        Clock::time_point until_{};
        bool running_ = false;
        bool stopping_ = false;
        bool ownExport_ = false;
    };
}
