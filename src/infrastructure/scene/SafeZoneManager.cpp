#include "fall_monitoring/infrastructure/scene/SafeZoneManager.hpp"

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace fall_monitoring::infrastructure::scene
{
    using domain::vision::NormalizedPoint;
    using domain::vision::SafeLieZone;

    bool SafeZoneManager::load(const std::string& configPath)
    {
        // 防止重载与 MQTT 热更新同时读取、覆盖同一份场景文件。
        std::lock_guard<std::mutex> mutationLock(configMutationMutex_);
        std::ifstream input(configPath);
        if (!input.is_open())
        {
            SPDLOG_WARN("[SafeZone] 无法打开场景配置：{}；保留当前安全区域", configPath);
            return false;
        }

        std::vector<SafeLieZone> newZones;
        try
        {
            const nlohmann::json root = nlohmann::json::parse(input);
            if (!root.is_object() || !root.contains("safe_lie_zones") || !root.at("safe_lie_zones").is_array())
            {
                SPDLOG_WARN("[SafeZone] 配置缺少 safe_lie_zones 数组：{}", configPath);
                return false;
            }

            for (const auto& zoneJson : root.at("safe_lie_zones"))
            {
                // 保持旧行为：跳过单个非法区域，不影响同一文件的其他区域。
                if (!zoneJson.is_object() || !zoneJson.contains("points"))
                {
                    continue;
                }

                SafeLieZone zone;
                zone.name = zoneJson.value("name", std::string("unnamed_zone"));
                const auto& pointsJson = zoneJson.at("points");
                if (!pointsJson.is_array())
                {
                    continue;
                }

                for (const auto& pointJson : pointsJson)
                {
                    if (!pointJson.is_array() || pointJson.size() != 2)
                    {
                        continue;
                    }
                    try
                    {
                        const float x = pointJson.at(0).get<float>();
                        const float y = pointJson.at(1).get<float>();
                        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
                        {
                            SPDLOG_WARN("[SafeZone] 区域 {} 包含非法坐标 ({}, {})", zone.name, x, y);
                            continue;
                        }
                        zone.points.push_back({x, y});
                    }
                    catch (const std::exception&)
                    {
                        SPDLOG_WARN("[SafeZone] 区域 {} 存在无法解析的坐标", zone.name);
                    }
                }

                if (zone.points.size() < 3)
                {
                    SPDLOG_WARN("[SafeZone] 区域 {} 点数不足，已忽略", zone.name);
                    continue;
                }
                newZones.push_back(std::move(zone));
            }

            // 全部解析完成后才替换内存；读取者只能看到完整旧配置或完整新配置。
            {
                std::unique_lock<std::shared_mutex> lock(zonesMutex_);
                zones_ = std::move(newZones);
            }
            SPDLOG_INFO("[SafeZone] 场景配置加载完成，安全区域={}，文件={}", getZoneCount(), configPath);
            return true;
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[SafeZone] 解析场景配置失败：{}，文件={}", e.what(), configPath);
            return false;
        }
    }

    bool SafeZoneManager::replaceZonesAndSave(const std::vector<SafeLieZone>& zones, const std::string& configPath)
    {
        std::lock_guard<std::mutex> mutationLock(configMutationMutex_);
        for (const auto& zone : zones)
        {
            std::string message;
            if (!validateZone(zone, message))
            {
                SPDLOG_ERROR("[SafeZone] 更新配置非法：name={}, error={}", zone.name, message);
                return false;
            }
        }

        // 先保存文件，成功后再更新内存；避免掉电/写盘失败造成半份 JSON。
        if (!writeConfigFile(zones, configPath))
        {
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(zonesMutex_);
            zones_ = zones;
        }
        SPDLOG_INFO("[SafeZone] 热更新完成，安全区域数量={}", zones.size());
        return true;
    }

    std::vector<SafeLieZone> SafeZoneManager::getZones() const
    {
        std::shared_lock<std::shared_mutex> lock(zonesMutex_);
        return zones_;
    }

    std::size_t SafeZoneManager::getZoneCount() const
    {
        std::shared_lock<std::shared_mutex> lock(zonesMutex_);
        return zones_.size();
    }

    bool SafeZoneManager::isPointInSafeZone(float normalizedX, float normalizedY) const
    {
        if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY))
        {
            return false;
        }
        const NormalizedPoint point{normalizedX, normalizedY};
        std::shared_lock<std::shared_mutex> lock(zonesMutex_);
        for (const auto& zone : zones_)
        {
            if (isPointInPolygon(point, zone.points))
            {
                return true;
            }
        }
        return false;
    }

    bool SafeZoneManager::isPointInPolygon(const NormalizedPoint& point, const std::vector<NormalizedPoint>& polygon)
    {
        if (polygon.size() < 3)
        {
            return false;
        }
        bool inside = false;
        std::size_t j = polygon.size() - 1;
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[j];
            // 旧算法的射线法（含分母微小偏移），不修改边界判断语义。
            const bool intersect = ((a.y > point.y) != (b.y > point.y)) && (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y + 1e-6f) + a.x);
            if (intersect)
            {
                inside = !inside;
            }
            j = i;
        }
        return inside;
    }

    bool SafeZoneManager::validateZone(const SafeLieZone& zone, std::string& errorMessage)
    {
        if (zone.name.empty())
        {
            errorMessage = "区域名称不能为空";
            return false;
        }
        if (zone.points.size() < 3)
        {
            errorMessage = "多边形至少需要 3 个顶点";
            return false;
        }
        for (std::size_t i = 0; i < zone.points.size(); ++i)
        {
            const auto& point = zone.points[i];
            if (!std::isfinite(point.x) || !std::isfinite(point.y))
            {
                errorMessage = "坐标包含非有限数值";
                return false;
            }
            if (point.x < 0.0f || point.x > 1.0f || point.y < 0.0f || point.y > 1.0f)
            {
                errorMessage = "第 " + std::to_string(i + 1) + " 个坐标不在 [0, 1] 范围内";
                return false;
            }
        }
        return true;
    }

    bool SafeZoneManager::writeConfigFile(const std::vector<SafeLieZone>& zones, const std::string& configPath)
    {
        try
        {
            nlohmann::json root;
            root["version"] = 1;
            root["safe_lie_zones"] = nlohmann::json::array();
            for (const auto& zone : zones)
            {
                nlohmann::json zoneJson;
                zoneJson["name"] = zone.name;
                zoneJson["points"] = nlohmann::json::array();
                for (const auto& point : zone.points)
                {
                    zoneJson["points"].push_back({point.x, point.y});
                }
                root["safe_lie_zones"].push_back(std::move(zoneJson));
            }

            const std::filesystem::path target(configPath);
            if (target.has_parent_path())
            {
                std::filesystem::create_directories(target.parent_path());
            }
            const std::filesystem::path temp = target.string() + ".tmp";
            {
                std::ofstream output(temp, std::ios::out | std::ios::trunc);
                if (!output.is_open())
                {
                    SPDLOG_ERROR("[SafeZone] 无法创建临时配置文件：{}", temp.string());
                    return false;
                }
                output << root.dump(2) << '\n';
                output.flush();
                if (!output.good())
                {
                    SPDLOG_ERROR("[SafeZone] 写入临时配置失败：{}", temp.string());
                    return false;
                }
            }

            std::error_code ec;
            // 在同一目录 rename（Linux）原子替换目标路径，避免截断正式文件。
            std::filesystem::rename(temp, target, ec);
            if (ec)
            {
                SPDLOG_ERROR("[SafeZone] 替换场景配置失败：{}", ec.message());
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                return false;
            }
            SPDLOG_INFO("[SafeZone] 场景配置已保存：{}", configPath);
            return true;
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[SafeZone] 保存场景配置失败：{}", e.what());
            return false;
        }
    }

    float SafeZoneManager::calculateBoxOverlapRatio(int boxX, int boxY, int boxWidth, int boxHeight, int frameWidth, int frameHeight) const
    {
        if (boxWidth <= 0 || boxHeight <= 0 || frameWidth <= 0 || frameHeight <= 0)
        {
            return 0.0f;
        }
        std::shared_lock<std::shared_mutex> lock(zonesMutex_);
        if (zones_.empty())
        {
            return 0.0f;
        }

        // 使用 cv::fillPoly + countNonZero，保留旧项目逐像素掩码重叠率算法。
        const int left = std::clamp(boxX, 0, frameWidth);
        const int top = std::clamp(boxY, 0, frameHeight);
        const int right = static_cast<int>(std::clamp(static_cast<long long>(boxX) + boxWidth, 0LL, static_cast<long long>(frameWidth)));
        const int bottom = static_cast<int>(std::clamp(static_cast<long long>(boxY) + boxHeight, 0LL, static_cast<long long>(frameHeight)));
        const int clippedWidth = right - left;
        const int clippedHeight = bottom - top;
        if (clippedWidth <= 0 || clippedHeight <= 0)
        {
            return 0.0f;
        }

        float maxOverlap = 0.0f;
        for (const auto& zone : zones_)
        {
            if (zone.points.size() < 3)
            {
                continue;
            }
            std::vector<cv::Point> polygon;
            polygon.reserve(zone.points.size());
            for (const auto& point : zone.points)
            {
                const int px = static_cast<int>(point.x * static_cast<float>(frameWidth));
                const int py = static_cast<int>(point.y * static_cast<float>(frameHeight));
                polygon.emplace_back(px - left, py - top);
            }
            cv::Mat mask = cv::Mat::zeros(clippedHeight, clippedWidth, CV_8UC1);
            std::vector<std::vector<cv::Point>> polygons;
            polygons.push_back(std::move(polygon));
            cv::fillPoly(mask, polygons, cv::Scalar(255));
            const int intersectionArea = cv::countNonZero(mask);
            const int boxArea = clippedWidth * clippedHeight;
            if (boxArea > 0)
            {
                maxOverlap = std::max(maxOverlap, static_cast<float>(intersectionArea) / static_cast<float>(boxArea));
            }
        }
        return maxOverlap;
    }
} // namespace fall_monitoring::infrastructure::scene
