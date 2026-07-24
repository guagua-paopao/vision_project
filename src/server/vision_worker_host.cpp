#include "server/vision_worker_host.h"

#include <chrono>
#include <iostream>
#include <utility>

#include <spdlog/spdlog.h>

#include "business/camera_task_repository.h"
#include "server/callback_delivery_worker.h"
#include "server/camera_algorithm_processor.h"
#include "server/camera_inference_pool.h"
#include "server/model_runner.h"
#include "server/people_flow_inference_worker.h"
#include "server/rtsp_camera_frame_source.h"

namespace yolo11_server {

VisionWorkerHost::VisionWorkerHost(
    int worker_id,
    const AppConfig& config,
    std::string people_flow_consumer_name,
    CameraTaskManagerFactory camera_manager_factory
) : worker_id_(worker_id),
    config_(config),
    people_flow_consumer_name_(std::move(people_flow_consumer_name)),
    camera_manager_factory_(std::move(camera_manager_factory)) {
}

VisionWorkerHost::~VisionWorkerHost() noexcept {
    stop();
}

bool VisionWorkerHost::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (config_.worker.worker_num != 1) {
        error = "VISION_WORKER_REQUIRES_WORKER_NUM_ONE";
        return false;
    }
    if (!config_.camera_hub.enabled) {
        error = "VISION_WORKER_REQUIRES_CAMERA_HUB";
        return false;
    }
    if (!config_.camera_tasks.config_error.empty()) {
        error = config_.camera_tasks.config_error;
        return false;
    }
    if (config_.callbacks.enabled &&
        !config_.callbacks.config_error.empty()) {
        error = config_.callbacks.config_error;
        return false;
    }

    hub_registry_ = createSharedCameraFrameHubRegistry(config_);
    if (!hub_registry_) {
        error = "failed to create shared camera Hub registry";
        return false;
    }
    std::cerr << "[BOOT] shared Camera FrameHub registry created\n";
    people_flow_worker_ = std::make_unique<PeopleFlowInferenceWorker>(
        worker_id_, config_, people_flow_consumer_name_, hub_registry_);
    std::cerr << "[BOOT] People Flow role constructed\n";
    if (!people_flow_worker_->start()) {
        error = "failed to start People Flow role";
        people_flow_worker_.reset();
        hub_registry_->stopAll();
        hub_registry_.reset();
        return false;
    }
    std::cerr << "[BOOT] People Flow role started\n";

    if (config_.camera_tasks.enabled &&
        (config_.analysis.enabled || config_.callbacks.enabled)) {
        camera_repository_ =
            std::make_shared<CameraTaskRepository>(config_.camera_tasks);
    }

