#include "server/camera_task_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "business/camera_pipeline.h"
#include "business/camera_frame_retention.h"
#include "business/camera_task_repository.h"
#include "business/frame_artifact_writer.h"
#include "server/camera_task_queue.h"

namespace yolo11_server {

namespace {

using nlohmann::json;

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string makeRecoveryRunId(long long now_ms) {
    static std::atomic<unsigned long long> sequence{ 0 };
    return "cr_recovery_" + std::to_string(now_ms) + "_" +
        std::to_string(sequence.fetch_add(1));
}

CameraTaskCommand recoveryCommand(
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

std::string recoveryDefinitionJson(const CameraTaskDefinition& task) {
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

bool acquireRecoveryOwnership(
    const CameraTasksSection& config,
    const std::shared_ptr<CameraTaskQueue>& runtime_control,
    const CameraTaskRunRecord& run,
    std::string& error
) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(config.lease_ttl_seconds + 5);
    std::string lease_error;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runtime_control->acquireRunLease(
                run.task_id, run.run_id, lease_error)) {
            error.clear();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    error = "CAMERA_RECOVERY_LEASE_ACTIVE";
    if (!lease_error.empty()) error += ": " + lease_error;
    return false;
}

bool prepareRecoveryCommands(
    const AppConfig& config,
    const std::shared_ptr<CameraTaskRepository>& repository,
    const std::shared_ptr<CameraTaskQueue>& runtime_control,
    std::vector<CameraTaskCommand>& commands,
    int& recovered_runs,
    std::string& error
) {
    commands.clear();
    recovered_runs = 0;
    std::vector<CameraTaskDefinition> tasks;
    constexpr int page_size = 100;
    for (int offset = 0;; offset += page_size) {
        std::vector<CameraTaskDefinition> page;
        if (!repository->listTasks(
                true, page_size, offset, page, error)) {
            return false;
        }
        tasks.insert(tasks.end(), page.begin(), page.end());
        if (static_cast<int>(page.size()) < page_size) break;
    }
    const auto desired_count = std::count_if(
        tasks.begin(), tasks.end(),
        [](const CameraTaskDefinition& task) {
            return !task.deleted_at_ms.has_value() &&
                task.enabled && task.desired_state == "running";
        });
    if (desired_count > static_cast<std::size_t>(
            config.camera_tasks.max_active_runs)) {
        error = "CAMERA_RECOVERY_CAPACITY_EXCEEDED";
        return false;
    }

    for (const auto& task : tasks) {
        std::vector<CameraTaskRunRecord> runs;
        if (!repository->listRuns(task.task_id, 1, 0, runs, error)) {
            return false;
        }
        if (!runs.empty() && !isCameraRunTerminal(runs.front().status)) {
            const auto active = runs.front();
            if (!acquireRecoveryOwnership(
                    config.camera_tasks, runtime_control, active, error)) {
                return false;
            }
            CameraTaskRunRecord failed = active;
            failed.status = "failed";
            failed.stop_time_ms = wallNowMs();
            failed.last_update_ms = failed.stop_time_ms;
            failed.stop_reason = "worker_restart_recovery";
            failed.error_code = "WORKER_RESTARTED";
            failed.error_message =
                "previous worker generation ended before the run completed";
            std::string transition_code;
            std::string transition_error;
            const bool transitioned = repository->transitionRun(
                active.run_id,
                { "queued", "starting", "running", "reconnecting", "stopping" },
                failed,
                transition_code,
                transition_error);
            std::string ignored;
            runtime_control->releaseRunLease(
                active.task_id, active.run_id, ignored);
            if (!transitioned) {
                error = transition_code.empty()
                    ? transition_error : transition_code;
                return false;
            }
            ++recovered_runs;
        }

        if (task.deleted_at_ms.has_value() || !task.enabled ||
            task.desired_state != "running") {
            continue;
        }
        const auto profile = config.camera_profiles.find(task.camera_profile);
        if (profile == config.camera_profiles.end() ||
            !profile->second.enabled) {
            error = "CAMERA_RECOVERY_PROFILE_UNAVAILABLE: " +
                task.camera_profile;
            return false;
        }

        CameraTaskRunRecord run;
        run.run_id = makeRecoveryRunId(wallNowMs());
        run.task_id = task.task_id;
        run.definition_version = task.version;
        run.definition_json = recoveryDefinitionJson(task);
        run.status = "queued";
        run.camera_profile = task.camera_profile;
        run.create_time_ms = wallNowMs();
        run.last_update_ms = run.create_time_ms;
        std::string code;
        if (!repository->createRun(run, code, error)) {
            if (!code.empty()) error = code + ": " + error;
            return false;
        }
        commands.push_back(recoveryCommand(task, run));
    }
    return true;
}

class SharedCameraCommandSource final : public ICameraTaskCommandSource {
public:
    explicit SharedCameraCommandSource(std::shared_ptr<CameraTaskQueue> queue)
        : queue_(std::move(queue)) {}
    bool start(std::string& error) override { return queue_->start(error); }
    bool poll(CameraTaskCommand& command, std::string& error) override {
        return queue_->poll(command, error);
    }
    bool acknowledge(const std::string& message_id, std::string& error) override {
        return queue_->acknowledge(message_id, error);
    }
    void interrupt() noexcept override { queue_->interrupt(); }
private:
    std::shared_ptr<CameraTaskQueue> queue_;
};

struct CameraTaskRuntimeResources {
    std::shared_ptr<CameraTaskRepository> repository;
    // XREADGROUP is intentionally isolated from short runtime-control calls.
    // Sharing one CameraTaskQueue would let its blocking poll hold the same
    // mutex/Redis context needed by lease refresh and hot-status publication.
    std::shared_ptr<CameraTaskQueue> command_queue;
    std::shared_ptr<CameraTaskQueue> runtime_control;
    std::shared_ptr<FrameArtifactWriter> writer;
    std::shared_ptr<CameraFrameRetentionSweeper> retention;

    ~CameraTaskRuntimeResources() noexcept {
        if (retention) retention->stop();
        if (writer) writer->stop();
    }
};

}  // namespace

std::unique_ptr<CameraTaskManager> createProductionCameraTaskManager(
    const AppConfig& config,
    const std::string& consumer_name,
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
    std::shared_ptr<ICameraFrameJobSink> inference_sink,
    std::string& error
) {
    error.clear();
    if (!hub_registry) {
        error = "camera Hub registry is unavailable";
        return nullptr;
    }
    auto resources = std::make_shared<CameraTaskRuntimeResources>();
    resources->repository = std::make_shared<CameraTaskRepository>(config.camera_tasks);
    if (!resources->repository->initialize(error)) return nullptr;

    const std::string camera_consumer = consumer_name + "_camera";
    resources->command_queue = std::make_shared<CameraTaskQueue>(
        config.redis, config.camera_tasks, camera_consumer);
    resources->runtime_control = std::make_shared<CameraTaskQueue>(
        config.redis, config.camera_tasks, camera_consumer + "_runtime");
    std::vector<CameraTaskCommand> recovery_commands;
    int recovered_runs = 0;
    if (!prepareRecoveryCommands(
            config, resources->repository, resources->runtime_control,
            recovery_commands, recovered_runs, error)) {
        return nullptr;
    }
    if (recovered_runs > 0 || !recovery_commands.empty()) {
        spdlog::warn(
            "Camera restart recovery closed {} prior run(s) and queued {} desired Camera(s)",
            recovered_runs, recovery_commands.size());
    }
    resources->writer = std::make_shared<FrameArtifactWriter>(
        config.camera_tasks, resources->repository);
    if (!resources->writer->start(error)) return nullptr;
    resources->retention = std::make_shared<CameraFrameRetentionSweeper>(
        config.camera_tasks, resources->repository);
    if (!resources->retention->start(error)) return nullptr;

    auto source = std::make_unique<SharedCameraCommandSource>(resources->command_queue);
    auto factory = [resources, hub_registry, inference_sink, config, camera_consumer](
        const CameraTaskCommand& command,
        std::string& factory_error) -> std::shared_ptr<ICameraTaskSession> {
        CameraTaskRunRecord run;
        bool found = false;
        if (!resources->repository->getRun(command.run_id, run, found, factory_error)) return nullptr;
        if (!found) {
            factory_error = "RUN_NOT_FOUND";
            return nullptr;
        }
        if (isCameraRunTerminal(run.status)) {
            factory_error = "RUN_TERMINAL";
            return nullptr;
        }
        if (run.task_id != command.task_id || run.camera_profile != command.camera_profile ||
            run.definition_version != command.definition_version) {
            factory_error = "RUN_DEFINITION_MISMATCH";
            return nullptr;
        }
        if (command.analysis_enabled && !inference_sink) {
            factory_error = "ANALYSIS_POOL_UNAVAILABLE";
            return nullptr;
        }
        if (!resources->runtime_control->acquireRunLease(
                command.task_id, command.run_id, factory_error)) {
            return nullptr;
        }
        if (run.status == "queued") {
            CameraTaskRunRecord starting = run;
            starting.status = "starting";
            starting.start_time_ms = wallNowMs();
            starting.last_update_ms = starting.start_time_ms;
            starting.worker_consumer = camera_consumer;
            std::string code;
            if (!resources->repository->transitionRun(
                run.run_id, { "queued" }, starting, code, factory_error)) {
                std::string ignored;
                resources->runtime_control->releaseRunLease(
                    command.task_id, command.run_id, ignored);
                return nullptr;
            }
        }
        return std::make_shared<CameraPipeline>(
            command, config.camera_tasks, config.capture.stale_frame_timeout_ms,
            camera_consumer, hub_registry, resources->writer, resources->repository,
            resources->runtime_control, inference_sink);
    };

    auto failure = [resources](
        const CameraTaskCommand& command,
        const std::string& code,
        const std::string& message) {
        if (code == "CAMERA_RUN_CAPACITY_EXCEEDED") return;
        std::string ignored;
        resources->runtime_control->releaseRunLease(command.task_id, command.run_id, ignored);
        CameraTaskRunRecord run;
        bool found = false;
        if (!resources->repository->getRun(command.run_id, run, found, ignored) || !found ||
            isCameraRunTerminal(run.status)) return;
        CameraTaskRunRecord failed = run;
        failed.status = "failed";
        failed.stop_time_ms = wallNowMs();
        failed.last_update_ms = failed.stop_time_ms;
        failed.stop_reason = "session_create_failed";
        failed.error_code = code;
        failed.error_message = message;
        std::string transition_code;
        resources->repository->transitionRun(command.run_id,
            { "queued", "starting", "running", "reconnecting", "stopping" },
            failed, transition_code, ignored);
    };

    return std::make_unique<CameraTaskManager>(
        config.camera_tasks.max_active_runs,
        std::move(source),
        std::move(factory),
        std::move(failure),
        std::move(recovery_commands));
}

}  // namespace yolo11_server
