#pragma once

#include "fall_monitoring/domain/vision/PoseDetection.hpp"

#include <chrono>
#include <cstddef>
#include <vector>

namespace fall_monitoring::domain::vision
{
    /**
     * @brief 一帧内已分配短时跟踪编号的人体姿态
     */
    struct TrackedPerson
    {
        int trackId = -1;
        DetectResult detection;
    };

    struct PersonTrackerConfig
    {
        float iouThreshold = 0.20f;

        // 两个人体框中心距离/两框平均高度
        float centerDistanceThreshold = 0.80f;

        // 短时遮挡或漏检的目标保留时长。
        int maxMissingMs = 1500;
    };

    /**
     * @brief 基于检测框 IoU + 归一化中心距离的轻量人体跟踪器。
     */
    class PersonTracker final 
    {
    public:
        explicit PersonTracker(PersonTrackerConfig config = {});

        [[nodiscard]] std::vector<TrackedPerson> update(const std::vector<DetectResult>& detection);

        /**
         * @brief 清除轨迹状态，保留 ID 计数器，避免重复编号。
         */
        void reset() noexcept;

        [[nodiscard]] std::size_t activeTrackCount() const noexcept;
        [[nodiscard]] const PersonTrackerConfig& config() const noexcept;

    private:
        struct Track
        {
            int id = -1;
            DetectResult lastDetection;
            std::chrono::steady_clock::time_point lastSeen;
        };

        [[nodiscard]] static float calculateIoU(const DetectResult& a, const DetectResult& b) noexcept;
        [[nodiscard]] static float calculateNormalizedCenterDistance(const DetectResult& a, const DetectResult& b) noexcept;

        void removeExpiredTracks(std::chrono::steady_clock::time_point now);

        PersonTrackerConfig config_;
        std::vector<Track> tracks_;
        int nextTrackId_ = 1;
    };
}