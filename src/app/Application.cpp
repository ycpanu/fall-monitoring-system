#include "fall_monitoring/app/Application.hpp"
#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"
#include "fall_monitoring/infrastructure/logging/Logger.hpp"

#include <spdlog/spdlog.h>
#include <iostream>

namespace fall_monitoring::app
{
    int Application::run()
    {
        infrastructure::config::ConfigManager config;
        if (!config.load("configs/config.json"))
        {
            // 配置初始化失败，stderr
            return 1;
        }

        infrastructure::logging::Logger logger;
        if (!logger.initialize(config.getString("system.log_file_path", "logs/gateway.log"), config.getString("system.log_level", "INFO")))
        {
            return 1;
        }

        SPDLOG_INFO("[APP] 启动, device_id={}, area={}", config.getString("device.device_id", "unknown"), config.getString("device.deployment_area", "unknown"));

        return 0;
    }
}