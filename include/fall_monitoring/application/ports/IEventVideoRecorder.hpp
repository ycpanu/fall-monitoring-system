#pragma once

#include <opencv2/core/mat.hpp>
#include <string>

namespace fall_monitoring::application::ports
{
    /**
     * @brief 事件录像端口。采集线程投递帧，EventService 消费线程提交录像任务。
     * 
     * 接口不暴露 FFmpeg 进程与编解码细节。
     */
    class IEventVideoRecorder
    {
    public:
        virtual ~IEventVideoRecorder() = default;

        /**
         * @brief 采集线程调用；不进行磁盘或网络 I/O。
         */
        virtual void observeFrame(const cv::Mat& frame) noexcept = 0;

        /**
         * @brief 请求前后片段录制。
         * @return true 表示任务已接收或此 event_id 已在处理；不代表文件已生成。
         */
        [[nodiscard]] virtual bool requestRecording(const std::string& eventId, const std::string& outputPath) = 0;

        /**
         * @brief 停止采集后调用，补齐未完成片段并排空编码任务。
         */
        virtual void stopAndDrain() noexcept = 0;
    }
}