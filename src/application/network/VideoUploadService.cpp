#include "fall_monitoring/application/network/VideoUploadService.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fall_monitoring::application::network
{
    VideoUploadService::VideoUploadService(VideoUploadOptions options, ports::IAlertRepository& repository, ports::IVideoUploadTransport& transport, ConnectivityProvider isOnline)
        : options_(std::move(options)), repository_(repository), transport_(transport), isOnline_(std::move(isOnline))
    {
        if (!isOnline_ || options_.retryInitialSeconds <= 0 ||
            options_.retryMaxSeconds < options_.retryInitialSeconds ||
            options_.scanSeconds <= 0 || options_.batchSize == 0 || options_.batchSize > 1000)
            throw std::invalid_argument("VideoUpload: 补传参数或网络状态提供器无效");
    }

    VideoUploadService::~VideoUploadService() { stop(); }

    bool VideoUploadService::start()
    {
        if (running_.load()) return true;
        if (worker_.joinable()) worker_.join();
        startedOnce_.store(true);
        running_.store(true);
        try { worker_ = std::thread(&VideoUploadService::run, this); }
        catch (const std::exception& e)
        {
            running_.store(false);
            SPDLOG_ERROR("[VideoUpload] 无法创建上传线程：{}", e.what());
            return false;
        }
        SPDLOG_INFO("[VideoUpload] 上传线程启动：scan={}s, retry={}..{}s, batch={}",
                    options_.scanSeconds, options_.retryInitialSeconds,
                    options_.retryMaxSeconds, options_.batchSize);
        return true;
    }

    void VideoUploadService::notifyNewTask() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wakeRequested_ = true;
        }
        cv_.notify_one();
    }

    std::chrono::seconds VideoUploadService::retryDelay(int retryCount) const noexcept
    {
        // 每失败一次指数退避，最长 300 秒；数据库重试次数在重启后仍有效。
        const int exponent = std::clamp(retryCount - 1, 0, 12);
        const auto factor = static_cast<long long>(1) << exponent;
        const auto delay = std::min(static_cast<long long>(options_.retryInitialSeconds) * factor, static_cast<long long>(options_.retryMaxSeconds));
        return std::chrono::seconds(delay);
    }

    void VideoUploadService::run() noexcept
    {
        bool wasOnline = false;
        SPDLOG_INFO("[VideoUpload] 工作线程开始扫描待上传录像");
        while (running_.load())
        {
            try
            {
                // 每轮检查少量已生成 MP4 与 SQLite 状态，修复回调落库失败或异常重启的遗漏。
                std::size_t scanned = 0;
                if (repository_.reconcileRecordedVideos(options_.batchSize, recoveryOffset_, scanned))
                    recoveryOffset_ = scanned == options_.batchSize ? recoveryOffset_ + scanned : 0;
                else
                {
                    SPDLOG_ERROR("[VideoUpload] 录像状态恢复扫描失败");
                    recoveryOffset_ = 0;
                }

                const bool online = isOnline_();
                if (online && !wasOnline)
                {
                    // 重连时立即重试，避免仍等待上次离线前的退避时间。
                    nextAttempt_.clear();
                    scanOffset_ = 0;
                    SPDLOG_INFO("[VideoUpload] 网络已恢复，开始扫描 SQLite 待上传视频");
                }
                if (!online && wasOnline)
                    SPDLOG_WARN("[VideoUpload] MQTT 连接中断，暂停 HTTP 视频上传");
                wasOnline = online;

                if (online)
                {
                    std::vector<ports::PendingVideoUpload> pending;
                    if (!repository_.loadPendingVideos(pending, options_.batchSize, scanOffset_))
                    {
                        SPDLOG_ERROR("[VideoUpload] 查询 SQLite 待上传视频失败，稍后重试");
                        scanOffset_ = 0;
                    }
                    else
                    {
                        if (pending.size() == options_.batchSize)
                            scanOffset_ += pending.size();
                        else scanOffset_ = 0;

                        for (const auto& video : pending)
                        {
                            if (!running_.load() || !isOnline_()) break;
                            if (video.eventId.empty() || video.videoPath.empty()) continue;

                            const auto due = nextAttempt_.find(video.eventId);
                            if (due != nextAttempt_.end() && Clock::now() < due->second)
                                continue;

                            const auto cloud = repository_.getAlertCloudState(video.eventId);
                            if (cloud == ports::AlertCloudState::Pending)
                            {
                                // 要求事件先入库再上传 MP4：未收到 SAVED 业务 ACK 不发视频。
                                continue;
                            }
                            if (cloud != ports::AlertCloudState::Uploaded)
                            {
                                SPDLOG_ERROR("[VideoUpload] 无法确认云端报警状态：event_id={}，state={}", video.eventId, static_cast<int>(cloud));
                                nextAttempt_[video.eventId] = Clock::now() + std::chrono::seconds(options_.retryInitialSeconds);
                                continue;
                            }

                            SPDLOG_INFO("[VideoUpload] 开始上传：event_id={}, retry={}, path={}", video.eventId, video.retryCount, video.videoPath);
                            const auto result = transport_.upload(video);
                            if (result.success)
                            {
                                const auto commit = repository_.markVideoUploadedIfMatches(video.eventId, video.videoPath);
                                if (commit == ports::VideoCommitResult::Uploaded)
                                {
                                    SPDLOG_INFO("[VideoUpload] 后端确认并完成本地状态更新：event_id={}", video.eventId);
                                    nextAttempt_.erase(video.eventId);
                                    continue;
                                }
                                if (commit == ports::VideoCommitResult::Stale)
                                {
                                    // 录像可能已被其他线程替换，旧 HTTP 结果不能确认新视频。
                                    SPDLOG_WARN("[VideoUpload] 上传后录像状态已改变，跳过旧版本确认：{}", video.eventId);
                                    nextAttempt_.erase(video.eventId);
                                    continue;
                                }
                                SPDLOG_ERROR("[VideoUpload] HTTP 上传成功但 SQLite 更新失败：{}", video.eventId);
                            }
                            else
                            {
                                SPDLOG_WARN("[VideoUpload] 上传失败：event_id={}，HTTP={}，reason={}", video.eventId, result.httpStatus, result.detail);
                            }

                            if (!repository_.incrementVideoRetryIfMatches(video.eventId, video.videoPath))
                                SPDLOG_ERROR("[VideoUpload] 无法持久化重试次数（或录像记录已变化）：{}", video.eventId);
                            nextAttempt_[video.eventId] = Clock::now() + retryDelay(video.retryCount + 1);
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("[VideoUpload] 补传循环异常，SQLite 任务仍保留：{}", e.what());
            }
            catch (...)
            {
                SPDLOG_ERROR("[VideoUpload] 补传循环发生未知异常，SQLite 任务仍保留");
            }

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(options_.scanSeconds), [this] {
                return !running_.load() || wakeRequested_;
            });
            wakeRequested_ = false;
        }
        running_.store(false);
        SPDLOG_INFO("[VideoUpload] 上传工作线程退出");
    }

    void VideoUploadService::stop() noexcept
    {
        running_.store(false);
        notifyNewTask();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id())
            {
                SPDLOG_CRITICAL("[VideoUpload] 禁止在上传线程中停止自身");
                std::terminate();
            }
            worker_.join();
            SPDLOG_INFO("[VideoUpload] 已停止，SQLite 未确认任务继续保留");
        }
    }
}
