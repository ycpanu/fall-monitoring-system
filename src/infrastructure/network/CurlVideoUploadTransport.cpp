#include "fall_monitoring/infrastructure/network/CurlVideoUploadTransport.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace fall_monitoring::infrastructure::network
{
    namespace
    {
        constexpr std::size_t MAX_RESPONSE_BYTES = 64 * 1024;

        // 限制响应大小：避免异常服务端占用大量设备内存。
        std::size_t collectResponse(char* buffer, std::size_t size, std::size_t count, void* user) noexcept
        {
            if (user == nullptr || (size != 0 && count > std::numeric_limits<std::size_t>::max() / size))
                return 0;
            const std::size_t bytes = size * count;
            auto& response = *static_cast<std::string*>(user);
            if (bytes > MAX_RESPONSE_BYTES - response.size()) return 0;
            try
            {
                response.append(buffer, bytes);
            }
            catch (...) { return 0; }
            return bytes;
        }
    }

    CurlVideoUploadTransport::CurlVideoUploadTransport(CurlVideoUploadOptions options) : options_(std::move(options))
    {
        while (!options_.apiBaseUrl.empty() && options_.apiBaseUrl.back() == '/')
            options_.apiBaseUrl.pop_back();
        if (options_.apiBaseUrl.empty() ||
            (options_.apiBaseUrl.rfind("http://", 0) != 0 && options_.apiBaseUrl.rfind("https://", 0) != 0) ||
            options_.connectTimeoutSeconds <= 0 || options_.timeoutSeconds <= 0 ||
            options_.connectTimeoutSeconds > options_.timeoutSeconds)
            throw std::invalid_argument("VideoUpload: FastAPI URL 或超时参数非法");

        const auto rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("VideoUpload: libcurl 初始化失败: ") + curl_easy_strerror(rc));
        curlInitialized_ = true;
    }

    CurlVideoUploadTransport::~CurlVideoUploadTransport()
    {
        if (curlInitialized_) curl_global_cleanup();
    }

    application::ports::VideoUploadResult CurlVideoUploadTransport::upload(
        const application::ports::PendingVideoUpload& video)
    {
        using application::ports::VideoUploadResult;
        namespace fs = std::filesystem;
        if (video.eventId.empty() || video.videoPath.empty())
            return {false, 0, "事件 ID 或录像路径为空"};

        // 旧 FastAPI 按文件名解析 event_id，而不是通过另一个表单字段传入。
        const std::string expectedName = "fall_" + video.eventId + ".mp4";
        const fs::path path(video.videoPath);
        if (path.filename().string() != expectedName)
            return {false, 0, "录像文件名与 event_id 不匹配"};
        std::error_code error;
        if (!fs::is_regular_file(path, error) || error)
            return {false, 0, "本地录像文件不存在或不是普通文件"};
        if (fs::file_size(path, error) == 0 || error)
            return {false, 0, "本地录像文件为空或无法读取"};

        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
        if (!curl) return {false, 0, "创建 HTTP 会话失败"};

        using MimeHandle = std::unique_ptr<curl_mime, decltype(&curl_mime_free)>;
        MimeHandle multipart(curl_mime_init(curl.get()), &curl_mime_free);
        if (!multipart) return {false, 0, "创建 multipart 表单失败"};
        curl_mimepart* file = curl_mime_addpart(multipart.get());
        if (file == nullptr || curl_mime_name(file, "file") != CURLE_OK ||
            curl_mime_filedata(file, video.videoPath.c_str()) != CURLE_OK ||
            curl_mime_filename(file, expectedName.c_str()) != CURLE_OK)
            return {false, 0, "添加 MP4 multipart 文件失败"};

        std::string response;
        std::array<char, CURL_ERROR_SIZE> curlError{};
        const std::string url = options_.apiBaseUrl + "/api/upload/video";
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, multipart.get());
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(options_.connectTimeoutSeconds));
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(options_.timeoutSeconds));
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, collectResponse);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError.data());

        const CURLcode rc = curl_easy_perform(curl.get());
        long httpStatus = 0;
        (void)curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);
        if (rc != CURLE_OK)
        {
            const std::string detail = curlError[0] != '\0' ? std::string(curlError.data()) : std::string(curl_easy_strerror(rc));
            return {false, httpStatus, detail};
        }
        if (httpStatus != 200)
        {
            // 404 常表示 MQTT 报警尚未在云端入库；外层等待并重试。
            return {false, httpStatus, "服务器未确认录像上传"};
        }

        const auto json = nlohmann::json::parse(response, nullptr, false);
        if (json.is_discarded() || !json.is_object() ||
            !json.contains("code") || !json["code"].is_number_integer() ||
            json["code"].get<int>() != 200 ||
            !json.contains("event_id") || !json["event_id"].is_string() ||
            json["event_id"].get<std::string>() != video.eventId ||
            !json.contains("video_url") || !json["video_url"].is_string() ||
            json["video_url"].get<std::string>().empty())
        {
            return {false, httpStatus, "上传响应缺少有效的 code/event_id/video_url"};
        }
        return {true, httpStatus, {}};
    }
}
