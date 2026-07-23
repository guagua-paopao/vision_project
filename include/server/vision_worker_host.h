#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "server/app_config.h"
#include "server/camera_inference_pool.h"
#include "server/camera_task_manager.h"
#include "server/shared_camera_frame_hub.h"

namespace yolo11_server {

class PeopleFlowInferenceWorker;
class CameraAlgorithmProcessor;
class CameraInferencePool;
class ICameraFrameJobSink;

using CameraTaskManagerFactory = std::function<std::unique_ptr<CameraTaskManager>(
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
    std::shared_ptr<ICameraFrameJobSink> inference_sink)>;

// Process composition root for the option-3 architecture. It is the sole
// owner of the process-local Hub Registry and coordinates domain runtimes in a
// deterministic shutdown order.
class VisionWorkerHost final {
public:
    VisionWorkerHost(
        int worker_id,
        const AppConfig& config,
        std::string people_flow_consumer_name,
        CameraTaskManagerFactory camera_manager_factory = {}
    );
    ~VisionWorkerHost() noexcept;

    VisionWorkerHost(const VisionWorkerHost&) = delete;
    VisionWorkerHost& operator=(const VisionWorkerHost&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool running() const;
    std::vector<CameraHubStatus> hubSnapshots() const;
    std::vector<std::string> activeCameraRunIds() const;
    CameraInferencePoolSnapshot inferenceSnapshot() const;

private:
    int worker_id_ = 0;
    AppConfig config_;
    std::string people_flow_consumer_name_;
    CameraTaskManagerFactory camera_manager_factory_;
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry_;
    std::unique_ptr<PeopleFlowInferenceWorker> people_flow_worker_;
    std::shared_ptr<CameraAlgorithmProcessor> camera_algorithm_processor_;
    std::shared_ptr<CameraInferencePool> camera_inference_pool_;
    std::unique_ptr<CameraTaskManager> camera_task_manager_;
    std::atomic<bool> running_{ false };
};

}  // namespace yolo11_server
