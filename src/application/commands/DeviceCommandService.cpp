#include "fall_monitoring/application/commands/DeviceCommandService.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fall_monitoring::application::commands
{
    namespace
    {
        using Json = nlohmann::json;
        using domain::vision::NormalizedPoint;
        using domain::vision::SafeLieZone;

        [[nodiscard]] std::int64_t epochMilliseconds()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        // 只接受旧后端的 normalized polygon 格式：{"name":..., "points":[[x,y],...]}
        [[nodiscard]] bool parseZones(const Json& message, const DeviceCommandOptions& options, std::vector<SafeLieZone>& zones, std::string& error)
        {
            const auto it = message.find("safe_lie_zones");
            if (it == message.end() || !it->is_array())
            {
                error = "缺少 safe_lie_zones 数组";
                return false;
            }
            if (it->size() > options.maxZones)
            {
                error = "区域数量超过上限";
                return false;
            }
            zones.clear();
            zones.reserve(it->size());
            for (const auto& item : *it)
            {
                if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("points") || !item["points"].is_array())
                {
                    error = "区域必须包含字符串 name 和数组 points";
                    return false;
                }
                SafeLieZone zone;
                zone.name = item["name"].get<std::string>();
                if (zone.name.empty() || zone.name.size() > 128)
                {
                    error = "安全区域名称为空或过长";
                    return false;
                }
                const auto& points = item["points"];
                if (points.size() < 3 || points.size() > options.maxPointsPerZone)
                {
                    error = "安全区域顶点数量不合法";
                    return false;
                }
                zone.points.reserve(points.size());
                for (const auto& point : points)
                {
                    if (!point.is_array() || point.size() != 2 ||
                        !point[0].is_number() || !point[1].is_number())
                    {
                        error = "顶点必须为 [x, y] 数值数组";
                        return false;
                    }
                    const double x = point[0].get<double>();
                    const double y = point[1].get<double>();
                    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
                    {
                        error = "归一化坐标必须位于 [0,1]";
                        return false;
                    }
                    zone.points.push_back(NormalizedPoint{
                        static_cast<float>(x), static_cast<float>(y)
                    });
                }
                zones.push_back(std::move(zone));
            }
            return true;
        }
    }

    DeviceCommandService::DeviceCommandService(
        DeviceCommandOptions options, ports::ISafeZoneConfigurator& safeZones,
        ports::ILiveStream& liveStream, ports::IMqttClient& mqtt)
        : options_(std::move(options)), safeZones_(safeZones),
          liveStream_(liveStream), mqtt_(mqtt),
          commandTopic_("fall_detection/commands/" + options_.deviceId),
          resultTopic_("fall_detection/command_results/" + options_.deviceId)
    {
        if (options_.deviceId.empty() || options_.sceneConfigPath.empty() || options_.maxPendingCommands == 0 || options_.maxPayloadBytes == 0 || options_.maxZones == 0 || options_.maxPointsPerZone < 3)
        {
            throw std::invalid_argument("DeviceCommandService 配置不合法");
        }
    }

    DeviceCommandService::~DeviceCommandService() { stop(); }

    bool DeviceCommandService::start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) return true;
        if (worker_.joinable() || stopping_) return false;
        running_.store(true);
        try
        {
            worker_ = std::thread(&DeviceCommandService::run, this);
            SPDLOG_INFO("[Commands] 指令处理服务启动：topic={}", commandTopic_);
            return true;
        }
        catch (const std::exception& e)
        {
            running_.store(false);
            SPDLOG_ERROR("[Commands] 创建指令线程失败：{}", e.what());
            return false;
        }
    }

    bool DeviceCommandService::enqueue(const std::string& topic, const std::string& payload)
    {
        if (topic != commandTopic_ || payload.empty() || payload.size() > options_.maxPayloadBytes)
        {
            SPDLOG_WARN("[Commands] 拒绝指令：主题不匹配或消息大小非法 (bytes={})", payload.size());
            return false;
        }
        try
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_.load() || stopping_ || pending_.size() >= options_.maxPendingCommands)
                {
                    SPDLOG_ERROR("[Commands] 指令服务不可用或队列已满：queued={}", pending_.size());
                    return false;
                }
                pending_.push_back(payload);
            }
            cv_.notify_one();
            return true;
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Commands] 指令入队异常：{}", e.what());
            return false;
        }
    }

    void DeviceCommandService::publishResult(const std::string& command,
        const std::string& requestId, const std::string& status, const std::string& detail)
    {
        Json reply = {
            {"device_id", options_.deviceId},
            {"cmd", command},
            {"status", status},
            {"message", detail},
            {"timestamp", epochMilliseconds()}
        };
        if (!requestId.empty()) reply["request_id"] = requestId;
        // QoS1 PUBACK 仅代表 Broker 接受；云端需显式订阅 command_results 才能展示结果。
        if (!mqtt_.publish(resultTopic_, reply.dump(), 1))
        {
            SPDLOG_WARN("[Commands] 执行结果未送达 Broker：cmd={}, status={}", command, status);
        }
    }

    void DeviceCommandService::execute(const std::string& payload)
    {
        const Json msg = Json::parse(payload, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
        {
            SPDLOG_WARN("[Commands] 丢弃非法 JSON 指令，bytes={}", payload.size());
            publishResult("unknown", "", "REJECTED", "JSON 格式错误");
            return;
        }
        std::string requestId;
        if (msg.contains("request_id"))
        {
            if (!msg["request_id"].is_string())
            {
                publishResult("unknown", "", "REJECTED", "request_id 必须是字符串");
                return;
            }
            requestId = msg["request_id"].get<std::string>();
        }
        if (!msg.contains("cmd") || !msg["cmd"].is_string())
        {
            SPDLOG_WARN("[Commands] 指令缺少合法 cmd");
            publishResult("unknown", requestId, "REJECTED", "缺少合法 cmd");
            return;
        }
        const std::string cmd = msg["cmd"].get<std::string>();
        if (cmd == "start_live")
        {
            if (!msg.contains("rtmp_url") || !msg["rtmp_url"].is_string())
            {
                publishResult(cmd, requestId, "REJECTED", "缺少 rtmp_url");
                return;
            }
            const std::string url = msg["rtmp_url"].get<std::string>();
            if (!liveStream_.start(url))
            {
                SPDLOG_ERROR("[Commands] 实时视频启动失败");
                publishResult(cmd, requestId, "FAILED", "FFmpeg 启动失败或 RTMP URL 无效");
                return;
            }
            SPDLOG_INFO("[Commands] 已接受实时视频推流指令");
            publishResult(cmd, requestId, "ACCEPTED", "编码进程已创建，尚不代表 RTMP 服务端收流成功");
        }
        else if (cmd == "stop_live")
        {
            liveStream_.stop();
            SPDLOG_INFO("[Commands] 实时视频已停止");
            publishResult(cmd, requestId, "SUCCESS", "实时视频已停止");
        }
        else if (cmd == "update_safe_zones")
        {
            std::vector<SafeLieZone> zones;
            std::string error;
            if (!parseZones(msg, options_, zones, error))
            {
                SPDLOG_WARN("[Commands] 拒绝安全区域配置：{}，request_id={}", error, requestId);
                publishResult(cmd, requestId, "REJECTED", error);
                return;
            }
            if (!safeZones_.replaceZonesAndSave(zones, options_.sceneConfigPath))
            {
                SPDLOG_ERROR("[Commands] 安全区域配置写盘或更新失败：request_id={}", requestId);
                publishResult(cmd, requestId, "FAILED", "安全区域校验或持久化失败");
                return;
            }
            SPDLOG_INFO("[Commands] 安全区域已热更新：request_id={}, count={}", requestId, safeZones_.getZoneCount());
            publishResult(cmd, requestId, "SUCCESS", "安全区域已保存并生效");
        }
        else
        {
            SPDLOG_WARN("[Commands] 未知控制指令：{}", cmd);
            publishResult(cmd, requestId, "REJECTED", "未知控制指令");
        }
    }

    void DeviceCommandService::run() noexcept
    {
        while (true)
        {
            std::string payload;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {return stopping_ || !pending_.empty();});
                if (stopping_) break;
                payload = std::move(pending_.front());
                pending_.pop_front();
            }
            try { execute(payload); }
            catch (const std::exception& e)
            {
                SPDLOG_ERROR("[Commands] 指令执行异常：{}", e.what());
            }
            catch (...) { SPDLOG_ERROR("[Commands] 指令执行发生未知异常"); }
        }
        running_.store(false);
        SPDLOG_INFO("[Commands] 指令处理线程退出");
    }

    void DeviceCommandService::stop() noexcept
    {
        std::size_t discarded = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            running_.store(false);
            discarded = pending_.size();
            pending_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id()) std::terminate();
            worker_.join();
        }
        if (discarded != 0)
        {
            SPDLOG_WARN("[Commands] 关闭时舍弃尚未执行的 {} 条指令", discarded);
        }
    }
}
