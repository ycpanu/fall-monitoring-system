#include "fall_monitoring/infrastructure/video/FfmpegLiveStreamer.hpp"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fall_monitoring::infrastructure::video
{
    namespace
    {
        using namespace std::chrono_literals;

        struct EncoderProcess
        {
            int writer = -1;
            pid_t pid = -1;
        };

        [[nodiscard]] bool validRtmpUrl(const std::string& url)
        {
            if (url.size() < 9 || url.size() > 2048) return false;
            const bool rtmp = url.rfind("rtmp://", 0) == 0;
            const bool rtmps = url.rfind("rtmps://", 0) == 0;
            if (!rtmp && !rtmps) return false;
            const auto firstHost = rtmp ? 7U : 8U;
            if (url.size() <= firstHost || url[firstHost] == '/') return false;
            for (unsigned char ch : url)
            {
                if (ch <= 0x20 || ch == 0x7f) return false;
            }
            return true;
        }

        // 子进程从标准输入读取 BGR24 原始帧；父进程用 socketpair 发送，
        // MSG_NOSIGNAL 避免 FFmpeg 退出时 SIGPIPE 终止整个网关。
        [[nodiscard]] EncoderProcess launchEncoder(const LiveStreamOptions& options, const std::string& url)
        {
            int fds[2] = {-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            {
                SPDLOG_ERROR("[Live] 创建 FFmpeg 输入套接字失败：{}", std::strerror(errno));
                return {};
            }
            // 子进程仅继承接到 stdin 的 fd，其余 socket 均由 file actions 显式关闭。
            posix_spawn_file_actions_t actions;
            if (::posix_spawn_file_actions_init(&actions) != 0)
            {
                ::close(fds[0]); ::close(fds[1]);
                return {};
            }
            ::posix_spawn_file_actions_adddup2(&actions, fds[1], STDIN_FILENO);
            ::posix_spawn_file_actions_addclose(&actions, fds[0]);
            if (fds[1] != STDIN_FILENO)
            {
                ::posix_spawn_file_actions_addclose(&actions, fds[1]);
            }

            std::vector<std::string> args = {
                options.ffmpegBinary,
                "-hide_banner", "-loglevel", "error", "-nostdin",
                "-f", "rawvideo", "-pixel_format", "bgr24",
                "-video_size", std::to_string(options.width) + "x" + std::to_string(options.height),
                "-framerate", std::to_string(options.fps), "-i", "pipe:0",
                "-an", "-c:v", options.encoder,
                "-b:v", std::to_string(options.bitrateKbps) + "k",
                "-g", std::to_string(options.fps), "-f", "flv", url
            };
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args) argv.push_back(arg.data());
            argv.push_back(nullptr);

            pid_t pid = -1;
            const int result = ::posix_spawnp(&pid, options.ffmpegBinary.c_str(), &actions, nullptr, argv.data(), environ);
            ::posix_spawn_file_actions_destroy(&actions);
            ::close(fds[1]);
            if (result != 0)
            {
                ::close(fds[0]);
                SPDLOG_ERROR("[Live] FFmpeg 启动失败：{}", std::strerror(result));
                return {};
            }
            const int flags = ::fcntl(fds[0], F_GETFL, 0);
            if (flags < 0 || ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0)
            {
                SPDLOG_ERROR("[Live] 无法设置 FFmpeg 管道非阻塞模式");
                ::close(fds[0]);
                ::kill(pid, SIGKILL);
                (void)::waitpid(pid, nullptr, 0);
                return {};
            }
            return EncoderProcess{fds[0], pid};
        }

        [[nodiscard]] bool writeFrame(int writer, const unsigned char* data,
                                      std::size_t size, const std::atomic<bool>& running)
        {
            std::size_t sent = 0;
            while (sent < size && running.load())
            {
                pollfd pfd{};
                pfd.fd = writer;
                pfd.events = POLLOUT;
                const int ready = ::poll(&pfd, 1, 150);
                if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
                if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                {
                    return false;
                }
                const ssize_t n = ::send(writer, data + sent, size - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n > 0) {sent += static_cast<std::size_t>(n); continue;}
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                return false;
            }
            return sent == size;
        }

        // 先通过 EOF 请求 FFmpeg 优雅退出；网络异常时通过 TERM/KILL 有界回收。
        void reapEncoder(pid_t pid) noexcept
        {
            if (pid <= 0) return;
            auto waitUntil = [pid](std::chrono::milliseconds duration) {
                const auto end = std::chrono::steady_clock::now() + duration;
                while (std::chrono::steady_clock::now() < end)
                {
                    int status = 0;
                    const pid_t rc = ::waitpid(pid, &status, WNOHANG);
                    if (rc == pid || (rc < 0 && errno == ECHILD)) return true;
                    if (rc < 0 && errno != EINTR) return true;
                    std::this_thread::sleep_for(50ms);
                }
                return false;
            };
            if (waitUntil(600ms)) return;
            (void)::kill(pid, SIGTERM);
            if (waitUntil(600ms)) return;
            SPDLOG_WARN("[Live] FFmpeg 未及时退出，强制回收 PID={}", pid);
            (void)::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }

    FfmpegLiveStreamer::FfmpegLiveStreamer(LiveStreamOptions options) : options_(std::move(options))
    {
        if (options_.width <= 0 || options_.height <= 0 || options_.fps <= 0 || options_.bitrateKbps <= 0 || options_.queueCapacity == 0 || options_.ffmpegBinary.empty() || options_.encoder.empty())
        {
            throw std::invalid_argument("实时视频编码参数无效");
        }
    }

    FfmpegLiveStreamer::~FfmpegLiveStreamer() { stop(); }

    bool FfmpegLiveStreamer::start(const std::string& rtmpUrl)
    {
        if (!validRtmpUrl(rtmpUrl))
        {
            SPDLOG_WARN("[Live] 拒绝非法 RTMP/RTMPS 地址");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_.load() && activeUrl_ == rtmpUrl) return true;
        }
        // 新地址需要终止旧编码进程；工作线程退出时 running_ 可能已变为 false，
        // 但 std::thread 仍是 joinable，所以重启前一律进行 stop() 回收。
        stop();
        std::lock_guard<std::mutex> lock(mutex_);
        auto queue = std::make_shared<FrameQueue>(options_.queueCapacity);
        const EncoderProcess process = launchEncoder(options_, rtmpUrl);
        if (process.writer < 0) return false;

        queue_ = queue;
        activeUrl_ = rtmpUrl;
        running_.store(true);
        try
        {
            worker_ = std::thread(&FfmpegLiveStreamer::streamLoop, this, std::move(queue), process.writer, static_cast<int>(process.pid));
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Live] 创建编码工作线程失败：{}", e.what());
            running_.store(false);
            queue_->close();
            queue_.reset();
            activeUrl_.clear();
            ::close(process.writer);
            (void)::kill(process.pid, SIGKILL);
            (void)::waitpid(process.pid, nullptr, 0);
            return false;
        }
        SPDLOG_INFO("[Live] FFmpeg 编码进程已创建：PID={}, codec={}, {}x{}@{}fps", process.pid, options_.encoder, options_.width, options_.height, options_.fps);
        return true;
    }

    void FfmpegLiveStreamer::pushFrame(const cv::Mat& frame) noexcept
    {
        if (!running_.load() || frame.empty()) return;
        try
        {
            std::shared_ptr<FrameQueue> queue;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_.load()) queue = queue_;
            }
            if (queue) (void)queue->push(frame); // cv::Mat 共享不可变帧数据，避免在采集线程复制像素
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Live] 入队视频帧失败：{}", e.what());
        }
    }

    bool FfmpegLiveStreamer::isStreaming() const noexcept {return running_.load();}

    void FfmpegLiveStreamer::stop() noexcept
    {
        std::thread oldWorker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.store(false);
            if (queue_) queue_->close();
            queue_.reset();
            activeUrl_.clear();
            oldWorker = std::move(worker_);
        }
        if (oldWorker.joinable())
        {
            if (oldWorker.get_id() == std::this_thread::get_id()) std::terminate();
            oldWorker.join();
            SPDLOG_INFO("[Live] 编码线程和 FFmpeg 子进程已回收");
        }
    }

    void FfmpegLiveStreamer::streamLoop(std::shared_ptr<FrameQueue> queue, int writer, int childPid) noexcept
    {
        std::uint64_t frameCount = 0;
        try
        {
            while (running_.load())
            {
                cv::Mat frame;
                if (!queue->waitPopFor(frame, std::chrono::milliseconds(200)))
                {
                    if (queue->isClosed()) break;
                    continue;
                }
                if (frame.empty()) continue;
                cv::Mat rgbCompatible;
                if (frame.channels() == 3)
                {
                    rgbCompatible = frame;
                }
                else if (frame.channels() == 4)
                {
                    cv::cvtColor(frame, rgbCompatible, cv::COLOR_BGRA2BGR);
                }
                else if (frame.channels() == 1)
                {
                    cv::cvtColor(frame, rgbCompatible, cv::COLOR_GRAY2BGR);
                }
                else continue;
                cv::Mat output;
                if (rgbCompatible.cols != options_.width || rgbCompatible.rows != options_.height)
                {
                    cv::resize(rgbCompatible, output, cv::Size(options_.width, options_.height));
                }
                else
                {
                    output = rgbCompatible;
                }
                if (!output.isContinuous()) output = output.clone();
                if (!writeFrame(writer, output.ptr<unsigned char>(), output.total() * output.elemSize(), running_))
                {
                    if (running_.load()) SPDLOG_ERROR("[Live] FFmpeg 输入中断，停止本次推流");
                    break;
                }
                ++frameCount;
            }
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Live] 推流工作线程异常：{}", e.what());
        }
        catch (...) {SPDLOG_ERROR("[Live] 推流工作线程发生未知异常");}
        running_.store(false);
        queue->close();
        ::close(writer); // 对 FFmpeg 发送 EOF
        reapEncoder(static_cast<pid_t>(childPid));
        SPDLOG_INFO("[Live] 推流线程退出，累计发送 {} 帧", frameCount);
    }
}
