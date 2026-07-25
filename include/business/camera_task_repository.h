#pragma once

#include <string>
#include <vector>

#include "business/camera_task_types.h"
#include "server/app_config.h"

namespace yolo11_server {

struct CameraTaskRepositoryStats {
    long long tasks_total = 0;
    long long tasks_enabled = 0;
    long long tasks_deleted = 0;
    long long runs_total = 0;
    long long runs_active = 0;
    long long runs_failed = 0;
    long long frames_total = 0;
    long long alerts_total = 0;
    long long callbacks_pending = 0;
    long long callbacks_delivering = 0;
    long long callbacks_delivered = 0;
    long long callbacks_retry = 0;
    long long callbacks_dead_letter = 0;
    long long archive_bytes = 0;
    long long latest_frame_time_ms = 0;
};

class CameraTaskRepository final {
public:
    explicit CameraTaskRepository(const CameraTasksSection& config);

    bool initialize(std::string& error) const;

    bool createTask(
        const CameraTaskDefinition& task,
        std::string& error_code,
        std::string& error
    ) const;
    bool getTask(
        const std::string& task_id,
        bool include_deleted,
        CameraTaskDefinition& task,
        bool& found,
        std::string& error
    ) const;
    bool listTasks(
        bool include_deleted,
        int limit,
        int offset,
        std::vector<CameraTaskDefinition>& tasks,
        std::string& error
    ) const;
    bool updateTask(
        const std::string& task_id,
        int expected_version,
        const CameraTaskPatch& patch,
        long long update_time_ms,
        CameraTaskDefinition& updated,
        std::string& error_code,
        std::string& error
    ) const;
    bool softDeleteTask(
        const std::string& task_id,
        int expected_version,
        long long delete_time_ms,
        std::string& error_code,
        std::string& error
    ) const;

    bool createRun(
        const CameraTaskRunRecord& run,
        std::string& error_code,
        std::string& error
    ) const;
    bool getRun(
        const std::string& run_id,
        CameraTaskRunRecord& run,
        bool& found,
        std::string& error
    ) const;
    bool listRuns(
        const std::string& task_id,
        int limit,
        int offset,
        std::vector<CameraTaskRunRecord>& runs,
        std::string& error
    ) const;
    bool transitionRun(
        const std::string& run_id,
        const std::vector<std::string>& allowed_from,
        const CameraTaskRunRecord& next,
        std::string& error_code,
        std::string& error
    ) const;
    bool updateRunProgress(const CameraTaskRunRecord& run, std::string& error) const;
    bool recoverStaleRuns(
        long long stale_before_ms,
        long long now_ms,
        int& recovered_count,
        std::string& error
    ) const;
    bool upsertRunAnalysisResult(
        const CameraRunAnalysisResultRecord& result,
        std::string& error
    ) const;
    bool getRunAnalysisResult(
        const std::string& run_id,
        CameraRunAnalysisResultRecord& result,
        bool& found,
        std::string& error
    ) const;

    bool insertFrame(const CameraFrameArtifact& frame, std::string& error) const;
    bool getLatestFrame(
        const std::string& task_id,
        CameraFrameArtifact& frame,
        bool& found,
        std::string& error
    ) const;
    bool listFrames(
        const std::string& task_id,
        const std::string& run_id,
        int limit,
        int offset,
        std::vector<CameraFrameArtifact>& frames,
        std::string& error
    ) const;
    bool listRetentionCandidates(
        long long now_ms,
        int limit,
        std::vector<CameraFrameArtifact>& frames,
        std::string& error
    ) const;
    bool listOldestFrames(
        int limit,
        std::vector<CameraFrameArtifact>& frames,
        std::string& error
    ) const;
    bool deleteFrameMetadata(const std::string& frame_id, std::string& error) const;

    bool insertAlert(
        const SecurityAlertEventRecord& alert,
        std::string& error_code,
        std::string& error
    ) const;
    bool insertAlert(
        const SecurityAlertEventRecord& alert,
        const std::string& callback_profile,
        std::string& error_code,
        std::string& error
    ) const;
    bool getAlert(
        const std::string& event_id,
        SecurityAlertEventRecord& alert,
        bool& found,
        std::string& error
    ) const;
    bool listAlerts(
        const std::string& task_id,
        const std::string& event_type,
        int minimum_severity,
        int limit,
        int offset,
        std::vector<SecurityAlertEventRecord>& alerts,
        std::string& error
    ) const;
    bool claimDueCallback(
        long long now_ms,
        int lease_timeout_ms,
        CallbackOutboxRecord& outbox,
        bool& found,
        std::string& error
    ) const;
    bool markCallbackDelivered(
        long long outbox_id,
        int attempt,
        long long delivered_at_ms,
        int http_status,
        const std::string& response_body_hash,
        std::string& error
    ) const;
    bool finishCallbackAttempt(
        long long outbox_id,
        int attempt,
        const std::string& next_status,
        long long next_attempt_at_ms,
        long long update_time_ms,
        int http_status,
        const std::string& error_code,
        const std::string& response_body_hash,
        std::string& error
    ) const;
    bool listCallbackOutbox(
        const std::string& status,
        const std::string& task_id,
        int limit,
        int offset,
        std::vector<CallbackOutboxRecord>& records,
        std::string& error
    ) const;
    bool requeueDeadCallback(
        long long outbox_id,
        int expected_attempt,
        long long update_time_ms,
        CallbackOutboxRecord& record,
        std::string& error_code,
        std::string& error
    ) const;

    bool getIdempotencyRecord(
        const std::string& operation_scope,
        const std::string& idempotency_key,
        CameraIdempotencyRecord& record,
        bool& found,
        std::string& error
    ) const;
    bool storeIdempotencyRecord(
        const CameraIdempotencyRecord& record,
        std::string& error_code,
        std::string& error
    ) const;
    bool stats(CameraTaskRepositoryStats& stats, std::string& error) const;

private:
    CameraTasksSection config_;
};

}  // namespace yolo11_server
