#include "fall_monitoring/domain/vision/PersonTracker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fall_monitoring::domain::vision
{
    PersonTracker::PersonTracker(PersonTrackerConfig config) : config_(config)
    {
    }

    void PersonTracker::reset() noexcept
    {
        tracks_.clear();
    }

    std::size_t PersonTracker::activeTrackCount() const noexcept
    {
        return tracks_.size();
    }

    const PersonTrackerConfig& PersonTracker::config() const noexcept
    {
        return config_;
    }

    float PersonTracker::calculateIoU(const DetectResult& a, const DetectResult& b) noexcept
    {
        const int x1 = std::max(a.x, b.x);
        const int y1 = std::max(a.y, b.y);
        const int x2 = std::min(a.x + a.width, b.x + b.width);
        const int y2 = std::min(a.y + a.height, b.y + b.height);

        if (x1 >= x2 || y1 >= y2)
        {
            return 0.0f;
        }

        const float intersection = static_cast<float>((x2 -x1) * (y2 - y1));
        const float areaA = static_cast<float>(a.width * a.height);
        const float areaB = static_cast<float>(b.width * b.height);
        const float unionArea = areaA + areaB - intersection;
        return unionArea > 0.0f ? intersection / unionArea : 0.0f;
    }

    float PersonTracker::calculateNormalizedCenterDistance(const DetectResult& a, const DetectResult& b) noexcept
    {
        const float centerAX = static_cast<float>(a.x) + static_cast<float>(a.width) / 2.0f;
        const float centerAY = static_cast<float>(a.y) + static_cast<float>(a.height) / 2.0f;
        const float centerBX = static_cast<float>(b.x) + static_cast<float>(b.width) / 2.0f;
        const float centerBY = static_cast<float>(b.y) + static_cast<float>(b.height) / 2.0f;
        const float dx = centerAX - centerBX;
        const float dy = centerAY - centerBY;
        const float distance = std::sqrt(dx * dx + dy * dy);

        // 使用人体框平均高度归一化。
        const float referenceHeight = std::max((static_cast<float>(a.height) + static_cast<float>(b.height)) / 2.0f, 1.0f);
        return distance / referenceHeight;
    }

    void PersonTracker::removeExpiredTracks(std::chrono::steady_clock::time_point now)
    {
        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
                const auto missingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - track.lastSeen).count();
                return missingMs > config_.maxMissingMs;
            }),
            tracks_.end());
    }

    std::vector<TrackedPerson> PersonTracker::update(const std::vector<DetectResult>& detections)
    {
        const auto now = std::chrono::steady_clock::steady_clock::now();
        removeExpiredTracks(now);

        std::vector<TrackedPerson> results;
        results.reserve(detections.size());

        // 保证同一帧中一个历史 Track 最多关联一个检测框。
        std::vector<bool> trackUsed(tracks_.size(), false);

        for (const auto& detection : detections)
        {
            int bestTrackIndex = -1;
            float bestScore = -std::numeric_limits<float>::infinity();

            for (std::size_t i = 0; i < tracks_.size(); ++i)
            {
                if (trackUsed[i])
                {
                    continue;
                }

                const float iou = calculateIoU(detection, tracks_[i].lastDetection);
                const float centerDistance = calculateNormalizedCenterDistance(detection, tracks_[i].lastDetection);

                if (iou < config_.iouThreshold && centerDistance > config_.centerDistanceThreshold)
                {
                    continue;
                }

                const float score = iou - 0.30f * centerDistance;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestTrackIndex = static_cast<int>(i);
                }
            }

            if (bestTrackIndex >= 0)
            {
                auto& track = tracks_[static_cast<std::size_t>(bestTrackIndex)];
                track.lastDetection = detection;
                track.lastSeen = now;
                trackUsed[static_cast<std::size_t>(bestTrackIndex)] = true;
                results.push_back(TrackedPerson{track.id, detection});
                continue;
            }

            Track newTrack;
            newTrack.id = nextTrackId_++;
            newTrack.lastDetection = detection;
            newTrack.lastSeen = now;
            tracks_.push_back(std::move(newTrack));
            trackUsed.push_back(true);
            results.push_back(TrackedPerson{tracks_.back().id, detection});
        }
        return results;
    }
}