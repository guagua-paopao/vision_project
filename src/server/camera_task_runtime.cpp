#include "server/camera_task_runtime.h"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

#include "business/camera_pipeline.h"
#include "business/camera_frame_retention.h"
#include "business/camera_task_repository.h"
#include "business/frame_artifact_writer.h"
#include "server/camera_task_queue.h"

namespace yolo11_server {

namespace {

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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
    int recovered = 0;
    const long long now = wallNowMs();
    if (!resources->repository->recoverStaleRuns(
        now - config.camera_tasks.stale_run_timeout_ms, now, recovered, error)) return nullptr;
    if (recovered > 0) {
        spdlog::warn("Recovered {} stale Camera Task run(s) during VisionWorkerHost startup", recovered);
    }

    const std::string camera_consumer = consumer_name + "_camera";
    resources->command_queue = std::make_shared<CameraTaskQueue>(
        config.redis, config.camera_tasks, camera_consumer);
    resources->runtime_control = std::make_shared<CameraTaskQueue>(
        config.redis, config.camera_tasks, camera_consumer + "_runtime");
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
        config.camera_tasks.max_active_runs, std::move(source), std::move(factory), std::move(failure));
}

}  // namespace yolo11_server
