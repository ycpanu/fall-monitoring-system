#pragma once

#include "fall_monitoring/domain/event/AlertEvent.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 尚未上传的视频任务，该结构是业务数据。
     */
    struct PendingVideoUpload
    {
        std::string eventId;
        std::string videoPath;
        int retryCount = 0;
    };

    /**
     * @brief 本次报警仓储端口
     */
    class IAlertRepository
    {
    public:
        virtual ~IAlertRepository() = default;
        
        [[nodiscard]] virtual bool saveAlert(const domain::event::AlertEvent& event) = 0;
        [[nodiscard]] virtual bool loadPendingAlerts(std::vector<domain::event::AlertEvent>& result, std::size_t limit = 100) = 0;
        [[nodiscard]] virtual bool markAsUploaded(const std::string& eventId) = 0;

        [[nodiscard]] virtual bool markVideoPending(const std::string& eventId, const std::string& videoPath) = 0;
        [[nodiscard]] virtual bool loadPendingVideos(std::vector<PendingVideoUpload>& result, std::size_t limit = 100) = 0;
        [[nodiscard]] virtual bool markVideoUploaded(const std::string& eventId) = 0;
        [[nodiscard]] virtual bool incrementVideoRetry(const std::string& eventId) = 0;
    }
}