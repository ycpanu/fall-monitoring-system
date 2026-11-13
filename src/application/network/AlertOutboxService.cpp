#include "fall_monitoring/application/network/AlertOutboxService.hpp"
#include "fall_monitoring/infrastructure/network/AlertJsonCodec.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fall_monitoring::application::network
{
    AlertOutboxService::AlertOutboxService(AlertOutboxOptions options,
        ports::IAlertRepository& repository, ports::IMqttClient& mqtt,
        MetricsProvider metricsProvider, CommandHandler onCommand)
        : options_(std::move(options)), repository_(repository), mqtt_(mqtt),
          metricsProvider_(std::move(metricsProvider)), commandHandler_(std::move(onCommand)),
          ackTopic_("fall_detection/ack/" + options_.deviceId),
          commandTopic_("fall_detection/commands/" + options_.deviceId),
          statusTopic_("fall_detection/status/" + options_.deviceId)
    {
        if (options_.deviceId.empty() || options_.alertTopic.empty() ||
            options_.retrySeconds <= 0 || options_.reconnectSeconds <= 0 ||
            options_.heartbeatSeconds <= 0 || options_.batchSize == 0 ||
            options_.batchSize > 1000 || options_.maxInFlight == 0)
            throw std::invalid_argument("Outbox MQTT 配置错误");
    }

    AlertOutboxService::~AlertOutboxService() { stop(); }

    bool AlertOutboxService::start()
    {
        if (running_.load()) return true;
        // 线程异常退出后先回收其 std::thread 对象，再将服务标记为运行。
        if (worker_.joinable()) worker_.join();
        if (running_.exchange(true)) return true;
        mqtt_.setMessageHandler([this](const std::string& topic, const std::string& payload) {
            enqueueMessage(topic, payload);
        });
        mqtt_.setConnectionHandler([this](bool connected) { onConnection(connected); });
        // 离线时先登记主题；Paho 自动重连回调会按登记记录再次订阅。
        if (!mqtt_.subscribe(ackTopic_, 1) || !mqtt_.subscribe(commandTopic_, 1))
        {
            SPDLOG_ERROR("[Outbox] 订阅主题登记失败，拒绝开始补传");
            running_.store(false);
            mqtt_.setMessageHandler({});
            mqtt_.setConnectionHandler({});
            return false;
        }
        try { worker_ = std::thread(&AlertOutboxService::run, this); }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Outbox] 无法启动补传线程：{}", e.what());
            running_.store(false);
            mqtt_.setMessageHandler({});
            mqtt_.setConnectionHandler({});
            return false;
        }
        SPDLOG_INFO("[Outbox] 已启动，ACK={}，command={}，retry={}s", ackTopic_, commandTopic_, options_.retrySeconds);
        return true;
    }

    void AlertOutboxService::notifyPersisted() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            wake_ = true;
        }
        wakeCv_.notify_one();
    }

    void AlertOutboxService::enqueueMessage(const std::string& topic, const std::string& payload)
    {
        if (!running_.load()) return;
        if (topic != ackTopic_ && topic != commandTopic_) return;
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            // 对无上界网络输入施加内存上限；丢失 ACK 时 SQLite 仍为 PENDING，会重试。
            if (inbox_.size() >= 512)
            {
                SPDLOG_ERROR("[Outbox] MQTT 接收积压，已拒绝一条消息；报警保持 PENDING");
                return;
            }
            inbox_.emplace_back(topic, payload);
            wake_ = true;
        }
        wakeCv_.notify_one();
    }

    void AlertOutboxService::onConnection(bool connected)
    {
        online_.store(connected);
        if (connected) forceResend_.store(true);
        notifyPersisted();
    }

    bool AlertOutboxService::isConnected() const noexcept { return online_.load(); }

    void AlertOutboxService::processAck(const std::string& payload)
    {
        // 从 Paho 回调转移到工作线程后才做 JSON 解析、SQLite 操作。
        const auto json = nlohmann::json::parse(payload, nullptr, false);
        if (json.is_discarded() || !json.is_object() ||
            !json.contains("event_id") || !json["event_id"].is_string() ||
            !json.contains("status") || !json["status"].is_string())
        {
            SPDLOG_WARN("[Outbox] 忽略格式错误的业务 ACK");
            return;
        }
        const std::string id = json["event_id"].get<std::string>();
        const std::string status = json["status"].get<std::string>();
        if (id.empty() || status != "SAVED")
        {
            SPDLOG_WARN("[Outbox] 忽略无效业务 ACK：id={} status={}", id, status);
            return;
        }
        const auto found = sent_.find(id);
        if (found == sent_.end())
        {
            // 无已知发送快照（如进程重启后的迟到 ACK）不直接标记已上传。
            SPDLOG_DEBUG("[Outbox] 忽略未关联发送记录的 ACK：id={}", id);
            return;
        }
        // 旧版 ACK 只有 event_id/status，无法识别多个融合版本。
        // 对 revision > 1 必须收到服务端回显的 revision，避免延迟的旧 ACK 误确认更新。
        const auto& snapshot = found->second.event;
        if (json.contains("revision"))
        {
            const auto& ackRevision = json["revision"];
            if (!ackRevision.is_number_integer() ||
                (ackRevision.is_number_unsigned()
                    ? ackRevision.get<std::uint64_t>() != snapshot.revision
                    : (ackRevision.get<std::int64_t>() <= 0 ||
                       static_cast<std::uint64_t>(ackRevision.get<std::int64_t>()) != snapshot.revision)))
            {
                SPDLOG_WARN("[Outbox] 忽略版本不匹配的业务 ACK：event_id={}，当前发送 revision={}", id, snapshot.revision);
                return;
            }
        }
        else if (snapshot.revision > 1)
        {
            SPDLOG_WARN("[Outbox] 已融合报警需要版本 ACK：event_id={}，revision={}；请升级云端 ACK 回显", id, snapshot.revision);
            return;
        }
        const auto result = repository_.markAsUploadedIfMatches(snapshot);
        switch (result)
        {
            case ports::AckCommitResult::Uploaded:
                SPDLOG_INFO("[Outbox] 云端业务 ACK 已持久化：event_id={}", id);
                sent_.erase(found);
                break;
            case ports::AckCommitResult::Stale:
                // 旧 ACK 不可确认发生了融合更新的较新 SQLite 记录。
                SPDLOG_INFO("[Outbox] 收到旧版或重复 ACK：event_id={}，保留待上传状态", id);
                sent_.erase(found);
                notifyPersisted();
                break;
            case ports::AckCommitResult::Error:
                SPDLOG_ERROR("[Outbox] ACK 落库失败：event_id={}，保留 PENDING 等待重试", id);
                sent_.erase(found);
                notifyPersisted();
                break;
        }
    }

    void AlertOutboxService::processMessages()
    {
        std::deque<std::pair<std::string, std::string>> messages;
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            messages.swap(inbox_);
        }
        for (const auto& item : messages)
        {
            if (item.first == ackTopic_)
            {
                processAck(item.second);
            }
            else if (item.first == commandTopic_)
            {
                if (commandHandler_)
                {
                    try { commandHandler_(item.first, item.second); }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("[Outbox] 设备控制消息处理失败：{}", e.what());
                    }
                    catch (...) { SPDLOG_ERROR("[Outbox] 设备控制消息处理发生未知异常"); }
                }
                else
                {
                    SPDLOG_WARN("[Outbox] 收到设备指令，但尚未注册具体处理器：topic={}", item.first);
                }
            }
        }
    }

    void AlertOutboxService::sendPending(bool resendNow)
    {
        std::vector<domain::event::AlertEvent> pending;
        if (!repository_.loadPendingAlerts(pending, options_.batchSize, scanOffset_))
        {
            SPDLOG_ERROR("[Outbox] SQLite 查询待上传报警失败");
            return;
        }
        if (pending.empty() && scanOffset_ != 0)
        {
            scanOffset_ = 0;  // 翻页末尾，下个周期回到最早待上传记录。
            return;
        }
        if (pending.size() == options_.batchSize) scanOffset_ += pending.size();
        else scanOffset_ = 0;

        const auto now = Clock::now();
        for (const auto& event : pending)
        {
            if (!running_.load() || !mqtt_.isConnected()) break;
            const auto existing = sent_.find(event.eventId);
            if (existing != sent_.end())
            {
                if (!resendNow && now - existing->second.lastAttempt <
                    std::chrono::seconds(options_.retrySeconds))
                    continue;
            }
            else if (sent_.size() >= options_.maxInFlight)
            {
                // 上游 SQLite 无丢失，限制长期离线/未 ACK 造成的内存膨胀。
                SPDLOG_WARN("[Outbox] 在途报警达到上限 {}，等待 ACK 后继续", options_.maxInFlight);
                break;
            }
            const auto payload = infrastructure::network::encodeAlertJson(event);
            if (!mqtt_.publish(options_.alertTopic, payload, 1))
            {
                SPDLOG_WARN("[Outbox] 报警发送失败：event_id={}，保持 PENDING", event.eventId);
                break;
            }
            // QoS1 PUBACK 仅说明 Broker 收到；云端 SAVED ACK 之前一直为 PENDING。
            sent_[event.eventId] = SentSnapshot{event, Clock::now()};
            SPDLOG_INFO("[Outbox] 已向 Broker 发送报警，等待业务 ACK：event_id={}", event.eventId);
        }
    }

    void AlertOutboxService::sendHeartbeat()
    {
        if (!metricsProvider_ || !mqtt_.isConnected()) return;
        const auto metrics = metricsProvider_();
        if (!metrics)
        {
            SPDLOG_WARN("[Outbox] 系统资源采集失败，跳过本轮状态心跳");
            return;
        }
        nlohmann::json payload;
        payload["device_id"] = options_.deviceId;
        payload["deployment_area"] = options_.deploymentArea;
        payload["cpu_usage"] = metrics->cpu;
        payload["memory_usage"] = metrics->memory;
        payload["storage_usage"] = metrics->storage;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!mqtt_.publish(statusTopic_, payload.dump(), 0))
            SPDLOG_WARN("[Outbox] 设备状态心跳发送失败");
        else
            SPDLOG_DEBUG("[Outbox] 心跳已发送 CPU={} 内存={} 存储={}", metrics->cpu, metrics->memory, metrics->storage);
    }

    void AlertOutboxService::run() noexcept
    {
        try
        {
            auto nextConnect = Clock::now();
            auto nextRetry = Clock::now();
            auto nextHeartbeat = Clock::now();
            while (running_.load())
            {
                processMessages();
                const auto now = Clock::now();
                if (!mqtt_.isConnected() && now >= nextConnect)
                {
                    (void)mqtt_.connect();
                    nextConnect = Clock::now() + std::chrono::seconds(options_.reconnectSeconds);
                }
                if (mqtt_.isConnected())
                {
                    if (now >= nextRetry || forceResend_.load())
                    {
                        sendPending(forceResend_.exchange(false));
                        nextRetry = Clock::now() + std::chrono::seconds(options_.retrySeconds);
                    }
                    if (now >= nextHeartbeat)
                    {
                        sendHeartbeat();
                        nextHeartbeat = Clock::now() + std::chrono::seconds(options_.heartbeatSeconds);
                    }
                }
                else
                {
                    nextRetry = Clock::now();
                    nextHeartbeat = Clock::now();
                }
                // 心跳与重试时间不受网络消息流量影响；事件更新通过通知主动唤醒。
                std::unique_lock<std::mutex> lock(inboxMutex_);
                wakeCv_.wait_for(lock, std::chrono::seconds(1), [this] {
                    return !running_.load() || wake_ || !inbox_.empty();
                });
                const bool changed = wake_;
                wake_ = false;
                lock.unlock();
                if (changed && mqtt_.isConnected()) nextRetry = Clock::now();
            }
            processMessages();
        }
        catch (const std::exception& e)
        {
            SPDLOG_CRITICAL("[Outbox] 补传线程发生异常：{}；本地待上传记录保持 PENDING", e.what());
        }
        catch (...) { SPDLOG_CRITICAL("[Outbox] 补传线程发生未知异常"); }
        running_.store(false);
        SPDLOG_INFO("[Outbox] 补传线程退出");
    }

    void AlertOutboxService::stop() noexcept
    {
        running_.store(false);
        notifyPersisted();
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id())
            {
                SPDLOG_CRITICAL("[Outbox] 禁止在补传线程中停止自己");
                std::terminate();
            }
            worker_.join();
        }
        mqtt_.setMessageHandler({});
        mqtt_.setConnectionHandler({});
        mqtt_.disconnect();
    }
}
