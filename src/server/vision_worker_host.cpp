#include "server/vision_worker_host.h"

#include <iostream>
#include <utility>

#include <spdlog/spdlog.h>

#include "business/camera_task_repository.h"
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

    if (config_.camera_tasks.enabled && config_.analysis.enabled) {
        auto algorithm_repository =
            std::make_shared<CameraTaskRepository>(config_.camera_tasks);
        camera_algorithm_processor_ = std::make_shared<CameraAlgorithmProcessor>(
            config_, std::move(algorithm_repository));
        if (!camera_algorithm_processor_->start(error)) {
            camera_algorithm_processor_.reset();
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
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] fixed Camera inference pool started with "
                  << config_.analysis.inference_workers << " workers\n";
    }

    if (config_.camera_tasks.enabled) {
        if (!camera_manager_factory_) {
            error = "camera task runtime factory is unavailable";
            if (camera_inference_pool_) camera_inference_pool_->stop();
            camera_inference_pool_.reset();
            if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
            camera_algorithm_processor_.reset();
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
            people_flow_worker_->stop();
            people_flow_worker_.reset();
            hub_registry_->stopAll();
            hub_registry_.reset();
            return false;
        }
        std::cerr << "[BOOT] Camera Task role started\n";
    }

    running_.store(true);
    return true;
}

void VisionWorkerHost::stop() noexcept {
    if (!running_.exchange(false) && !people_flow_worker_ && !camera_task_manager_ &&
        !camera_inference_pool_ && !camera_algorithm_processor_ && !hub_registry_) {
        return;
    }
    try {
        if (camera_task_manager_) camera_task_manager_->stop();
        if (camera_inference_pool_) camera_inference_pool_->stop();
        if (camera_algorithm_processor_) camera_algorithm_processor_->stop();
        if (people_flow_worker_) people_flow_worker_->stop();
        camera_task_manager_.reset();
        camera_inference_pool_.reset();
        camera_algorithm_processor_.reset();
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

}  // namespace yolo11_server
