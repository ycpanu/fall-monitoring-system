#pragma once

#include "fall_monitoring/application/ports/IMqttClient.hpp"

#include <memory>
#include <string>

namespace fall_monitoring::infrastructure::network
{
    struct MqttOptions
    {
        std::string broker;
        std::string clientId;
        int keepAliveSeconds = 20;
        int connectTimeoutSeconds = 5;
        int publishTimeoutSeconds = 5;
    };

    /**
     * @brief Paho C++ 异步客户端适配器。
     * PImpl 隔离 mqtt/async_client.h，自动重连后重新订阅已登记主题。
     */
    class PahoMqttClient final : public application::ports::IMqttClient
    {
    public:
        explicit PahoMqttClient(MqttOptions options);
        ~PahoMqttClient() override;
        PahoMqttClient(const PahoMqttClient&) = delete;
        PahoMqttClient& operator=(const PahoMqttClient&) = delete;

        void setMessageHandler(MessageHandler handler) override;
        void setConnectionHandler(ConnectionHandler handler) override;
        [[nodiscard]] bool subscribe(const std::string& topic, int qos) override;
        [[nodiscard]] bool connect() override;
        void disconnect() noexcept override;
        [[nodiscard]] bool isConnected() const noexcept override;
        [[nodiscard]] bool publish(const std::string& topic, const std::string& payload, int qos) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
