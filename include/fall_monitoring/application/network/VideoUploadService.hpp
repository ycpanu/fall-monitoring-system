#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"
#include "fall_monitoring/application/ports/IVideoUploadTransport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace fall_monitoring::application::network
{
    struct VideoUploadOptions
    {
        int retryInitialSeconds = 5;
        int retryMaxSeconds = 300;
        int scanSeconds = 2;
        std::size_t batchSize = 100;
    };

    /**
     * @brief 单工作线程视频补传服务。
     * SQLite 是任务事实来源，重启后读取 PENDING 继续上传；网络和数据库错误不会删除任务。
     * 录像上传前必须确认该 event_id 的报警已被云端业务 ACK。
     * 线程职责：查询 PENDING -> 检查报警状态 -> HTTP 上传 -> SQLite CAS 更新。
     */
    class VideoUploadService final
    {
    public:
        using ConnectivityProvider = std::function<bool()>;

        VideoUploadService(VideoUploadOptions options, ports::IAlertRepository& repository, ports::IVideoUploadTransport& transport, ConnectivityProvider isOnline);
        ~VideoUploadService();

        VideoUploadService(const VideoUploadService&) = delete;
        VideoUploadService& operator=(const VideoUploadService&) = delete;

        [[nodiscard]] bool start();
        [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
        void notifyNewTask() noexcept;
        void stop() noexcept;

    private:
        using Clock = std::chrono::steady_clock;
        void run() noexcept;
        [[nodiscard]] std::chrono::seconds retryDelay(int retryCount) const noexcept;

        VideoUploadOptions options_;
        ports::IAlertRepository& repository_;
        ports::IVideoUploadTransport& transport_;
        ConnectivityProvider isOnline_;
        std::atomic<bool> running_{false};
        std::atomic<bool> startedOnce_{false};
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool wakeRequested_ = false;
        std::unordered_map<std::string, Clock::time_point> nextAttempt_;
        std::size_t scanOffset_ = 0;
        std::size_t recoveryOffset_ = 0;
    };
}
