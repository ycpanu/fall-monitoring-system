#include "fall_monitoring/infrastructure/logging/Logger.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace fall_monitoring::infrastructure::logging
{
    bool Logger::initialize(const std::string& filePath, const std::string& level)
    {
        if (initialized_) return true;
        try
        {
            const std::filesystem::path path(filePath);
            if (path.has_parent_path())
            {
                std::filesystem::create_directories(path.parent_path());
            }

            // 8192条缓冲，1个异步写盘线程
            spdlog::init_thread_pool(8192, 1);
            auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filePath, 5 * 1024 * 1024, 3);
            std::vector<spdlog::sink_ptr> sinks{console, file};

            // 使用阻塞移除策略，避免关键错误日志因队列满被静默覆盖
            auto logger = std::make_shared<spdlog::async_logger>("FallMonitoring", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);

            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] [%s:%#] %v");
            std::string normalizedLevel = level;
            std::transform(normalizedLevel.begin(), normalizedLevel.end(), normalizedLevel.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            logger->set_level(spdlog::level::from_str(normalizedLevel));
            logger->flush_on(spdlog::level::err);
            spdlog::set_default_logger(logger);
            initialized_ = true;
            SPDLOG_INFO("[Logging] 日志初始化成功: file={}, level={}", filePath, level);
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Logging] 初始化失败：" << e.what() << '\n';
            spdlog::shutdown();
            return false;
        }
    }

    Logger::~Logger()
    {
        if (!initialized_) return;
        SPDLOG_INFO("[Logging] 日志系统关闭");
        spdlog::default_logger()->flush();
        spdlog::shutdown();
    }
}
