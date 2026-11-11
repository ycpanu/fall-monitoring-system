#pragma once

#include "fall_monitoring/application/ports/ILiveStream.hpp"
#include "fall_monitoring/application/ports/IMqttClient.hpp"
#include "fall_monitoring/application/ports/ISafeZoneConfigurator.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace fall_monitoring::application::commands
{
    struct DeviceCommandOptions
    {
        std::string deviceId;
        std::string sceneConfigPath = "configs/scene_config.json";
        std::size_t maxPendingCommands = 128;
        std::size_t maxPayloadBytes = 65536;
        std::size_t maxZones = 64;
        std::size_t maxPointsPerZone = 128;
    };

    /**
     * @brief MQTT 下行命令编排服务。
     *
     * Outbox 工作线程仅执行 enqueue()，不在其线程内启动 FFmpeg 或执行文件写入。
     * 单独的指令工作线程串行执行，避免 start/stop 和多次配置更新交错。
     * 指令结果发布主题与旧后端兼容性详见 docs/device-commands.md。
     */
    class DeviceCommandService final
    {
    public:
        DeviceCommandService(DeviceCommandOptions options, ports::ISafeZoneConfigurator& safeZones, ports::ILiveStream& liveStream, ports::IMqttClient& mqtt);
        ~DeviceCommandService();
        DeviceCommandService(const DeviceCommandService&) = delete;
        DeviceCommandService& operator=(const DeviceCommandService&) = delete;

        [[nodiscard]] bool start();
        /** @return false 表示服务未启动、关闭或消息队列已满。不会静默丢弃。 */
        [[nodiscard]] bool enqueue(const std::string& topic, const std::string& payload);
        /** 关闭时拒收新指令，并丢弃尚未执行的命令；已开始执行的命令等待结束。 */
        void stop() noexcept;

    private:
        void run() noexcept;
        void execute(const std::string& payload);
        void publishResult(const std::string& command, const std::string& requestId, const std::string& status, const std::string& detail);

        DeviceCommandOptions options_;
        ports::ISafeZoneConfigurator& safeZones_;
        ports::ILiveStream& liveStream_;
        ports::IMqttClient& mqtt_;
        const std::string commandTopic_;
        const std::string resultTopic_;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::string> pending_;
        std::thread worker_;
        bool stopping_ = false;
        std::atomic<bool> running_{false};
    };
}
