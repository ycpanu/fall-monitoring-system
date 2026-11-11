#include "fall_monitoring/infrastructure/video/FfmpegEventRecorder.hpp"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fall_monitoring::infrastructure::video
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        namespace fs = std::filesystem;

        struct ChildProcess
        {
            pid_t pid = -1;
            int writer = -1;
        };

        // 不使用 shell，防止包含空格的本地路径被错误解释为命令参数。
        ChildProcess startFfmpeg(const EventVideoOptions& o, const std::string& tempPath)
        {
            int fds[2]{-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            {
                SPDLOG_ERROR("[VideoRecord] FFmpeg 输入套接字创建失败：{}", std::strerror(errno));
                return {};
            }

            posix_spawn_file_actions_t actions;
            const int initRc = ::posix_spawn_file_actions_init(&actions);
            if (initRc != 0)
            {
                ::close(fds[0]); ::close(fds[1]);
                SPDLOG_ERROR("[VideoRecord] posix_spawn 初始化失败：{}", std::strerror(initRc));
                return {};
            }
            ::posix_spawn_file_actions_adddup2(&actions, fds[1], STDIN_FILENO);
            ::posix_spawn_file_actions_addclose(&actions, fds[0]);
            if (fds[1] != STDIN_FILENO)
                ::posix_spawn_file_actions_addclose(&actions, fds[1]);

            std::vector<std::string> args{
                o.ffmpegBinary, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                "-f", "rawvideo", "-pixel_format", "bgr24",
                "-video_size", std::to_string(o.width) + "x" + std::to_string(o.height),
                "-framerate", std::to_string(o.fps), "-i", "pipe:0",
                "-an", "-vf", "format=nv12", "-c:v", o.encoder,
                "-b:v", std::to_string(o.bitrateKbps) + "k",
                "-movflags", "+faststart", "-f", "mp4", tempPath
            };
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args) argv.push_back(arg.data());
            argv.push_back(nullptr);

            pid_t pid = -1;
            const int rc = ::posix_spawnp(&pid, o.ffmpegBinary.c_str(), &actions, nullptr, argv.data(), environ);
            ::posix_spawn_file_actions_destroy(&actions);
            ::close(fds[1]);
            if (rc != 0)
            {
                ::close(fds[0]);
                SPDLOG_ERROR("[VideoRecord] 启动 FFmpeg 失败：{}", std::strerror(rc));
                return {};
            }

            const int flags = ::fcntl(fds[0], F_GETFL, 0);
            if (flags < 0 || ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) != 0)
            {
                SPDLOG_ERROR("[VideoRecord] 设置非阻塞视频管道失败");
                ::close(fds[0]);
                (void)::kill(pid, SIGKILL);
                (void)::waitpid(pid, nullptr, 0);
                return {};
            }
            return {pid, fds[0]};
        }

        // 每一帧的完整投递最多等待 3 秒；FFmpeg 卡住不会无限阻塞停机。
        bool writeFrame(int fd, const cv::Mat& frame)
        {
            const auto* data = frame.ptr<unsigned char>();
            const std::size_t size = frame.total() * frame.elemSize();
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            std::size_t offset = 0;
            while (offset < size && Clock::now() < deadline)
            {
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                const int ready = ::poll(&pfd, 1, 100);
                if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
                if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
                const ssize_t n = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n > 0) { offset += static_cast<std::size_t>(n); continue; }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                return false;
            }
            return offset == size;
        }

        // 关闭输入后等待 FFmpeg 完成 MP4 moov 写入；异常时有界 TERM/KILL。
        bool waitEncoder(pid_t pid) noexcept
        {
            if (pid <= 0) return false;
            int status = 0;
            bool reaped = false;
            auto waitFor = [&](std::chrono::milliseconds duration) -> bool
            {
                const auto end = Clock::now() + duration;
                do
                {
                    const pid_t got = ::waitpid(pid, &status, WNOHANG);
                    if (got == pid) { reaped = true; return true; }
                    if (got < 0 && errno == EINTR) continue;
                    if (got < 0) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } while (Clock::now() < end);
                return false;
            };
            if (waitFor(std::chrono::seconds(10)))
                return reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
            (void)::kill(pid, SIGTERM);
            if (waitFor(std::chrono::seconds(2))) return false;
            (void)::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }

        bool syncFile(const fs::path& path)
        {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) return false;
            const bool okay = ::fsync(fd) == 0;
            ::close(fd);
            return okay;
        }

        bool syncDirectory(const fs::path& path)
        {
            const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) return false;
            const bool okay = ::fsync(fd) == 0;
            ::close(fd);
            return okay;
        }
    }

    FfmpegEventRecorder::FfmpegEventRecorder(EventVideoOptions options, CompletionHandler onCompleted) : options_(std::move(options)), onCompleted_(std::move(onCompleted))
    {
        if (options_.width <= 0 || options_.height <= 0 || options_.fps <= 0 ||
            options_.bitrateKbps <= 0 || options_.preFrames <= 0 ||
            options_.postFrames < 0 || options_.maxOutstanding == 0 ||
            options_.ffmpegBinary.empty() || options_.encoder.empty() || !onCompleted_)
            throw std::invalid_argument("视频录制参数非法或未提供完成回调");
        worker_ = std::thread(&FfmpegEventRecorder::encodingLoop, this);
        SPDLOG_INFO("[VideoRecord] 缓存就绪：{}x{}, pre={}, post={}, maxOutstanding={}", options_.width, options_.height, options_.preFrames, options_.postFrames, options_.maxOutstanding);
    }

    FfmpegEventRecorder::~FfmpegEventRecorder() { stopAndDrain(); }

    void FfmpegEventRecorder::observeFrame(const cv::Mat& frame) noexcept
    {
        if (frame.empty()) return;
        try
        {
            // 所有缓冲均保持独立、连续的 BGR24 像素；编码线程只读，不会改写原图。
            cv::Mat bgr;
            if (frame.type() == CV_8UC3) bgr = frame;
            else if (frame.type() == CV_8UC4) cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
            else if (frame.type() == CV_8UC1) cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
            else return;

            cv::Mat stored;
            if (bgr.cols != options_.width || bgr.rows != options_.height)
                cv::resize(bgr, stored, cv::Size(options_.width, options_.height));
            else
                stored = bgr.clone();
            if (!stored.isContinuous()) stored = stored.clone();

            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closing_) return;
                history_.push_back(stored);
                if (history_.size() > static_cast<std::size_t>(options_.preFrames))
                    history_.pop_front();

                for (auto it = pending_.begin(); it != pending_.end();)
                {
                    it->task.frames.push_back(stored);
                    if (--it->remaining <= 0)
                    {
                        ready_.push_back(std::move(it->task));
                        it = pending_.erase(it);
                        notify = true;
                    }
                    else ++it;
                }
            }
            if (notify) available_.notify_one();
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[VideoRecord] 缓存采集帧失败：{}", e.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("[VideoRecord] 缓存采集帧失败：未知异常");
        }
    }

    bool FfmpegEventRecorder::requestRecording(const std::string& eventId, const std::string& outputPath)
    {
        if (eventId.empty() || outputPath.empty() || std::filesystem::path(outputPath).extension() != ".mp4")
        {
            SPDLOG_ERROR("[VideoRecord] 拒绝录像请求：event_id 或 MP4 路径非法");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return false;
        if (outstandingIds_.count(eventId) != 0 || completedIds_.count(eventId) != 0)
            return true; // 跨模态融合更新不得重复生成同一事件录像。
        if (outstandingIds_.size() >= options_.maxOutstanding)
        {
            SPDLOG_ERROR("[VideoRecord] 编码/采集任务已满，拒绝录像：event_id={}", eventId);
            return false;
        }

        Pending pending;
        pending.task.eventId = eventId;
        pending.task.outputPath = outputPath;
        pending.task.frames = history_; // cv::Mat 引用计数，避免重复复制整段预录像素。
        pending.remaining = options_.postFrames;
        outstandingIds_.insert(eventId);
        if (pending.remaining <= 0)
        {
            ready_.push_back(std::move(pending.task));
            available_.notify_one();
        }
        else
        {
            pending_.push_back(std::move(pending));
        }
        SPDLOG_INFO("[VideoRecord] 已接收录像任务：event_id={}, pre_frames={}, post_frames={}", eventId, history_.size(), options_.postFrames);
        return true;
    }

    bool FfmpegEventRecorder::encodeToMp4(const Task& task) const
    {
        if (task.frames.empty())
        {
            SPDLOG_ERROR("[VideoRecord] 无可用视频帧：event_id={}", task.eventId);
            return false;
        }
        namespace fs = std::filesystem;
        const fs::path finalPath(task.outputPath);
        const fs::path tmpPath = finalPath.parent_path() / (finalPath.stem().string() + ".part.mp4");
        std::error_code ec;
        const fs::path parent = finalPath.parent_path().empty() ? fs::path(".") : finalPath.parent_path();
        fs::create_directories(parent, ec);
        if (ec)
        {
            SPDLOG_ERROR("[VideoRecord] 无法创建录像目录：{}", ec.message());
            return false;
        }
        // 已完成且非空的文件不应被融合更新或设备重启重新覆盖。
        if (fs::is_regular_file(finalPath, ec) && !ec && fs::file_size(finalPath, ec) > 0 && !ec)
        {
            SPDLOG_INFO("[VideoRecord] 录像已存在，复用文件：{}", finalPath.string());
            return true;
        }
        fs::remove(tmpPath, ec);
        ec.clear();

        const ChildProcess child = startFfmpeg(options_, tmpPath.string());
        if (child.pid < 0) return false;

        bool written = true;
        for (const auto& frame : task.frames)
        {
            if (frame.empty() || frame.type() != CV_8UC3 || frame.cols != options_.width || frame.rows != options_.height || !frame.isContinuous() || !writeFrame(child.writer, frame))
            {
                written = false;
                SPDLOG_ERROR("[VideoRecord] 写入 FFmpeg 原始帧失败：event_id={}", task.eventId);
                break;
            }
        }
        ::close(child.writer); // EOF 通知 FFmpeg 完成 MP4 封装。
        const bool exited = waitEncoder(child.pid);
        if (!written || !exited)
        {
            SPDLOG_ERROR("[VideoRecord] 编码进程失败：event_id={}, exited={}", task.eventId, exited);
            fs::remove(tmpPath, ec);
            return false;
        }

        ec.clear();
        if (!fs::is_regular_file(tmpPath, ec) || ec || fs::file_size(tmpPath, ec) == 0 || ec || !syncFile(tmpPath))
        {
            SPDLOG_ERROR("[VideoRecord] 输出文件为空或磁盘同步失败：event_id={}", task.eventId);
            fs::remove(tmpPath, ec);
            return false;
        }
        fs::rename(tmpPath, finalPath, ec);
        if (ec || !syncDirectory(parent))
        {
            SPDLOG_ERROR("[VideoRecord] MP4 提交失败：event_id={}, reason={}", task.eventId, ec.message());
            return false;
        }
        return true;
    }

    void FfmpegEventRecorder::reportCompletion(const Task& task, bool success) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            outstandingIds_.erase(task.eventId);
            if (success)
            {
                completedIds_.insert(task.eventId);
                completedOrder_.push_back(task.eventId);
                if (completedOrder_.size() > 256)
                {
                    completedIds_.erase(completedOrder_.front());
                    completedOrder_.pop_front();
                }
            }
        }
        try
        {
            onCompleted_(task.eventId, task.outputPath, success);
        }
        catch (const std::exception& e)
        {
            SPDLOG_CRITICAL("[VideoRecord] 录像完成通知异常：event_id={}, error={}", task.eventId, e.what());
        }
        catch (...)
        {
            SPDLOG_CRITICAL("[VideoRecord] 录像完成通知异常：event_id={}", task.eventId);
        }
    }

    void FfmpegEventRecorder::encodingLoop() noexcept
    {
        SPDLOG_INFO("[VideoRecord] FFmpeg 顺序编码线程开始");
        for (;;)
        {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                available_.wait(lock, [this] { return closing_ || !ready_.empty(); });
                if (ready_.empty()) break; // 只有 closing_ 才能走到这里。
                task = std::move(ready_.front());
                ready_.pop_front();
            }
            bool success = false;
            const auto began = Clock::now();
            try
            {
                success = encodeToMp4(task);
            }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("[VideoRecord] 录像编码异常：event_id={}, error={}", task.eventId, e.what());
            }
            catch (...)
            {
                SPDLOG_ERROR("[VideoRecord] 录像编码未知异常：event_id={}", task.eventId);
            }
            SPDLOG_INFO("[VideoRecord] 任务完成：event_id={}, success={}, frames={}, cost_ms={}", task.eventId, success, task.frames.size(), std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count());
            reportCompletion(task, success);
        }
        SPDLOG_INFO("[VideoRecord] 编码线程退出");
    }

    void FfmpegEventRecorder::stopAndDrain() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!closing_)
            {
                closing_ = true;
                for (auto& pending : pending_) ready_.push_back(std::move(pending.task));
                pending_.clear();
                history_.clear();
            }
        }
        available_.notify_all();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id()) std::terminate();
            worker_.join();
            SPDLOG_INFO("[VideoRecord] 视频录制任务已排空，编码线程已回收");
        }
    }
}
