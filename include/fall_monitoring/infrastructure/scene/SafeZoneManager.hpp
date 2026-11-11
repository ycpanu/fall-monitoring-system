#pragma once

#include "fall_monitoring/application/ports/ISafeZoneConfigurator.hpp"
#include "fall_monitoring/domain/vision/ISafeZoneQuery.hpp"
#include "fall_monitoring/domain/vision/SafeLieZone.hpp"

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace fall_monitoring::infrastructure::scene
{
    /**
     * @brief 场景安全区域适配器。
     *
     * 负责对接旧项目的 scene_config.json、OpenCV 多边形掩码，
     * 对领域层仅暴露 ISafeZoneQuery 两个只读查询接口。
     *
     * 线程约定：
     *   - VisionPipeline 高频调用只读查询；使用 shared_lock。
     *   - MQTT 配置处理线程未来调用 replaceZonesAndSave()；使用 unique_lock。
     *   - configMutationMutex_ 序列化加载与热更新，防止两个更新交错落盘。
     *
     * 不持有 FallRuleEngine；由 Application 负责按生命周期注入。
     */
    class SafeZoneManager final : public domain::vision::ISafeZoneQuery, public application::ports::ISafeZoneConfigurator
    {
    public:
        SafeZoneManager() = default;
        SafeZoneManager(const SafeZoneManager&) = delete;
        SafeZoneManager& operator=(const SafeZoneManager&) = delete;

        /**
         * @brief 加载 JSON 场景配置。
         * @return 文件不存在或配置结构非法时返回 false，保留已有内存配置。
         * @note 单个非法区域可以被忽略，其他合法区域仍会加载（保持旧行为）。
         */
        [[nodiscard]] bool load(const std::string& configPath);

        /**
         * @brief 将服务端下发的区域写入配置文件，并热更新内存。
         *
         * 先全部校验，再同目录临时文件写盘、rename，最后替换内存数据；
         * 任何校验/文件错误都不修改原有区域配置。
         */
        [[nodiscard]] bool replaceZonesAndSave(const std::vector<domain::vision::SafeLieZone>& zones, const std::string& configPath) override;

        [[nodiscard]] std::vector<domain::vision::SafeLieZone> getZones() const;
        [[nodiscard]] std::size_t getZoneCount() const override;

        [[nodiscard]] bool isPointInSafeZone(float normalizedX, float normalizedY) const override;

        [[nodiscard]] float calculateBoxOverlapRatio(int boxX, int boxY, int boxWidth, int boxHeight, int frameWidth, int frameHeight) const override;

    private:
        static bool isPointInPolygon(const domain::vision::NormalizedPoint& point, const std::vector<domain::vision::NormalizedPoint>& polygon);

        static bool validateZone(const domain::vision::SafeLieZone& zone, std::string& errorMessage);

        static bool writeConfigFile(const std::vector<domain::vision::SafeLieZone>& zones, const std::string& configPath);

        // 控制多线程写入顺序；与读取 zones_ 的读写锁分开。
        mutable std::mutex configMutationMutex_;
        mutable std::shared_mutex zonesMutex_;
        std::vector<domain::vision::SafeLieZone> zones_;
    };
} // namespace fall_monitoring::infrastructure::scene
