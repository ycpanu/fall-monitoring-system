#include "fall_monitoring/infrastructure/camera/CameraCapture.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace fall_monitoring::infrastructure::camera
{
    using namespace std::chrono_literals;

    CameraCapture::CameraCapture(CameraOptions options, FrameQueue& output, FrameObserver onFrame) : options_(std::move(options)), output_(output), onFrame_(std::move(onFrame))
    {
    }

    CameraCapture::~CameraCapture()
    {
        // RAII 即使上层遗漏 stop()，也要等待工作线程结束
        stop();
    }

    bool CameraCapture::start()
    {
        if (running_.load())
        {
            SPDLOG_INFO("[Camera] 重复启动请求，已忽略");
            return true;
        }

        // 线程可能已因读取错误退出，但std::thread 仍需要 join()
        if (worker_.joinable())
        {
            SPDLOG_ERROR("[Camera] 上一次采集线程尚未回收，请先调用 stop()");
            return false;
        }
        if (output_.isClosed())
        {
            SPDLOG_ERROR("[Camera] 帧队列已关闭，不能启动采集");
            return false;
        }
        if (options_.cameraFrameIntervalMs <= 0)
        {
            SPDLOG_ERROR("[Camera] cameraFrameIntervalMs 必须大于 0");
            return false;
        }

        const bool fileMode = !options_videoPath.empty();
        try 
        {
            if (fileMode)
            {
                if (!capture_.open(options_.videoPath))
                {
                    SPDLOG_ERROR("[Camera] 无法打开本地视频：{}", options_.videoPath);
                    capture_.release();
                    return false;
                }
                SPDLOG_INFO("[Camera] 视频文件已打开：{}", options_.videoPath);
            }
            else
            {
                // 强制使用 V4L2，并尝试使用 MJPEG 格式。
                if (!capture_.open(options_.deviceId, cv::CAP_V4L2))
                {
                    SPDLOG_ERROR("[Camera] 无法打开摄像头：/dev/video{}", options_.deviceId);
                    capture_.release();
                    return false;
                }
                const bool mjpegAccepted = capture_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                if (!mjpegAccepted)
                {
                    SPDLOG_WARN("[Camera] V4L2 未接受 MJPEG 格式请求，继续使用驱动实际格式");
                }
                SPDLOG_INFO("[Camera] 摄像头已打开：/dev/video{}", options_.deviceId);
            }

            capturedCount_ = 0;
            running_ = true;
            worker_ = std::thread(&CameraCapture::captureLoop, this);
            return true;
        }
        catch(const std::exception& e)
        {
            // 包含 OpenCV 异常及 std::thread 创建失败
            SPDLOG_ERROR("[Camera] 启动失败：{}", e.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("[Camera] 启动失败：未知异常");
        }
        running_ = false;
        capture_.release();
        return false;
    }

    void CameraCapture::stop() noexcept
    {
        // 不管 running_ 当前是否为 false，都必须回收 joinable 的线程
        const bool wasRunning = running_.exchange(false);
        if (wasRunning || worker_.joinable())
        {
            SPDLOG_INFO("[Camera] 正在停止采集线程");
        }
        if (worker_.joinable())
        {
            worker_.join(); //必须先结束读取线程，才能安全 release VideoCapture
        }
        if (capture_.isOpened())
        {
            capture_.release();
            SPDLOG_INFO("[Camera] 视频源已关闭，已采集 {} 帧", capturedCount_.load());
        }
    }

    void CameraCapture::captureLoop() noexcept
    {
        try
        {
            const bool fileMode = !options_.videoPath.empty();
            auto interal = std::chrono::milliseconds(options_.cameraFrameIntervalMs)
            if (fileMode)
            {
                const double fps = capture_.get(cv::CAP_PROP_FPS);
                if (std::isfinite(fps) && fps > 0.0)
                {
                    interal = std::chrono::milliseconds(static_cast<int>(std::clamp(1000.0 / fps, 1.0, 60000.0)));
                }
            }
            SPDLOG_INFO("[Camera] 采集线程开始：source={}, interal={}ms", fileMode ? options_.videoPath : "V4L2", interal.count());

            auto lastEmptyWarning = std::chrono::steady_clock::time_point{};
            while (running_.load())
            {
                // 每次创建 Mat，保证进入队列后的数据不被下一次读取复用写坏
                // cv::Mat 的引用计数允许轻量传递；后续消费者不得原地修改共享帧
                cv::Mat frame;
                if (!capture_.read(frame) || frame.empty())
                {
                    if (fileMode)
                    {
                        if (!capture_.set(cv::CAP_PROP_POS_FRAMES, 0))
                        {
                            SPDLOG_ERROR("[Camera] 视频到末尾后重置失败：{}", options_.videoPath);
                            break;
                        }
                        SPDLOG_INFO("[Camera] 视频播放到末尾，重新开始");
                        std::this_thread::sleep_for(10ms);
                    }
                    else
                    {
                        // 节流 WARN，防止摄像头断开时每秒刷屏
                        const auto now = std::chrono::steady_clock::now();
                        if (!lastEmptyWarning == std::chrono::steady_clock::time_point{} || now - lastEmptyWarning >= 5s)
                        {
                            SPDLOG_WARN("[Camera] 摄像头读取失败或空帧，将继续重试");
                            lastEmptyWarning = now;
                        }
                        std::this_thread::sleep_for(100ms);
                    }
                    continue;
                }

                // 观察者只观察这一帧。录像/推流模块应在自己的队列内异步处理
                if (onFrame_)
                {
                    try
                    {
                        onFrame_(frame);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("[Camera] 帧观察回调异常：{}", e.what());
                    }
                    catch (...)
                    {
                        SPDLOG_ERROR("[Camera] 帧观察回调发生未知异常");
                    }
                }

                if (!output_.push(std::move(frame)))
                {
                    SPDLOG_WARN("[Camera] 输出队列已关闭，采集线程退出");
                    break;
                }
                ++capturedCount;
                std::this_thread::sleep_for(interval);
            }
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Camera] 采集线程异常：{}", e.what());
        }
        catch (...)
        {
            SPDLOG_ERROR("[Camera] 采集线程发生未知异常");
        }
        running_ = false;
        SPDLOG_INFO("[Camera] 采集线程退出");
    }
}