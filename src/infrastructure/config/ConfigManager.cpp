#include "fall_monitoring/infrastructure/config/ConfigManager.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace fall_monitoring::infrastructure::config
{
    bool ConfigManager::load(const std::string& path)
    {
        loaded_ = false;
        root_ = nlohmann::json{};

        std::ifstream input(path);
        if (!input.is_open())
        {
            std::cerr << "[Config] 无法打开配置文件：" << path << '\n';
            return false;
        }

        // 非异常抛出：配置格式错误时返回 false
        nlohmann::json parsed = nlohmann::json::parse(input, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            std::cerr << "[Config] JSON 格式错误或根节点不是对象：" << path << '\n';
            return false;
        }

        root_ = std::move(parsed);
        loaded_ = true;
        return true;
    }

    const nlohmann::json* ConfigManager::find(const std::string& key) const 
    {
        if (!loaded_ || key.empty()) return nullptr;

        const nlohmann::json* node = &root_;
        std::istringstream path(key);
        std::string segment;
        while (std::getline(path, segment, '.'))
        {
            if (segment.empty() || !node->is_object()) return nullptr;
            auto it = node->find(segment);
            if (it == node->end()) return nullptr;
            node = &it.value();
        }
        return node;
    }

    std::string ConfigManager::getString(const std::string& key, const std::string& fallback) const
    {
        const auto* v = find(key);
        return v && v->is_string() ? v->get<std::string>() : fallback;
    }

    int ConfigManager::getInt(const std::string& key, int fallback) const
    {
        const auto* v = find(key);
        return v && v->is_number_integer() ? v->get<int>() : fallback;
    }

    double ConfigManager::getDouble(const std::string& key, double fallback) const
    {
        const auto* v = find(key);
        return v && v->is_number() ? v->get<double>() : fallback;
    }

    bool ConfigManager::getBool(const std::string& key, bool fallback) const
    {
        const auto* v = find(key);
        return v && v->is_boolean() ? v->get<bool>() : fallback;
    }
}