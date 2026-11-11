#include "fall_monitoring/infrastructure/hardware/SysfsBuzzer.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace fall_monitoring::infrastructure::hardware
{
    namespace
    {
        // sysfs 虚拟文件需按一次写操作提交；禁止使用 shell 重定向。
        bool writeSysfs(const std::string& path, const std::string& value) noexcept
        {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
            if (fd < 0) return false;
            const ssize_t written = ::write(fd, value.data(), value.size());
            const int writeErrno = errno;
            const int closeResult = ::close(fd);
            if (written != static_cast<ssize_t>(value.size()))
            {
                errno = writeErrno;
                return false;
            }
            return closeResult == 0;
        }
    }

    SysfsBuzzer::SysfsBuzzer(SysfsBuzzerOptions options) : options_(options), gpioDir_("/sys/class/gpio/gpio" + std::to_string(options.gpioPin)), valuePath_(gpioDir_ + "/value")
    {
    }

    SysfsBuzzer::~SysfsBuzzer() { stop(); }

    bool SysfsBuzzer::initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return true;
        if (options_.gpioPin < 0)
        {
            SPDLOG_ERROR("[Buzzer] GPIO 编号不能为负数：{}", options_.gpioPin);
            return false;
        }
        if (worker_.joinable())
        {
            SPDLOG_ERROR("[Buzzer] 上一次工作线程尚未回收");
            return false;
        }

        ownExport_ = false;
        if (!std::filesystem::exists(gpioDir_))
        {
            if (!writeSysfs("/sys/class/gpio/export", std::to_string(options_.gpioPin)))
            {
                SPDLOG_ERROR("[Buzzer] 无法导出 GPIO {}：{}；请检查内核 Sysfs 与 GPIO 权限", options_.gpioPin, std::strerror(errno));
                return false;
            }
            ownExport_ = true;
        }

        // export 后内核可能稍后才创建 gpioN/direction；有限等待，不长期阻塞。
        bool ready = false;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            if (std::filesystem::exists(valuePath_) && std::filesystem::exists(gpioDir_ + "/direction"))
            {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!ready)
        {
            SPDLOG_ERROR("[Buzzer] GPIO {} 导出后仍无方向或数值节点", options_.gpioPin);
            releaseExport();
            return false;
        }

        if (!writeSysfs(gpioDir_ + "/direction", "out") || !writeValue(false))
        {
            SPDLOG_ERROR("[Buzzer] GPIO {} 设为输出或默认关闭失败：{}", options_.gpioPin, std::strerror(errno));
            releaseExport();
            return false;
        }

        stopping_ = false;
        until_ = {};
        running_ = true;
        try
        {
            worker_ = std::thread(&SysfsBuzzer::workerLoop, this);
        }
        catch (const std::exception& e)
        {
            running_ = false;
            SPDLOG_ERROR("[Buzzer] 创建工作线程失败：{}", e.what());
            (void)writeValue(false);
            releaseExport();
            return false;
        }
        SPDLOG_INFO("[Buzzer] 初始化完成：gpio={}, activeHigh={}", options_.gpioPin, options_.activeHigh);
        return true;
    }

    bool SysfsBuzzer::trigger(std::chrono::milliseconds duration) noexcept
    {
        if (duration.count() <= 0 || duration.count() > 60000) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_) return false;
            const auto deadline = Clock::now() + duration;
            // 不丢弃当前报警；短间隔内多个独立事件会延长蜂鸣，而不是重启 GPIO。
            if (deadline > until_) until_ = deadline;
        }
        cv_.notify_one();
        return true;
    }

    void SysfsBuzzer::workerLoop() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_)
        {
            cv_.wait(lock, [this] { return stopping_ || until_ > Clock::now(); });
            if (stopping_) break;

            lock.unlock();
            if (!writeValue(true))
            {
                SPDLOG_ERROR("[Buzzer] 无法拉起 GPIO {}：{}", options_.gpioPin, std::strerror(errno));
                lock.lock();
                until_ = {};
                continue;
            }
            lock.lock();

            // wait_until 期间触发新的事件只会延长 deadline，避免长时间持锁。
            while (!stopping_ && Clock::now() < until_)
                cv_.wait_until(lock, until_);

            if (!stopping_) until_ = {};
            lock.unlock();
            if (!writeValue(false))
                SPDLOG_ERROR("[Buzzer] 无法关闭 GPIO {}：{}", options_.gpioPin, std::strerror(errno));
            lock.lock();
        }
        lock.unlock();
        (void)writeValue(false);
        SPDLOG_INFO("[Buzzer] 报警工作线程退出");
    }

    void SysfsBuzzer::releaseExport() noexcept
    {
        if (ownExport_)
        {
            if (!writeSysfs("/sys/class/gpio/unexport", std::to_string(options_.gpioPin)))
                SPDLOG_WARN("[Buzzer] 取消导出 GPIO {} 失败：{}", options_.gpioPin, std::strerror(errno));
            ownExport_ = false;
        }
    }

    void SysfsBuzzer::stop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !worker_.joinable() && !ownExport_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id())
            {
                SPDLOG_CRITICAL("[Buzzer] 不允许在 GPIO 工作线程调用 stop()");
                std::terminate();
            }
            worker_.join();
        }
        (void)writeValue(false);
        releaseExport();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            until_ = {};
        }
        SPDLOG_INFO("[Buzzer] 已安全关闭");
    }

    bool SysfsBuzzer::isInitialized() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && !stopping_;
    }
}
