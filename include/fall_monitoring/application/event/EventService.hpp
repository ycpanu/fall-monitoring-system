#pragma once

#include "fall_monitoring/concurrency/ReliableEventQueue.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"
#include "fall_monitoring/domain/event/EventCorrelator.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace fall_monitoring::application::event
{
    struct EventServiceOptions
    {
        std::string deviceId;
        std::string deploymentArea;
        std::string videoOutputDir = "videos";
        std::int64_t fusionWindowMs = 10000;
        std::size_t queueCapacity = 10;
        std::chrono::milliseconds submitTimeout{50};
    };

    /**
     * @brief 事件的单一入口和顺序分发服务
     * 
     * 视觉推理线程和 ALSA 采集线程只调用 submit()，不直接融合事件
     */
    class EventService final 
    {
    public:
        using EventHandler = std::function<void(domain::event::AlertEvent)>;

        EventService(EventServiceOptions options, EventHandler onPreparedEvent);
        ~EventService();

        EventService(const EventService&) = delete;
        EventService& operator=(const EventService&) = delete;

        [[nodiscard]] bool start();

        /**
         * @brief 提交来自视觉或语音的原始事件
         * @return 队列已关闭、尚未启动或等待超时时返回 false。
         * @note 不静默覆盖旧事件；失败时调用方必须记录/处理。
         */
        [[nodiscard]] bool submit(domain::event::AlertEvent event);

        /**
         * @brief 停止接收新事件、排空内存队列并回收消费者线程
         * @note 调用方必须先停止所有事件生产者；不可在事件回调内调用 stop()。
         */
        void stop() noexcept;

    private:
        void consumeLoop() noexcept;

        EventServiceOptions options_;
        concurrency::ReliableEventQueue<domain::event::AlertEvent> queue_;
        domain::event::EventCorrelator correlator_;
        EventHandler handler_;
        std::thread worker_;
        std::atomic<bool> accepting_{false};
        std::atomic<bool> startedOnce_{false};
    };
}