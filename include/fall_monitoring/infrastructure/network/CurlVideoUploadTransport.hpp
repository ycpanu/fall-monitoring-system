#pragma once

#include "fall_monitoring/application/ports/IVideoUploadTransport.hpp"

#include <string>

namespace fall_monitoring::infrastructure::network
{
    struct CurlVideoUploadOptions
    {
        std::string apiBaseUrl;
        int connectTimeoutSeconds = 5;
        int timeoutSeconds = 60;
    };

    /**
     * @brief FastAPI /api/upload/video 上传适配器。
     * 使用 libcurl multipart 表单 file，不执行 shell 命令，不依赖 MQTT 连接细节。
     * 本对象只供 VideoUploadService 的单个工作线程调用。
     */
    class CurlVideoUploadTransport final : public application::ports::IVideoUploadTransport
    {
    public:
        explicit CurlVideoUploadTransport(CurlVideoUploadOptions options);
        ~CurlVideoUploadTransport() override;

        CurlVideoUploadTransport(const CurlVideoUploadTransport&) = delete;
        CurlVideoUploadTransport& operator=(const CurlVideoUploadTransport&) = delete;

        [[nodiscard]] application::ports::VideoUploadResult upload(const application::ports::PendingVideoUpload& video) override;

    private:
        CurlVideoUploadOptions options_;
        bool curlInitialized_ = false;
    };
}
