#pragma once

#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace fall_monitoring::application::ports
{
    struct PendingVideoUpload
    {
        std::string eventId;
        std::string videoPath;
        int retryCount = 0;
    };

    // 必须收到云端 status=SAVED 才允许标记 Uploaded。
    enum class AckCommitResult { Uploaded, Stale, Error };
    enum class AlertCloudState { Pending, Uploaded, Missing, Error };
    enum class VideoCommitResult { Uploaded, Stale, Error };

    /**
     * @brief 本地报警仓储契约（Repository）。
     * 所有方法可从 MQTT/录像/事件/HTTP 不同线程调用，实现方需串行化数据库访问。
     * 查询返回 false 代表数据库错误，返回 true 且结果为空代表正常无任务。
     */
    class IAlertRepository
    {
    public:
        virtual ~IAlertRepository() = default;

        [[nodiscard]] virtual bool saveAlert(const domain::event::AlertEvent& event) = 0;
        [[nodiscard]] virtual bool loadPendingAlerts(std::vector<domain::event::AlertEvent>& result, std::size_t limit = 100, std::size_t offset = 0) = 0;
        [[nodiscard]] virtual bool markAsUploaded(const std::string& eventId) = 0;
        [[nodiscard]] virtual AckCommitResult markAsUploadedIfMatches(const domain::event::AlertEvent& sentSnapshot) = 0;

        [[nodiscard]] virtual bool markVideoPending(const std::string& eventId, const std::string& videoPath) = 0;
        [[nodiscard]] virtual bool markVideoFailed(const std::string& eventId) = 0;
        [[nodiscard]] virtual bool loadPendingVideos(std::vector<PendingVideoUpload>& result, std::size_t limit = 100, std::size_t offset = 0) = 0;
        [[nodiscard]] virtual bool markVideoUploaded(const std::string& eventId) = 0;
        [[nodiscard]] virtual bool incrementVideoRetry(const std::string& eventId) = 0;

        // 视频上传前确认报警已收到后端 SAVED ACK。
        [[nodiscard]] virtual AlertCloudState getAlertCloudState(const std::string& eventId) = 0;
        // CAS：避免过期上传响应将新录像路径标记为已上传。
        [[nodiscard]] virtual VideoCommitResult markVideoUploadedIfMatches(const std::string& eventId, const std::string& videoPath) = 0;
        [[nodiscard]] virtual bool incrementVideoRetryIfMatches(const std::string& eventId, const std::string& videoPath) = 0;
    };
}