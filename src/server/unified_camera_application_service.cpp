#include "server/unified_camera_application_service.h"

#include <atomic>
#include <chrono>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace yolo11_server {

namespace {

using json = nlohmann::json;

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

long long effectiveTime(long long operation_time_ms) {
    return operation_time_ms > 0 ? operation_time_ms : wallNowMs();
}

std::string makeRunId() {
    static std::atomic<unsigned long long> sequence{ 0 };
    std::ostringstream output;
    output << "cr_" << std::hex << wallNowMs() << '_' << ++sequence;
    return output.str();
}

std::string definitionJson(const CameraTaskDefinition& task) {
    return json({
        {"camera_profile", task.camera_profile},
        {"frame_interval_ms", task.frame_interval_ms},
        {"output_mode", task.output_mode},
        {"jpeg_quality", task.jpeg_quality},
        {"max_width", task.max_width},
        {"max_height", task.max_height},
        {"retention_days", task.retention_days},
        {"max_saved_frames", task.max_saved_frames},
        {"desired_state", task.desired_state},
        {"analysis", {
            {"enabled", task.analysis_enabled},
            {"target_infer_fps", task.target_infer_fps},
            {"algorithm_profile", task.algorithm_profile},
            {"algorithms", task.algorithms}
        }},
        {"callback_profile", task.callback_profile}
    }).dump();
}

CameraTaskCommand commandFrom(
    const CameraTaskDefinition& task,
    const CameraTaskRunRecord& run
) {
    CameraTaskCommand command;
    command.task_id = task.task_id;
    command.run_id = run.run_id;
    command.camera_profile = task.camera_profile;
    command.definition_version = task.version;
    command.frame_interval_ms = task.frame_interval_ms;
    command.output_mode = task.output_mode;
    command.jpeg_quality = task.jpeg_quality;
    command.max_width = task.max_width;
    command.max_height = task.max_height;
    command.retention_days = task.retention_days;
    command.max_saved_frames = task.max_saved_frames;
    command.analysis_enabled = task.analysis_enabled;
    command.target_infer_fps = task.target_infer_fps;
    command.algorithm_profile = task.algorithm_profile;
    command.algorithms = task.algorithms;
    command.callback_profile = task.callback_profile;
    command.create_time_ms = run.create_time_ms;
    return command;
}

}  // namespace

UnifiedCameraApplicationService::UnifiedCameraApplicationService(
    AppConfig config,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICameraTaskApiControl> control,
    std::shared_ptr<CameraProfileRegistry> profile_registry
) : config_(std::move(config)),
    repository_(std::move(repository)),
    control_(std::move(control)),
    profile_registry_(std::move(profile_registry)) {
}

std::unique_lock<std::recursive_mutex>
UnifiedCameraApplicationService::lockLifecycle() const {
    return std::unique_lock<std::recursive_mutex>(lifecycle_mutex_);
}

bool UnifiedCameraApplicationService::getActiveRun(
    const std::string& task_id,
    CameraTaskRunRecord& run,
    bool& found,
    std::string& error,
    bool include_stopping
) const {
    found = false;
    error.clear();
    if (!repository_) {
        error = "camera task repository is unavailable";
        return false;
    }
    std::vector<CameraTaskRunRecord> runs;
    if (!repository_->listRuns(task_id, 100, 0, runs, error)) return false;
    CameraTaskRunRecord stopping;
    bool stopping_found = false;
    for (const auto& candidate : runs) {
        if (!isCameraRunActive(candidate.status)) continue;
        if (candidate.status != "stopping") {
            run = candidate;
            found = true;
            return true;
        }
        if (include_stopping && !stopping_found) {
            stopping = candidate;
            stopping_found = true;
        }
    }
    if (stopping_found) {
        run = stopping;
        found = true;
    }
    return true;
}

