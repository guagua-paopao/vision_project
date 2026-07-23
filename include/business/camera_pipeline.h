#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "business/camera_task_repository.h"
#include "business/camera_task_runtime_control.h"
#include "business/frame_artifact_writer.h"
#include "server/camera_task_manager.h"
#include "server/shared_camera_frame_hub.h"

namespace yolo11_server {

struct CameraFrameJob {
    std::string task_id;
    std::string run_id;
    std::string camera_profile;
    unsigned long long source_sequence = 0;
    long long capture_time_ms = 0;
    std::string algorithm_profile;
    std::vector<std::string> algorithms;
    SharedCameraFrame frame;
};

struct CameraFrameJobSubmitResult {
    bool accepted = false;
    long long dropped_backlog = 0;
};

// P2 boundary between per-camera sampling and the fixed inference pool added
// in P3. submitLatest must be non-blocking and bounded.
class ICameraFrameJobSink {
public:
    virtual ~ICameraFrameJobSink() = default;
    virtual bool submitLatest(
        CameraFrameJob job,
        CameraFrameJobSubmitResult& result,
        std::string& error) = 0;
    virtual void detachCamera(
        const std::string& task_id,
        const std::string& run_id) noexcept = 0;
};

// One long-lived sampling/extraction owner per stable camera_id. In P3 this
// same thread also submits latest-only FrameJobs; model execution never runs
// on the pipeline thread.
class CameraPipeline final : public ICameraTaskSession {
public:
    CameraPipeline(
        CameraTaskCommand command,
        const CameraTasksSection& camera_config,
        int stale_frame_timeout_ms,
        std::string worker_consumer,
        std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
        std::shared_ptr<FrameArtifactWriter> writer,
        std::shared_ptr<CameraTaskRepository> repository,
        std::shared_ptr<ICameraTaskRuntimeControl> control,
        std::shared_ptr<ICameraFrameJobSink> inference_sink = {}
    );
    ~CameraPipeline() noexcept override;

    void run() noexcept override;
    void requestStop() noexcept override;

    // Shared with asynchronous writer callbacks; public for the translation
    // unit helper only, not part of the HTTP/domain contract.
    struct WriteState {
        std::atomic<long long> saved_frames{ 0 };
        std::atomic<bool> fatal{ false };
        std::atomic<int> last_width{ 0 };
        std::atomic<int> last_height{ 0 };
        std::atomic<long long> last_frame_time_ms{ 0 };
        std::mutex error_mutex;
        std::string error_code;
        std::string error_message;
    };

private:
    void runImpl();
    bool transition(
        CameraTaskRunRecord& run,
        const std::vector<std::string>& allowed_from,
        const std::string& next_status,
        const std::string& error_code = {},
        const std::string& error_message = {}
    );
    void publishProgress(
        CameraTaskRunRecord& run,
        const CameraHubStatus& hub,
        const SubscriptionMetrics& subscription,
        long long sampled_frames,
        long long dropped_frames,
        long long inference_submit_drops,
        const std::shared_ptr<WriteState>& writes,
        std::chrono::steady_clock::time_point started
    ) noexcept;

    CameraTaskCommand command_;
    CameraTasksSection camera_config_;
    int stale_frame_timeout_ms_ = 5000;
    std::string worker_consumer_;
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry_;
    std::shared_ptr<FrameArtifactWriter> writer_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::shared_ptr<ICameraTaskRuntimeControl> control_;
    std::shared_ptr<ICameraFrameJobSink> inference_sink_;
    std::atomic<bool> stop_requested_{ false };
};

}  // namespace yolo11_server
