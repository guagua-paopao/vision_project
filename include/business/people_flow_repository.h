#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "business/people_flow_types.h"
#include "server/app_config.h"

namespace yolo11_server {

    struct PeopleFlowSessionRecord {
        std::string session_id;
        std::string camera_id;
        std::string status;
        long long start_time_ms = 0;
        long long stop_time_ms = 0;
        long long initial_occupancy = 0;
        long long in_count = 0;
        long long out_count = 0;
        long long final_occupancy = 0;
        std::string config_version;
        std::string stop_reason;
        std::string error;
        bool consistency_ok = true;
    };

    struct PeopleFlowSummaryBucket {
        long long bucket_start_ms = 0;
        long long in_count = 0;
        long long out_count = 0;
        long long net_count = 0;
    };

    struct PeopleFlowCalibrationRecord {
        std::string camera_id;
        std::string session_id;
        long long before_occupancy = 0;
        long long after_occupancy = 0;
        std::string reason;
        std::string operator_name;
        long long timestamp_ms = 0;
    };

    struct PeopleFlowRepositoryHealth {
        bool started = false;
        bool writer_enabled = false;
        bool degraded = false;
        std::size_t queue_depth = 0;
        std::size_t queue_capacity = 0;
        long long written_events = 0;
        long long duplicate_events = 0;
        long long failed_batches = 0;
        long long dropped_tasks = 0;
        std::string last_error;
    };

    class PeopleFlowRepository {
    public:
        explicit PeopleFlowRepository(const PeopleFlowSection& config);
        ~PeopleFlowRepository() noexcept;

        PeopleFlowRepository(const PeopleFlowRepository&) = delete;
        PeopleFlowRepository& operator=(const PeopleFlowRepository&) = delete;

        // Query-only HTTP processes pass false; the inference worker passes true.
        // A false return means the initial schema attempt failed. When writer is
        // enabled the retry thread still starts so inference can continue degraded.
        bool start(bool enable_writer, std::string& error);
        void stop() noexcept;

        bool enqueueSessionStart(const PeopleFlowSessionRecord& session);
        bool enqueueEvent(const CrossingEvent& event);
        bool enqueueSessionFinish(const PeopleFlowSessionRecord& session);

        bool queryEvents(
            const std::string& camera_id,
            const std::string& direction,
            long long from_ms,
            long long to_ms,
            int limit,
            int offset,
            std::vector<CrossingEvent>& events,
            std::string& error
        ) const;
        bool querySummary(
            const std::string& camera_id,
            long long from_ms,
            long long to_ms,
            const std::string& bucket,
            std::vector<PeopleFlowSummaryBucket>& summary,
            std::string& error
        ) const;
        bool getSession(const std::string& session_id, PeopleFlowSessionRecord& session, bool& found, std::string& error) const;
        bool writeCalibration(const PeopleFlowCalibrationRecord& record, std::string& error) const;
        bool finalizeSessionSync(const PeopleFlowSessionRecord& session, std::string& error) const;
        bool cleanupRetention(long long now_ms, std::string& error) const;

        PeopleFlowRepositoryHealth health() const;

    private:
        enum class TaskType { SessionStart, Event, SessionFinish };
        struct WriteTask {
            TaskType type = TaskType::Event;
            PeopleFlowSessionRecord session;
            CrossingEvent event;
        };

        bool enqueue(WriteTask task);
        void writerLoop() noexcept;
        bool ensureSchema(std::string& error) const;
        bool writeBatch(const std::vector<WriteTask>& batch, std::string& error);
        void setError(const std::string& error, bool degraded);

    private:
        PeopleFlowSection config_;
        mutable std::mutex queue_mutex_;
        std::condition_variable queue_cv_;
        std::deque<WriteTask> queue_;
        std::thread writer_thread_;
        std::atomic<bool> stop_requested_{ false };
        std::atomic<bool> started_{ false };
        std::atomic<bool> writer_enabled_{ false };
        std::atomic<bool> degraded_{ false };
        std::atomic<long long> written_events_{ 0 };
        std::atomic<long long> duplicate_events_{ 0 };
        std::atomic<long long> failed_batches_{ 0 };
        std::atomic<long long> dropped_tasks_{ 0 };
        mutable std::mutex error_mutex_;
        std::string last_error_;
    };

}  // namespace yolo11_server
