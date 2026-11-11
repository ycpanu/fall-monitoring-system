#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"

#include <string>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 单次视频上传结果。只有后端 HTTP 200 且返回的 event_id 对应当前事件才算成功。
     * 失败是否重试由 VideoUploadService 统一决定，HTTP 适配器不操作 SQLite。
     */
    struct VideoUploadResult
    {
        bool success = false;
        long httpStatus = 0;
        std::string detail;
    };

    class IVideoUploadTransport
    {
    public:
        virtual ~IVideoUploadTransport() = default;
        [[nodiscard]] virtual VideoUploadResult upload(const PendingVideoUpload& video) = 0;
    };
}
