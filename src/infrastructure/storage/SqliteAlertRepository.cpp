#include "fall_monitoring/infrastructure/storage/SqliteAlertRepository.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace fall_monitoring::infrastructure::storage
{
    namespace
    {
        // Prepared Statement 的自动回收：所有返回路径都执行 sqlite3_finalize()。
        class Statement final
        {
        public:
            Statement(sqlite3* database, const char* sql) : database_(database)
            {
                if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK)
                {
                    SPDLOG_ERROR("[Storage] SQL 预编译失败：{}", sqlite3_errmsg(database_));
                }
            }

            ~Statement()
            {
                if (statement_ != nullptr) sqlite3_finalize(statement_);
            }

            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            explicit operator bool() const noexcept { return statement_ != nullptr; }
            sqlite3_stmt* get() const noexcept { return statement_; }

            bool text(int index, const std::string& value) const
            {
                return sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
            }

            bool integer(int index, int value) const
            {
                return sqlite3_bind_int(statement_, index, value) == SQLITE_OK;
            }

            bool integer64(int index, std::int64_t value) const
            {
                return sqlite3_bind_int64(statement_, index, value) == SQLITE_OK;
            }

        private:
            sqlite3* database_ = nullptr;
            sqlite3_stmt* statement_ = nullptr;
        };

        std::string readText(sqlite3_stmt* statement, int column)
        {
            const auto* text = sqlite3_column_text(statement, column);
            return text != nullptr ? reinterpret_cast<const char*>(text) : "";
        }

        std::string sourcesToText(const std::vector<domain::event::EventSource>& sources)
        {
            std::string result;
            for (const auto source : sources)
            {
                if (!result.empty()) result += ',';
                result += domain::event::toString(source);
            }
            return result;
        }

        std::vector<domain::event::EventSource> sourcesFromText(const std::string& value)
        {
            std::vector<domain::event::EventSource> result;
            std::stringstream input(value);
            std::string field;
            while (std::getline(input, field, ','))
            {
                if (field == "VISION") result.push_back(domain::event::EventSource::VISION);
                else if (field == "VOICE") result.push_back(domain::event::EventSource::VOICE);
            }
            return result;
        }

        domain::event::EventStatus statusFromText(const std::string& value)
        {
            if (value == "ACKNOWLEDGED") return domain::event::EventStatus::ACKNOWLEDGED;
            if (value == "RESOLVED") return domain::event::EventStatus::RESOLVED;
            return domain::event::EventStatus::NEW;
        }

        bool validLimit(std::size_t limit)
        {
            // 避免无界查询占用大量内存或整数转换溢出。
            return limit > 0 && limit <= 1000;
        }
    }

    SqliteAlertRepository::SqliteAlertRepository(std::string path) : path_(std::move(path))
    {
    }

    SqliteAlertRepository::~SqliteAlertRepository()
    {
        close();
    }

    bool SqliteAlertRepository::executeLocked(const char* sql)
    {
        char* message = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (rc == SQLITE_OK) return true;

        SPDLOG_ERROR("[Storage] SQLite 执行失败：rc={}，error={}", rc, message != nullptr ? message : sqlite3_errmsg(db_));
        sqlite3_free(message);
        return false;
    }

    bool SqliteAlertRepository::columnExistsLocked(const char* column, bool& found)
    {
        found = false;
        Statement statement(db_, "PRAGMA table_info(alerts);");
        if (!statement) return false;
        while (true)
        {
            const int rc = sqlite3_step(statement.get());
            if (rc == SQLITE_DONE) return true;
            if (rc != SQLITE_ROW)
            {
                SPDLOG_ERROR("[Storage] 查询表字段失败：{}", sqlite3_errmsg(db_));
                return false;
            }
            if (readText(statement.get(), 1) == column) found = true;
        }
    }

    bool SqliteAlertRepository::initializeSchemaLocked()
    {
        // 字段名称与旧 LocalDatabase 完全一致，旧设备的 .db 文件可以继续使用。
        constexpr const char* createAlerts =
            "CREATE TABLE IF NOT EXISTS alerts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "event_id TEXT NOT NULL UNIQUE,"
            "device_id TEXT NOT NULL,"
            "deployment_area TEXT DEFAULT '',"
            "event_type TEXT NOT NULL,"
            "sources TEXT NOT NULL,"
            "timestamp INTEGER NOT NULL,"
            "person_track_id INTEGER DEFAULT -1,"
            "trigger_x INTEGER DEFAULT 0,"
            "trigger_y INTEGER DEFAULT 0,"
            "keyword TEXT DEFAULT '',"
            "video_path TEXT DEFAULT '',"
            "event_status TEXT NOT NULL,"
            "upload_status TEXT NOT NULL DEFAULT 'PENDING',"
            "video_upload_status TEXT NOT NULL DEFAULT 'NONE',"
            "video_retry_count INTEGER NOT NULL DEFAULT 0,"
            "revision INTEGER NOT NULL DEFAULT 1"
            ");";
        if (!executeLocked(createAlerts)) return false;

        // 只补充历史数据库可能缺失的两列，不删除表、也不丢弃历史报警。
        for (const auto* column : {"video_upload_status", "video_retry_count", "revision"})
        {
            bool exists = false;
            if (!columnExistsLocked(column, exists)) return false;
            if (exists) continue;
            const std::string migration = std::string("ALTER TABLE alerts ADD COLUMN ") +
                (std::string(column) == "video_upload_status"
                    ? "video_upload_status TEXT NOT NULL DEFAULT 'NONE';"
                    : std::string(column) == "video_retry_count"
                        ? "video_retry_count INTEGER NOT NULL DEFAULT 0;"
                        : "revision INTEGER NOT NULL DEFAULT 1;");
            if (!executeLocked(migration.c_str())) return false;
            SPDLOG_INFO("[Storage] 历史数据库迁移：新增字段 {}", column);
        }

        if (!executeLocked(
                "CREATE TABLE IF NOT EXISTS event_ingress ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL);")) return false;
        return executeLocked("CREATE INDEX IF NOT EXISTS idx_alerts_upload ON alerts(upload_status, id);") &&
               executeLocked("CREATE INDEX IF NOT EXISTS idx_alerts_video ON alerts(video_upload_status, id);");
    }

    bool SqliteAlertRepository::initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ != nullptr) return true;
        if (path_.empty())
        {
            SPDLOG_ERROR("[Storage] SQLite 数据库路径不能为空");
            return false;
        }

        try
        {
            const auto parent = std::filesystem::path(path_).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("[Storage] 创建数据库目录失败：{}", e.what());
            return false;
        }

        const int rc = sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK)
        {
            SPDLOG_ERROR("[Storage] SQLite 打开失败：path={}，error={}", path_, db_ != nullptr ? sqlite3_errmsg(db_) : sqlite3_errstr(rc));
            if (db_ != nullptr) sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        // WAL 允许后台补传查询与顺序写入共存；FULL 提高突然断电时的持久性。
        // busy_timeout 防止其他进程短时间持锁就立即导致报警落盘失败。
        if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK ||
            !executeLocked("PRAGMA journal_mode=WAL;") ||
            !executeLocked("PRAGMA synchronous=FULL;") ||
            !initializeSchemaLocked())
        {
            SPDLOG_ERROR("[Storage] SQLite 初始化失败，拒绝启动事件服务");
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        SPDLOG_INFO("[Storage] SQLite 已就绪：path={}，journal=WAL，sync=FULL", path_);
        return true;
    }

    void SqliteAlertRepository::close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr) return;
        const int rc = sqlite3_close(db_);
        if (rc == SQLITE_OK)
        {
            db_ = nullptr;
            SPDLOG_INFO("[Storage] SQLite 数据库已关闭");
        }
        else
        {
            // 不在这里强制 sqlite3_close_v2()，避免错误掩盖未回收的语句。
            SPDLOG_CRITICAL("[Storage] SQLite 关闭失败：{}", sqlite3_errmsg(db_));
        }
    }

    bool SqliteAlertRepository::stashIncoming(const domain::event::AlertEvent& raw)
    {
        try
        {
            nlohmann::json item;
            item["event_id"] = raw.eventId;
            item["event_type"] = domain::event::toString(raw.eventType);
            item["timestamp"] = raw.timestamp;
            item["person_track_id"] = raw.personTrackId;
            item["trigger_x"] = raw.triggerBoxX;
            item["trigger_y"] = raw.triggerBoxY;
            item["keyword"] = raw.keyword;
            item["video_path"] = raw.videoPath;
            item["local_alarm_pretriggered"] = raw.localAlarmAlreadyRequested;
            item["sources"] = nlohmann::json::array();
            for (const auto source : raw.source)
                item["sources"].push_back(domain::event::toString(source));
            const auto encoded = item.dump();
            std::lock_guard<std::mutex> lock(mutex_);
            if (db_ == nullptr) return false;
            Statement stmt(db_, "INSERT INTO event_ingress(payload) VALUES(?);");
            if (!stmt || !stmt.text(1, encoded) || sqlite3_step(stmt.get()) != SQLITE_DONE)
            {
                SPDLOG_CRITICAL("[Storage] 溢出报警无法持久化：{}", sqlite3_errmsg(db_));
                return false;
            }
            return true;
        }
        catch (const std::exception& error)
        {
            SPDLOG_CRITICAL("[Storage] 序列化溢出报警失败：{}", error.what());
            return false;
        }
    }

    bool SqliteAlertRepository::loadIncoming(std::vector<application::ports::PendingIncomingEvent>& batch, std::size_t limit)
    {
        batch.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || !validLimit(limit)) return false;
        Statement stmt(db_, "SELECT id,payload FROM event_ingress ORDER BY id ASC LIMIT ?;");
        if (!stmt || !stmt.integer(1, static_cast<int>(limit))) return false;
        for (;;)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) return true;
            if (rc != SQLITE_ROW)
            {
                SPDLOG_ERROR("[Storage] 读取报警溢出缓存失败：{}", sqlite3_errmsg(db_));
                batch.clear();
                return false;
            }
            try
            {
                application::ports::PendingIncomingEvent incoming;
                incoming.rowId = sqlite3_column_int64(stmt.get(), 0);
                const auto j = nlohmann::json::parse(readText(stmt.get(), 1));
                const auto type = j.at("event_type").get<std::string>();
                if (type == "FALL") incoming.event.eventType = domain::event::EventType::FALL;
                else if (type == "HELP_REQUEST") incoming.event.eventType = domain::event::EventType::HELP_REQUEST;
                else throw std::runtime_error("invalid event_type");
                incoming.event.eventId = j.value("event_id", std::string{});
                incoming.event.timestamp = j.at("timestamp").get<std::int64_t>();
                incoming.event.personTrackId = j.value("person_track_id", -1);
                incoming.event.triggerBoxX = j.value("trigger_x", 0);
                incoming.event.triggerBoxY = j.value("trigger_y", 0);
                incoming.event.keyword = j.value("keyword", std::string{});
                incoming.event.videoPath = j.value("video_path", std::string{});
                incoming.event.localAlarmAlreadyRequested = j.value("local_alarm_pretriggered", false);
                for (const auto& rawSource : j.at("sources"))
                {
                    const auto source = rawSource.get<std::string>();
                    if (source == "VISION") incoming.event.source.push_back(domain::event::EventSource::VISION);
                    else if (source == "VOICE") incoming.event.source.push_back(domain::event::EventSource::VOICE);
                    else throw std::runtime_error("invalid source");
                }
                batch.push_back(std::move(incoming));
            }
            catch (const std::exception& error)
            {
                // 保留损坏记录供人工检查，避免静默丢失报警。
                SPDLOG_CRITICAL("[Storage] 报警溢出记录无法反序列化：{}", error.what());
                batch.clear();
                return false;
            }
        }
    }

    bool SqliteAlertRepository::eraseIncoming(std::int64_t rowId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || rowId <= 0) return false;
        Statement stmt(db_, "DELETE FROM event_ingress WHERE id=?;");
        if (!stmt || !stmt.integer64(1, rowId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 清理已处理报警缓存失败：{}", sqlite3_errmsg(db_));
            return false;
        }
        return sqlite3_changes(db_) == 1;
    }

    bool SqliteAlertRepository::reconcileRecordedVideos(std::size_t limit, std::size_t offset, std::size_t& scanned)
    {
        scanned = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || !validLimit(limit) || offset > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        std::vector<std::pair<std::string, std::string>> candidates;
        {
            Statement stmt(db_,
                "SELECT event_id,video_path FROM alerts "
                "WHERE video_path<>'' AND video_upload_status IN ('NONE','FAILED') "
                "ORDER BY id ASC LIMIT ? OFFSET ?;");
            if (!stmt || !stmt.integer(1, static_cast<int>(limit)) || !stmt.integer(2, static_cast<int>(offset))) return false;
            for (;;)
            {
                const int rc = sqlite3_step(stmt.get());
                if (rc == SQLITE_DONE) break;
                if (rc != SQLITE_ROW)
                {
                    SPDLOG_ERROR("[Storage] 扫描录像恢复候选失败：{}", sqlite3_errmsg(db_));
                    return false;
                }
                candidates.emplace_back(readText(stmt.get(), 0), readText(stmt.get(), 1));
            }
        }
        scanned = candidates.size();
        for (const auto& [id, name] : candidates)
        {
            std::error_code ec;
            const std::filesystem::path path(name);
            if (path.extension() != ".mp4" ||
                !std::filesystem::is_regular_file(path, ec) || ec ||
                std::filesystem::file_size(path, ec) == 0 || ec) continue;
            Statement update(db_,
                "UPDATE alerts SET video_upload_status='PENDING',video_retry_count=0 "
                "WHERE event_id=? AND video_path=? "
                "AND video_upload_status IN ('NONE','FAILED');");
            if (!update || !update.text(1, id) || !update.text(2, name) || sqlite3_step(update.get()) != SQLITE_DONE)
            {
                SPDLOG_ERROR("[Storage] 恢复已生成录像的上传状态失败：{}", id);
                return false;
            }
            if (sqlite3_changes(db_) == 1)
                SPDLOG_INFO("[Storage] 已发现完成的录像文件，恢复待上传任务：{}", id);
        }
        return true;
    }

    bool SqliteAlertRepository::saveAlert(const domain::event::AlertEvent& event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || event.eventId.empty() || event.deviceId.empty() || event.timestamp <= 0)
        {
            SPDLOG_ERROR("[Storage] 拒绝保存无效事件：event_id={}", event.eventId);
            return false;
        }

        // INSERT/UPSERT 本身即为 SQLite 原子事务，sqlite3_step(DONE) 后提交完成。
        // 保留第一次出现的 event_type / timestamp / event_status，不覆盖人工处理状态。
        // 同一 event_id 视觉语音融合时合并来源、位置和关键词，并重新标记 PENDING。
        constexpr const char* sql =
            "INSERT INTO alerts (event_id,device_id,deployment_area,event_type,sources,timestamp,"
            "person_track_id,trigger_x,trigger_y,keyword,video_path,event_status,revision,upload_status)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?, 'PENDING')"
            " ON CONFLICT(event_id) DO UPDATE SET"
            " sources=excluded.sources,"
            " person_track_id=CASE WHEN excluded.person_track_id>=0 THEN excluded.person_track_id ELSE alerts.person_track_id END,"
            " trigger_x=CASE WHEN excluded.person_track_id>=0 THEN excluded.trigger_x ELSE alerts.trigger_x END,"
            " trigger_y=CASE WHEN excluded.person_track_id>=0 THEN excluded.trigger_y ELSE alerts.trigger_y END,"
            " keyword=CASE WHEN excluded.keyword<>'' THEN excluded.keyword ELSE alerts.keyword END,"
            " video_path=CASE WHEN excluded.video_path<>'' THEN excluded.video_path ELSE alerts.video_path END,"
            " deployment_area=excluded.deployment_area,"
            " revision=excluded.revision,"
            " upload_status='PENDING'"
            " WHERE excluded.revision>alerts.revision;";

        Statement stmt(db_, sql);
        if (!stmt) return false;
        const auto sources = sourcesToText(event.source);
        if (!stmt.text(1, event.eventId) ||
            !stmt.text(2, event.deviceId) ||
            !stmt.text(3, event.deploymentArea) ||
            !stmt.text(4, domain::event::toString(event.eventType)) ||
            !stmt.text(5, sources) ||
            !stmt.integer64(6, event.timestamp) ||
            !stmt.integer(7, event.personTrackId) ||
            !stmt.integer(8, event.triggerBoxX) ||
            !stmt.integer(9, event.triggerBoxY) ||
            !stmt.text(10, event.keyword) ||
            !stmt.text(11, event.videoPath) ||
            !stmt.text(12, domain::event::toString(event.status)) ||
            !stmt.integer64(13, static_cast<std::int64_t>(event.revision)))
        {
            SPDLOG_ERROR("[Storage] SQLite 参数绑定失败：{}", sqlite3_errmsg(db_));
            return false;
        }

        if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 报警写入失败：event_id={}，error={}", event.eventId, sqlite3_errmsg(db_));
            return false;
        }
        SPDLOG_INFO("[Storage] 报警已持久化：event_id={}，sources={}，fusion={}，upload=PENDING", event.eventId, sources, event.isFusionUpdate);
        return true;
    }

    bool SqliteAlertRepository::loadPendingAlerts(std::vector<domain::event::AlertEvent>& result, std::size_t limit, std::size_t offset)
    {
        result.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || !validLimit(limit) || offset > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        constexpr const char* sql =
            "SELECT event_id,device_id,deployment_area,event_type,sources,timestamp,"
            "person_track_id,trigger_x,trigger_y,keyword,video_path,event_status,revision "
            "FROM alerts WHERE upload_status='PENDING' ORDER BY id ASC LIMIT ? OFFSET ?;";
        Statement stmt(db_, sql);
        if (!stmt || !stmt.integer(1, static_cast<int>(limit)) || !stmt.integer(2, static_cast<int>(offset))) return false;
        while (true)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) return true;
            if (rc != SQLITE_ROW)
            {
                SPDLOG_ERROR("[Storage] 读取待上传报警失败：{}", sqlite3_errmsg(db_));
                result.clear();
                return false;
            }
            domain::event::AlertEvent event;
            event.eventId = readText(stmt.get(), 0);
            event.deviceId = readText(stmt.get(), 1);
            event.deploymentArea = readText(stmt.get(), 2);
            event.eventType = readText(stmt.get(), 3) == "FALL" ? domain::event::EventType::FALL : domain::event::EventType::HELP_REQUEST;
            event.source = sourcesFromText(readText(stmt.get(), 4));
            event.timestamp = sqlite3_column_int64(stmt.get(), 5);
            event.personTrackId = sqlite3_column_int(stmt.get(), 6);
            event.triggerBoxX = sqlite3_column_int(stmt.get(), 7);
            event.triggerBoxY = sqlite3_column_int(stmt.get(), 8);
            event.keyword = readText(stmt.get(), 9);
            event.videoPath = readText(stmt.get(), 10);
            event.status = statusFromText(readText(stmt.get(), 11));
            event.revision = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 12));
            result.push_back(std::move(event));
        }
    }

    bool SqliteAlertRepository::rowExistsLocked(const std::string& eventId)
    {
        Statement stmt(db_, "SELECT 1 FROM alerts WHERE event_id=? LIMIT 1;");
        return stmt && stmt.text(1, eventId) && sqlite3_step(stmt.get()) == SQLITE_ROW;
    }

    application::ports::AckCommitResult SqliteAlertRepository::markAsUploadedIfMatches(const domain::event::AlertEvent& sent)
    {
        using Result = application::ports::AckCommitResult;
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || sent.eventId.empty()) return Result::Error;
        constexpr const char* sql =
            "UPDATE alerts SET upload_status='UPLOADED' "
            "WHERE event_id=? AND upload_status='PENDING' AND device_id=? "
            "AND deployment_area=? AND event_type=? AND sources=? AND timestamp=? "
            "AND person_track_id=? AND trigger_x=? AND trigger_y=? AND keyword=? "
            "AND video_path=? AND event_status=? AND revision=?;";
        Statement stmt(db_, sql);
        if (!stmt || !stmt.text(1, sent.eventId) || !stmt.text(2, sent.deviceId) ||
            !stmt.text(3, sent.deploymentArea) ||
            !stmt.text(4, domain::event::toString(sent.eventType)) ||
            !stmt.text(5, sourcesToText(sent.source)) ||
            !stmt.integer64(6, sent.timestamp) ||
            !stmt.integer(7, sent.personTrackId) ||
            !stmt.integer(8, sent.triggerBoxX) ||
            !stmt.integer(9, sent.triggerBoxY) ||
            !stmt.text(10, sent.keyword) || !stmt.text(11, sent.videoPath) ||
            !stmt.text(12, domain::event::toString(sent.status)) ||
            !stmt.integer64(13, static_cast<std::int64_t>(sent.revision)))
        {
            SPDLOG_ERROR("[Storage] 业务 ACK SQL 参数绑定失败：{}", sqlite3_errmsg(db_));
            return Result::Error;
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 业务 ACK SQL 更新失败：{}", sqlite3_errmsg(db_));
            return Result::Error;
        }
        return sqlite3_changes(db_) > 0 ? Result::Uploaded : Result::Stale;
    }

    bool SqliteAlertRepository::markAsUploaded(const std::string& eventId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty()) return false;
        Statement stmt(db_, "UPDATE alerts SET upload_status='UPLOADED' WHERE event_id=? AND upload_status='PENDING';");
        if (!stmt || !stmt.text(1, eventId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 更新报警上传状态失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        if (sqlite3_changes(db_) == 0 && !rowExistsLocked(eventId))
        {
            SPDLOG_WARN("[Storage] 收到未知 event_id 的业务 ACK：{}", eventId);
            return false;
        }
        SPDLOG_INFO("[Storage] 报警业务 ACK 已确认：event_id={}，upload=UPLOADED", eventId);
        return true;
    }

    bool SqliteAlertRepository::markVideoPending(const std::string& eventId, const std::string& videoPath)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty() || videoPath.empty()) return false;
        Statement stmt(db_, "UPDATE alerts SET video_path=?,video_upload_status='PENDING',video_retry_count=0 WHERE event_id=?;");
        if (!stmt || !stmt.text(1, videoPath) || !stmt.text(2, eventId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 设置录像待上传失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        if (sqlite3_changes(db_) == 0 && !rowExistsLocked(eventId)) return false;
        SPDLOG_INFO("[Storage] 录像已进入待上传状态：event_id={}，path={}", eventId, videoPath);
        return true;
    }

    bool SqliteAlertRepository::markVideoFailed(const std::string& eventId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty()) return false;
        Statement stmt(db_,
            "UPDATE alerts SET video_upload_status='FAILED' "
            "WHERE event_id=? AND video_upload_status!='UPLOADED';");
        if (!stmt || !stmt.text(1, eventId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 标记录像失败出错：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        return sqlite3_changes(db_) == 1 || rowExistsLocked(eventId);
    }

    bool SqliteAlertRepository::loadPendingVideos(std::vector<application::ports::PendingVideoUpload>& result, std::size_t limit, std::size_t offset)
    {
        result.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || !validLimit(limit) || offset > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        Statement stmt(db_,
            "SELECT event_id,video_path,video_retry_count FROM alerts "
            "WHERE video_upload_status='PENDING' AND video_path<>'' ORDER BY id ASC LIMIT ? OFFSET ?;");
        if (!stmt || !stmt.integer(1, static_cast<int>(limit)) || !stmt.integer(2, static_cast<int>(offset))) return false;
        while (true)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) return true;
            if (rc != SQLITE_ROW)
            {
                SPDLOG_ERROR("[Storage] 读取待上传录像失败：{}", sqlite3_errmsg(db_));
                result.clear();
                return false;
            }
            application::ports::PendingVideoUpload video;
            video.eventId = readText(stmt.get(), 0);
            video.videoPath = readText(stmt.get(), 1);
            video.retryCount = sqlite3_column_int(stmt.get(), 2);
            result.push_back(std::move(video));
        }
    }

    application::ports::AlertCloudState SqliteAlertRepository::getAlertCloudState(const std::string& eventId)
    {
        using State = application::ports::AlertCloudState;
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty()) return State::Error;
        Statement stmt(db_, "SELECT upload_status FROM alerts WHERE event_id=? LIMIT 1;");
        if (!stmt || !stmt.text(1, eventId)) return State::Error;
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return State::Missing;
        if (rc != SQLITE_ROW)
        {
            SPDLOG_ERROR("[Storage] 查询报警云端状态失败：{}", sqlite3_errmsg(db_));
            return State::Error;
        }
        return readText(stmt.get(), 0) == "UPLOADED" ? State::Uploaded : State::Pending;
    }

    application::ports::VideoCommitResult SqliteAlertRepository::markVideoUploadedIfMatches(const std::string& eventId, const std::string& videoPath)
    {
        using Result = application::ports::VideoCommitResult;
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty() || videoPath.empty()) return Result::Error;
        Statement stmt(db_,
            "UPDATE alerts SET video_upload_status='UPLOADED' "
            "WHERE event_id=? AND video_path=? AND video_upload_status='PENDING';");
        if (!stmt || !stmt.text(1, eventId) || !stmt.text(2, videoPath) ||
            sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 录像条件确认失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return Result::Error;
        }
        return sqlite3_changes(db_) == 1 ? Result::Uploaded : Result::Stale;
    }

    bool SqliteAlertRepository::incrementVideoRetryIfMatches(const std::string& eventId, const std::string& videoPath)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty() || videoPath.empty()) return false;
        Statement stmt(db_,
            "UPDATE alerts SET video_retry_count=video_retry_count+1 "
            "WHERE event_id=? AND video_path=? AND video_upload_status='PENDING';");
        if (!stmt || !stmt.text(1, eventId) || !stmt.text(2, videoPath) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 录像重试次数更新失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        return sqlite3_changes(db_) == 1;
    }

    bool SqliteAlertRepository::markVideoUploaded(const std::string& eventId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty()) return false;

        Statement stmt(db_, "UPDATE alerts SET video_upload_status='UPLOADED' WHERE event_id=? AND video_upload_status='PENDING';");
        if (!stmt || !stmt.text(1, eventId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 更新录像上传状态失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        if (sqlite3_changes(db_) == 0 && !rowExistsLocked(eventId)) return false;
        SPDLOG_INFO("[Storage] 录像已标记为上传完成：event_id={}", eventId);
        return true;
    }

    bool SqliteAlertRepository::incrementVideoRetry(const std::string& eventId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || eventId.empty()) return false;
        Statement stmt(db_,
            "UPDATE alerts SET video_retry_count=video_retry_count+1 "
            "WHERE event_id=? AND video_upload_status='PENDING';");
        if (!stmt || !stmt.text(1, eventId) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            SPDLOG_ERROR("[Storage] 更新视频重试次数失败：event_id={}，error={}", eventId, sqlite3_errmsg(db_));
            return false;
        }
        return sqlite3_changes(db_) == 1;
    }
}
