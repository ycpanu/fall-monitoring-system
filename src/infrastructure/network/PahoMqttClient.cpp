#include "fall_monitoring/infrastructure/network/PahoMqttClient.hpp"

#include <mqtt/async_client.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fall_monitoring::infrastructure::network
{
    struct PahoMqttClient::Impl final : public virtual mqtt::callback
    {
        explicit Impl(MqttOptions options) : options_(std::move(options))
        {
            if (options_.broker.empty() || options_.clientId.empty() || options_.keepAliveSeconds <= 0 || options_.connectTimeoutSeconds <= 0 || options_.publishTimeoutSeconds <= 0)
            {
                throw std::invalid_argument("MQTT 配置错误：broker/client_id 不能为空且超时参数必须大于 0");
            }
            client_ = std::make_unique<mqtt::async_client>(options_.broker, options_.clientId);
            client_->set_callback(*this);
        }

        ~Impl() override { disconnect(); }

        void setMessageHandler(MessageHandler handler)
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            onMessage_ = std::move(handler);
        }

        void setConnectionHandler(ConnectionHandler handler)
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            onConnection_ = std::move(handler);
        }

        bool isConnected() const noexcept
        {
            return client_ != nullptr && client_->is_connected();
        }

        bool connect()
        {
            std::lock_guard<std::mutex> lock(connectMutex_);
            if (isConnected()) return true;
            try
            {
                mqtt::connect_options options;
                options.set_clean_session(true);
                options.set_keep_alive_interval(options_.keepAliveSeconds);
                options.set_connect_timeout(options_.connectTimeoutSeconds);
                options.set_automatic_reconnect(1, 10);
                auto token = client_->connect(options);
                if (!token->wait_for(std::chrono::seconds(options_.connectTimeoutSeconds)))
                {
                    SPDLOG_WARN("[MQTT] 连接 Broker 超时：{}", options_.broker);
                    return false;
                }
                // 已连接时 connected() 回调也可能触发 notifyConnection()，内部去重。
                notifyConnection(true);
                SPDLOG_INFO("[MQTT] Broker 已连接：{}", options_.broker);
                return true;
            }
            catch (const mqtt::exception& e)
            {
                SPDLOG_WARN("[MQTT] Broker 连接失败，稍后重试：{}", e.what());
                return false;
            }
        }

        void disconnect() noexcept
        {
            std::lock_guard<std::mutex> lock(connectMutex_);
            try
            {
                if (isConnected())
                {
                    auto token = client_->disconnect();
                    if (!token->wait_for(std::chrono::seconds(options_.connectTimeoutSeconds)))
                        SPDLOG_WARN("[MQTT] 断开 Broker 等待超时");
                }
            }
            catch (const mqtt::exception& e)
            {
                SPDLOG_WARN("[MQTT] 断开 Broker 异常：{}", e.what());
            }
            notifyConnection(false);
        }

        bool subscribe(const std::string& topic, int qos)
        {
            if (topic.empty() || qos < 0 || qos > 2) return false;
            {
                std::lock_guard<std::mutex> lock(subscriptionsMutex_);
                bool found = false;
                for (auto& sub : subscriptions_)
                {
                    if (sub.first == topic) { sub.second = qos; found = true; break; }
                }
                if (!found) subscriptions_.emplace_back(topic, qos);
            }
            if (isConnected()) return subscribeOnline(topic, qos);
            return true;  // 离线订阅已登记，connected() 时自动恢复。
        }

        bool subscribeOnline(const std::string& topic, int qos)
        {
            try
            {
                // 此方法可由 Paho connected() 回调调用，禁止等待 token，否则死锁。
                (void)client_->subscribe(topic, qos);
                SPDLOG_INFO("[MQTT] 已提交订阅：topic={}，qos={}", topic, qos);
                return true;
            }
            catch (const mqtt::exception& e)
            {
                SPDLOG_ERROR("[MQTT] 订阅失败：topic={}，error={}", topic, e.what());
                return false;
            }
        }

        bool publish(const std::string& topic, const std::string& payload, int qos)
        {
            if (topic.empty() || qos < 0 || qos > 1 || !isConnected()) return false;
            try
            {
                auto message = mqtt::make_message(topic, payload);
                message->set_qos(qos);
                message->set_retained(false);
                const auto token = client_->publish(message);
                if (qos == 1 && !token->wait_for(std::chrono::seconds(options_.publishTimeoutSeconds)))
                {
                    // 消息仍可能到达 Broker；不能因超时而修改 SQLite 状态。
                    SPDLOG_WARN("[MQTT] QoS1 PUBACK 等待超时，topic={}", topic);
                    return false;
                }
                return true;
            }
            catch (const mqtt::exception& e)
            {
                SPDLOG_WARN("[MQTT] 发布失败：topic={}，error={}", topic, e.what());
                return false;
            }
        }

        void connected(const std::string& reason) override
        {
            SPDLOG_INFO("[MQTT] 已连接/重连，恢复订阅：{}", reason);
            std::vector<std::pair<std::string, int>> copy;
            {
                std::lock_guard<std::mutex> lock(subscriptionsMutex_);
                copy = subscriptions_;
            }
            for (const auto& sub : copy) (void)subscribeOnline(sub.first, sub.second);
            notifyConnection(true);
        }

        void connection_lost(const std::string& cause) override
        {
            SPDLOG_WARN("[MQTT] 网络连接断开：{}", cause);
            notifyConnection(false);
        }

        void message_arrived(mqtt::const_message_ptr msg) override
        {
            MessageHandler handler;
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                handler = onMessage_;
            }
            if (!handler) return;
            try { handler(msg->get_topic(), msg->to_string()); }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("[MQTT] 消息回调异常：{}", e.what());
            }
            catch (...) { SPDLOG_ERROR("[MQTT] 消息回调发生未知异常"); }
        }

        void notifyConnection(bool connectedNow)
        {
            ConnectionHandler handler;
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (notifiedConnected_ == connectedNow) return;
                notifiedConnected_ = connectedNow;
                handler = onConnection_;
            }
            if (!handler) return;
            try { handler(connectedNow); }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("[MQTT] 连接状态回调异常：{}", e.what());
            }
            catch (...) { SPDLOG_ERROR("[MQTT] 连接状态回调发生未知异常"); }
        }

        MqttOptions options_;
        std::unique_ptr<mqtt::async_client> client_;
        mutable std::mutex connectMutex_;
        mutable std::mutex callbackMutex_;
        mutable std::mutex subscriptionsMutex_;
        MessageHandler onMessage_;
        ConnectionHandler onConnection_;
        bool notifiedConnected_ = false;
        std::vector<std::pair<std::string, int>> subscriptions_;
    };

    PahoMqttClient::PahoMqttClient(MqttOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
    PahoMqttClient::~PahoMqttClient() = default;
    void PahoMqttClient::setMessageHandler(MessageHandler h) { impl_->setMessageHandler(std::move(h)); }
    void PahoMqttClient::setConnectionHandler(ConnectionHandler h) { impl_->setConnectionHandler(std::move(h)); }
    bool PahoMqttClient::subscribe(const std::string& t, int q) { return impl_->subscribe(t, q); }
    bool PahoMqttClient::connect() { return impl_->connect(); }
    void PahoMqttClient::disconnect() noexcept { impl_->disconnect(); }
    bool PahoMqttClient::isConnected() const noexcept { return impl_->isConnected(); }
    bool PahoMqttClient::publish(const std::string& t, const std::string& p, int q) { return impl_->publish(t,p,q); }
}
