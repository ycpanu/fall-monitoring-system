#pragma once

#include "fall_monitoring/application/ports/IAlertRepository.hpp"

#include <mutex>
#include <string>

struct sqlite3;

namespace fall_monitoring::infrastructure::storage
{
    /** @brief 本地 SQLite WAL 报警仓储；内部互斥锁保护共享连接。 */
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
        [[nodiscard]] bool loadPendingAlerts(std::vector<domain::event::AlertEvent>& result, std::size_t limit = 100, std::size_t offset = 0) override;
        [[nodiscard]] bool markAsUploaded(const std::string& eventId) override;
        [[nodiscard]] application::ports::AckCommitResult markAsUploadedIfMatches(const domain::event::AlertEvent& sentSnapshot) override;
        [[nodiscard]] bool markVideoPending(const std::string& eventId, const std::string& videoPath) override;
        [[nodiscard]] bool markVideoFailed(const std::string& eventId) override;
        [[nodiscard]] bool loadPendingVideos(std::vector<application::ports::PendingVideoUpload>& result, std::size_t limit = 100, std::size_t offset = 0) override;
        [[nodiscard]] bool markVideoUploaded(const std::string& eventId) override;
        [[nodiscard]] bool incrementVideoRetry(const std::string& eventId) override;
        [[nodiscard]] application::ports::AlertCloudState getAlertCloudState(const std::string& eventId) override;
        [[nodiscard]] application::ports::VideoCommitResult markVideoUploadedIfMatches(const std::string& eventId, const std::string& videoPath) override;
        [[nodiscard]] bool incrementVideoRetryIfMatches(const std::string& eventId, const std::string& videoPath) override;

    private:
        [[nodiscard]] bool executeLocked(const char* sql);
        [[nodiscard]] bool columnExistsLocked(const char* column, bool& found);
        [[nodiscard]] bool rowExistsLocked(const std::string& eventId);
        [[nodiscard]] bool initializeSchemaLocked();

        std::string path_;
        sqlite3* db_ = nullptr;
        mutable std::mutex mutex_;
    };
} // namespace fall_monitoring::infrastructure::storage
