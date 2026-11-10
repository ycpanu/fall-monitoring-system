#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"

#include <mutex>
#include <string>

struct sqlite3;

namespace fall_monitoring::infrastructure::storage
{
    /**
     * @brief SQLite 本次报警仓储适配器
     * 
     * 所有 SQLite 操作共用一个连接，并通过互斥锁串行化。
     * 生命周期：Application 先 initialize()，再 start() EventService。
     * 停机时先 stop() EventService，再关闭本仓储。
     */
    class SqliteAlertRepository final : public application::ports::IAlertRepository
    {
    public:
        explicit SqliteAlertRepository(std::string path);
        ~SqliteAlertRepository() override;

        SqliteAlertRepository(const SqliteAlertRepository&) = delete;
        SqliteAlertRepository& operator=(const SqliteAlertRepository&) = delete;
        SqliteAlertRepository(SqliteAlertRepository&&) = delete;
        SqliteAlertRepository& operator=(SqliteAlertRepository&&) = delete;

        [[nodiscard]] bool initialize();
        void close() noexcept;

        [[nodiscard]] bool saveAlert(const domain::event::AlertEvent& event) override;
        [[nodiscard]] bool loadPendingAlerts(std::vector<domain::event::AlertEvent>& result, std::size_t limit = 100) override;
        [[nodiscard]] bool markAsUploaded(const std::string& eventId) override;

        [[nodiscard]] bool markVideoPending(const std::string& eventId, const std::string& videoPath) override;
        [[nodiscard]] bool loadPendingVideos(std::vector<application::ports::PendingVideoUpload>& result, std::size_t limit = 100) override;
        [[nodiscard]] bool markVideoUploaded(const std::string& eventId) override;
        [[nodiscard]] bool incrementVideoRetry(const std::string& eventId) override;

    private:
        // 以下私有函数仅在 mutex_ 已持有时调用。
        [[nodiscard]] bool executeLocked(const char* sql);
        [[nodiscard]] bool columnExistsLocked(const char* column, bool& found);
        [[nodiscard]] bool rowExistsLocked(const std::string& eventId);
        [[nodiscard]] bool initializeSchemaLocked();

        std::string path_;
        sqlite3* db_ = nullptr;
        mutable std::mutex mutex_;
    };
}