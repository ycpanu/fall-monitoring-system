#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fall_monitoring::infrastructure::config
{
    /**
     * @brief 只负责加载与读取配置，不负责修改配置和业务校验
     * 
     * 实例只属于 Application，不使用可变全局单例
     * 加载完成后供各服务只读使用
     */
    class ConfigManager final
    {
    public:
        bool load(const std::string& path);
        
        [[nodiscard]] std::string getString(const std::string& key, const std::string &fallback = "") const;
        [[nodiscard]] int getInt(const std::string& key, int fallback = 0) const;
        [[nodiscard]] double getDouble(const std::string& key, double fallback = 0.0) const;
        [[nodiscard]] bool getBool(const std::string& key, bool fallback = false) const;

    private:
        [[nodiscard]] const nlohmann::json* find(const std::string& key) const;

        nlohmann::json root_;
        bool loaded_ = false;
    };
}