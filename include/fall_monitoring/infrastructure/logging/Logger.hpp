#pragma once

#include <string>

namespace fall_monitoring::infrastructure::logging
{
    /**
     * @brief 进程日志生命周期管理(RAII)
     * 
     * 日志通过 spdlog 的默认 Logger 输出，避免每个模块创建日志线程
     */
    class Logger final 
    {
    public:
        Logger() = default;
        ~Logger();

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;
        Logger(Logger&&) = delete;
        Logger& operator=(Logger&&) = delete;

        // 返回 false 表示文件/线程池初始化失败，调用者应终止启动
        bool initialize(const std::string& filePath, const std::string& level);

    private:
        bool initialized_ = false;
    };
}