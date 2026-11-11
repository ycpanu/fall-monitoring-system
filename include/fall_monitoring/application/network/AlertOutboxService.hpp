#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"
#include "fall_monitoring/application/ports/IMqttClient.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fall_monitoring::application::network
{
    struct DeviceUsage
    {
        int cpu = 0;
        int memory = 0;
        int storage = 0;
    };

    struct AlertOutboxOptions
    {
        std::string deviceId;
        std::string deploymentArea;
        std::string alertTopic = "fall_detection/alerts";
        int retrySeconds = 60;
        int reconnectSeconds = 10;
        int heartbeatSeconds = 10;
        std::size_t batchSize = 100;
        std::size_t maxInFlight = 256;
    };

    /**
     * @brief 本地 Outbox 与 MQTT 业务 ACK 协调器。
     * SQLite 是事实来源；只在收到 status=SAVED 且数据库内容与发送快照一致时标记已上传。
     * 网络/数据库访问只发生在本服务工作线程，不阻塞 ALSA/视觉/Paho 回调线程。
     */
    class AlertOutboxService final
    {
    public:
        using MetricsProvider = std::function<std::optional<DeviceUsage>()>;
        using CommandHandler = std::function<void(const std::string&, const std::string&)>;

        AlertOutboxService(AlertOutboxOptions options, ports::IAlertRepository& repository, ports::IMqttClient& mqtt, MetricsProvider metricsProvider = {}, CommandHandler onCommand = {});
        ~AlertOutboxService();

        AlertOutboxService(const AlertOutboxService&) = delete;
        AlertOutboxService& operator=(const AlertOutboxService&) = delete;

        [[nodiscard]] bool start();
        void stop() noexcept;
        /** SQLite 成功落盘后通知补传线程；不能在落盘前调用。 */
        void notifyPersisted() noexcept;
        [[nodiscard]] bool isConnected() const noexcept;

    private:
        using Clock = std::chrono::steady_clock;
        struct SentSnapshot
        {
            domain::event::AlertEvent event;
            Clock::time_point lastAttempt{};
        };

        void run() noexcept;
        void enqueueMessage(const std::string& topic, const std::string& payload);
        void onConnection(bool connected);
        void processMessages();
        void processAck(const std::string& payload);
        void sendPending(bool resendNow);
        void sendHeartbeat();

        AlertOutboxOptions options_;
        ports::IAlertRepository& repository_;
        ports::IMqttClient& mqtt_;
        MetricsProvider metricsProvider_;
        CommandHandler commandHandler_;
        const std::string ackTopic_;
        const std::string commandTopic_;
        const std::string statusTopic_;

        std::atomic<bool> running_{false};
        std::atomic<bool> online_{false};
        std::atomic<bool> forceResend_{false};
        std::thread worker_;
        std::mutex inboxMutex_;
        std::condition_variable wakeCv_;
        bool wake_ = false;
        std::deque<std::pair<std::string, std::string>> inbox_;
        std::unordered_map<std::string, SentSnapshot> sent_; // 仅 worker_ 访问。
        std::size_t scanOffset_ = 0;
    };
}