bool UnifiedCameraApplicationService::startCamera(
    const std::string& task_id,
    CameraStartApplicationResult& result,
    std::string& error_code,
    std::string& error,
    long long operation_time_ms
) {
    auto lifecycle_lock = lockLifecycle();
    result = CameraStartApplicationResult{};
    error_code.clear();
    error.clear();
    if (!repository_ || !control_) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        error = "camera application dependencies are unavailable";
        return false;
    }

    CameraTaskDefinition task;
    bool found = false;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        return false;
    }
    if (!found) {
        result.failure = CameraApplicationFailure::not_found;
        error_code = "TASK_NOT_FOUND";
        return false;
    }

    const long long now_ms = effectiveTime(operation_time_ms);
    if (!task.enabled) {
        CameraTaskPatch start_patch;
        start_patch.enabled = true;
        start_patch.desired_state = "running";
        CameraTaskDefinition updated;
        std::string update_code;
        if (!repository_->updateTask(
                task_id,
                task.version,
                start_patch,
                now_ms,
                updated,
                update_code,
                error)) {
            result.failure = update_code == "TASK_VERSION_CONFLICT"
                ? CameraApplicationFailure::conflict
                : CameraApplicationFailure::unavailable;
            error_code = update_code.empty() ? "STORAGE_UNAVAILABLE" : update_code;
            return false;
        }
        task = std::move(updated);
    }

    CameraProfile resolved_profile;
    bool profile_found = false;
    if (profile_registry_) {
        if (!profile_registry_->get(
                task.camera_profile,
                false,
                resolved_profile,
                profile_found,
                error)) {
            result.failure = CameraApplicationFailure::unavailable;
            error_code = "STORAGE_UNAVAILABLE";
            return false;
        }
    }
    else {
        const auto profile = config_.camera_profiles.find(task.camera_profile);
        profile_found = profile != config_.camera_profiles.end();
        if (profile_found) resolved_profile = profile->second;
    }
    if (!profile_found) {
        result.failure = CameraApplicationFailure::invalid_request;
        error_code = "CAMERA_PROFILE_NOT_FOUND";
        return false;
    }
    if (!resolved_profile.enabled) {
        result.failure = CameraApplicationFailure::conflict;
        error_code = "CAMERA_PROFILE_DISABLED";
        return false;
    }

    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error, false)) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        return false;
    }
    if (active_found) {
        result.task = std::move(task);
        result.run = std::move(active);
        result.idempotent_replay = true;
        return true;
    }

    CameraTaskRunRecord run;
    run.run_id = makeRunId();
    run.task_id = task.task_id;
    run.definition_version = task.version;
    run.definition_json = definitionJson(task);
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = now_ms;
    run.last_update_ms = now_ms;

    std::string repository_code;
    if (!repository_->createRun(run, repository_code, error)) {
        if (repository_code == "ACTIVE_RUN_EXISTS" &&
            getActiveRun(task_id, active, active_found, error, false) &&
            active_found) {
            result.task = std::move(task);
            result.run = std::move(active);
            result.idempotent_replay = true;
            result.recovered_active_run_conflict = true;
            return true;
        }
        result.failure =
            repository_code == "TASK_DISABLED" ||
            repository_code == "TASK_VERSION_CONFLICT"
                ? CameraApplicationFailure::conflict
                : CameraApplicationFailure::unavailable;
        error_code = repository_code.empty()
            ? "STORAGE_UNAVAILABLE"
            : repository_code;
        return false;
    }

    CameraTaskCommand command = commandFrom(task, run);
    if (!control_->submitStart(command, error)) {
        CameraTaskRunRecord failed = run;
        failed.status = "failed";
        failed.stop_time_ms = now_ms;
        failed.last_update_ms = now_ms;
        failed.stop_reason = "queue_submit_failed";
        failed.error_code = "QUEUE_SUBMIT_FAILED";
        failed.error_message = "camera task queue submission failed";
        std::string transition_code;
        std::string transition_error;
        repository_->transitionRun(
            run.run_id,
            { "queued" },
            failed,
            transition_code,
            transition_error);
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "QUEUE_SUBMIT_FAILED";
        return false;
    }

    result.task = std::move(task);
    result.run = std::move(run);
    return true;
}

bool UnifiedCameraApplicationService::stopCamera(
    const std::string& task_id,
    CameraStopApplicationResult& result,
    std::string& error_code,
    std::string& error,
    long long operation_time_ms
) {
    auto lifecycle_lock = lockLifecycle();
    result = CameraStopApplicationResult{};
    error_code.clear();
    error.clear();
    if (!repository_ || !control_) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        error = "camera application dependencies are unavailable";
        return false;
    }

    CameraTaskDefinition task;
    bool task_found = false;
    if (!repository_->getTask(task_id, false, task, task_found, error)) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        return false;
    }
    if (!task_found) {
        result.failure = CameraApplicationFailure::not_found;
        error_code = "TASK_NOT_FOUND";
        return false;
    }

    const long long now_ms = effectiveTime(operation_time_ms);
    if (task.enabled || task.desired_state != "stopped") {
        CameraTaskPatch stop_patch;
        stop_patch.enabled = false;
        stop_patch.desired_state = "stopped";
        CameraTaskDefinition updated;
        std::string update_code;
        if (!repository_->updateTask(
                task_id,
                task.version,
                stop_patch,
                now_ms,
                updated,
                update_code,
                error)) {
            result.failure = update_code == "TASK_VERSION_CONFLICT"
                ? CameraApplicationFailure::conflict
                : CameraApplicationFailure::unavailable;
            error_code = update_code.empty() ? "STORAGE_UNAVAILABLE" : update_code;
            return false;
        }
        task = std::move(updated);
    }

    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error)) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "STORAGE_UNAVAILABLE";
        return false;
    }
    if (!active_found) {
        std::vector<CameraTaskRunRecord> runs;
        if (!repository_->listRuns(task_id, 1, 0, runs, error)) {
            result.failure = CameraApplicationFailure::unavailable;
            error_code = "STORAGE_UNAVAILABLE";
            return false;
        }
        result.task = std::move(task);
        result.run_id = runs.empty() ? std::string{} : runs.front().run_id;
        result.status = runs.empty() ? "idle" : runs.front().status;
        result.idempotent_replay = true;
        return true;
    }

    if (!control_->requestStop(active.run_id, error)) {
        result.failure = CameraApplicationFailure::unavailable;
        error_code = "QUEUE_SUBMIT_FAILED";
        return false;
    }
    const bool already_stopping = active.status == "stopping";
    if (!already_stopping) {
        CameraTaskRunRecord stopping = active;
        stopping.status = "stopping";
        stopping.last_update_ms = now_ms;
        std::string transition_code;
        std::string transition_error;
        repository_->transitionRun(
            active.run_id,
            { "queued", "starting", "running", "reconnecting" },
            stopping,
            transition_code,
            transition_error);
    }

    result.task = std::move(task);
    result.run_id = active.run_id;
    result.status = "stopping";
    result.idempotent_replay = already_stopping;
    return true;
}

}  // namespace yolo11_server