    if (config_.camera_tasks.enabled && config_.analysis.enabled) {
        camera_algorithm_processor_ = std::make_shared<CameraAlgorithmProcessor>(
            config_, camera_repository_);
        if (!camera_algorithm_processor_->start(error)) {
            camera_algorithm_processor_.reset();
            camera_repository_.reset();
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        camera_inference_pool_ = std::make_shared<CameraInferencePool>(
            config_,
            [model_type = config_.model.type](int) {
                return createModelRunner(model_type);
            },
            camera_algorithm_processor_);
        if (!camera_inference_pool_->start(error)) {
            camera_inference_pool_.reset();
            camera_algorithm_processor_->stop();
            camera_algorithm_processor_.reset();
            camera_repository_.reset();
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] fixed Camera inference pool started with "
                  << config_.analysis.inference_workers << " workers\n";
    }

    if (config_.camera_tasks.enabled && config_.callbacks.enabled) {
        callback_delivery_worker_ = std::make_unique<CallbackDeliveryWorker>(
            config_, camera_repository_);
        if (!callback_delivery_worker_->start(error)) {
            callback_delivery_worker_.reset();
            if (camera_inference_pool_) camera_inference_pool_->stop();
            camera_inference_pool_.reset();
            if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
            camera_algorithm_processor_.reset();
            camera_repository_.reset();
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] durable callback delivery worker started\n";
    }

    if (config_.camera_tasks.enabled) {
        if (!camera_manager_factory_) {
            error = "camera task runtime factory is unavailable";
            if (camera_inference_pool_) camera_inference_pool_->stop();
            camera_inference_pool_.reset();
            if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
            camera_algorithm_processor_.reset();
            if (callback_delivery_worker_) callback_delivery_worker_->stop();
            callback_delivery_worker_.reset();
            camera_repository_.reset();
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] creating Camera Task runtime\n";
        camera_task_manager_ = camera_manager_factory_(
            hub_registry_, camera_inference_pool_);
        if (!camera_task_manager_ || !camera_task_manager_->start(error)) {
            if (error.empty()) error = "failed to start Camera Task role";
            camera_task_manager_.reset();
            if (camera_inference_pool_) camera_inference_pool_->stop();
            camera_inference_pool_.reset();
            if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
            camera_algorithm_processor_.reset();
            if (callback_delivery_worker_) callback_delivery_worker_->stop();
            callback_delivery_worker_.reset();
            camera_repository_.reset();
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] Camera Task role started\n";
    }

    running_.store(true);
    people_flow_worker_->setAlgorithmRuntimeProvider(
        [this]() { return algorithmRuntimeSnapshot(); });
    return true;
}

void VisionWorkerHost::stop() noexcept {
    if (!running_.exchange(false) && !people_flow_worker_ && !camera_task_manager_ &&
        !camera_inference_pool_ && !camera_algorithm_processor_ &&
        !callback_delivery_worker_ && !hub_registry_) {
        return;
    }
    try {
        if (people_flow_worker_) {
            people_flow_worker_->setAlgorithmRuntimeProvider({});
        }
        if (camera_task_manager_) camera_task_manager_->stop();
        if (camera_inference_pool_) camera_inference_pool_->stop();
        if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
        if (callback_delivery_worker_) callback_delivery_worker_->stop();
        if (people_flow_worker_) people_flow_worker_->stop();
        camera_task_manager_.reset();
        camera_inference_pool_.reset();
        camera_algorithm_processor_.reset();
        callback_delivery_worker_.reset();
        camera_repository_.reset();
        people_flow_worker_.reset();
        if (hub_registry_) hub_registry_->stopAll();
        hub_registry_.reset();
    }
    catch (...) {
        spdlog::error("VisionWorkerHost stop exception ignored");
    }
}

bool VisionWorkerHost::running() const {
    return running_.load();
}

std::vector<CameraHubStatus> VisionWorkerHost::hubSnapshots() const {
    return hub_registry_ ? hub_registry_->snapshots() : std::vector<CameraHubStatus>{};
}

std::vector<std::string> VisionWorkerHost::activeCameraRunIds() const {
    return camera_task_manager_ ? camera_task_manager_->activeRunIds() : std::vector<std::string>{};
}

CameraInferencePoolSnapshot VisionWorkerHost::inferenceSnapshot() const {
    return camera_inference_pool_
        ? camera_inference_pool_->snapshot()
        : CameraInferencePoolSnapshot{};
}

CallbackDeliverySnapshot VisionWorkerHost::callbackSnapshot() const {
    return callback_delivery_worker_
        ? callback_delivery_worker_->snapshot()
        : CallbackDeliverySnapshot{};
}

AlgorithmRuntimeSnapshot VisionWorkerHost::algorithmRuntimeSnapshot() const {
    AlgorithmRuntimeSnapshot result;
    result.generated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.host_running = running_.load();
    result.active_pipelines = camera_task_manager_
        ? static_cast<long long>(camera_task_manager_->activePipelineCount()) : 0;

    result.inference_configured =
        config_.camera_tasks.enabled && config_.analysis.enabled;
    const auto inference = inferenceSnapshot();
    result.inference_running = inference.running;
    result.inference_workers_configured = inference.workers_configured;
    result.inference_workers_ready = inference.workers_ready;
    result.inference_active_cameras =
        static_cast<long long>(inference.active_cameras);
    result.inference_pending_cameras =
        static_cast<long long>(inference.pending_cameras);
    result.inference_submitted_jobs = inference.submitted_jobs;
    result.inference_replaced_jobs = inference.replaced_jobs;
    result.inference_processed_jobs = inference.processed_jobs;
    result.inference_failed_jobs = inference.failed_jobs;
    result.inference_stale_results = inference.stale_results;

    result.processor_running = camera_algorithm_processor_ != nullptr;
    if (camera_algorithm_processor_) {
        const auto processor = camera_algorithm_processor_->snapshot();
        result.processor_active_sessions =
            static_cast<long long>(processor.active_sessions);
        result.processor_processed_frames = processor.processed_frames;
        result.processor_persisted_alerts = processor.persisted_alerts;
        result.processor_duplicate_alerts = processor.duplicate_alerts;
        result.processor_failed_frames = processor.failed_frames;
    }

    result.callbacks_configured =
        config_.camera_tasks.enabled && config_.callbacks.enabled;
    const auto callback = callbackSnapshot();
    result.callback_running = callback.running;
    result.callback_profiles_ready = callback.profiles_ready;
    result.callback_claimed = callback.claimed;
    result.callback_delivered = callback.delivered;
    result.callback_retries = callback.retries;
    result.callback_dead_letters = callback.dead_letters;
    result.callback_transport_failures = callback.transport_failures;
    result.callback_lease_conflicts = callback.lease_conflicts;
    result.callback_last_success_at_ms = callback.last_success_at_ms;
    result.callback_last_error_code = callback.last_error_code;
    return result;
}

}  // namespace yolo11_server
