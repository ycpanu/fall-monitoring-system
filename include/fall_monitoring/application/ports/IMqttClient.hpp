#pragma once

#include <functional>
#include <string>

namespace fall_monitoring::application::ports
{
    /**
     * @brief MQTT 通信端口。Paho 的网络回调不得执行耗时的业务、磁盘操作。
     * publish() 的 true 仅代表指定 MQTT QoS 级别的发送完成，不代表云端保存。
     */
    class IMqttClient
    {
    public:
        using MessageHandler = std::function<void(const std::string&, const std::string&)>;
        using ConnectionHandler = std::function<void(bool)>;

        virtual ~IMqttClient() = default;
        virtual void setMessageHandler(MessageHandler handler) = 0;
        virtual void setConnectionHandler(ConnectionHandler handler) = 0;
        [[nodiscard]] virtual bool subscribe(const std::string& topic, int qos) = 0;
        [[nodiscard]] virtual bool connect() = 0;
        virtual void disconnect() noexcept = 0;
        [[nodiscard]] virtual bool isConnected() const noexcept = 0;
        [[nodiscard]] virtual bool publish(const std::string& topic, const std::string& payload, int qos) = 0;
    };
}