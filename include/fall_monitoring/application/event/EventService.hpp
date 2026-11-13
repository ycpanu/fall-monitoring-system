#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"
#include "fall_monitoring/concurrency/ReliableEventQueue.hpp"
#include "fall_monitoring/domain/event/AlertEvent.hpp"
#include "fall_monitoring/domain/event/EventCorrelator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
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
     * 有界内存事件队列 + SQLite 磁盘溢出缓存。
     * 超过队列容量的事件写入持久化缓存；消费者按顺序准备、落盘并删除缓存记录。
     * 单一消费线程独占 EventCorrelator，避免跨线程更改融合状态。
     */
    class EventService final
    {
    public:
        using EventHandler = std::function<bool(const domain::event::AlertEvent&)>;
        using EmergencyAlarmHandler = std::function<bool()>;

        EventService(EventServiceOptions options, EventHandler onPreparedEvent, ports::IAlertRepository& repository, EmergencyAlarmHandler emergencyAlarm = {});
        ~EventService();

        EventService(const EventService&) = delete;
        EventService& operator=(const EventService&) = delete;

        [[nodiscard]] bool start();
        // 返回 false 表示未接受：队列已关闭，或溢出缓存也未能落盘。
        // 调用方应触发应急本地声响并记录严重故障。
        [[nodiscard]] bool submit(domain::event::AlertEvent event);
        [[nodiscard]] std::size_t pendingCount() const { return queue_.size(); }
        void stop() noexcept;

    private:
        void consumeLoop() noexcept;
        [[nodiscard]] bool handlePrepared(domain::event::AlertEvent raw, std::uint64_t& persisted, std::uint64_t& fused) noexcept;

        EventServiceOptions options_;
        concurrency::ReliableEventQueue<domain::event::AlertEvent> queue_;
        domain::event::EventCorrelator correlator_;
        EventHandler handler_;
        ports::IAlertRepository& repository_;
        EmergencyAlarmHandler emergencyAlarm_;
        std::thread worker_;
        std::atomic<bool> accepting_{false};
        std::atomic<bool> startedOnce_{false};
        std::atomic<bool> stopping_{false};
        std::mutex retryMutex_;
        std::condition_variable retryCv_;
    };
}
