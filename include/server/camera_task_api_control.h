#pragma once

#include <string>

#include "business/camera_hub_status.h"
#include "server/camera_task_manager.h"

namespace yolo11_server {

struct CameraAnalysisSnapshot {
    std::string analysis_config_version;
    double infer_fps = 0.0;
    double last_inference_ms = 0.0;
    long long analysis_frame_count = 0;
    long long initial_occupancy = 0;
    long long in_count = 0;
    long long out_count = 0;
    long long occupancy = 0;
    int live_persons = 0;
    int analysis_reconnect_count = 0;
    int warmup_frames_remaining = 0;
    std::string security_state_json = "{}";
    std::string analysis_snapshot_relative_path;
    bool analysis_storage_degraded = false;
    bool analysis_snapshot_degraded = false;
    long long analysis_last_update_ms = 0;
};

// Redis Run status is the merged capture/extraction snapshot plus the
// algorithm-owned CameraAnalysisSnapshot base subobject. The two writers
// update disjoint Redis hash fields.
struct CameraTaskRunHotStatus : CameraAnalysisSnapshot {
    bool found = false;
    std::string run_id;
    std::string task_id;
    std::string status;
    std::string camera_profile;
    std::string hub_instance_id;
    std::string hub_state;
    std::string capture_backend;
    std::string error_code;
    std::string error_message;
    long long last_update_ms = 0;
    long long latest_frame_age_ms = -1;
    bool pipeline_thread_running = false;
    long long pipeline_started_at_ms = 0;
    double sample_fps = 0.0;
    long long sampled_frames = 0;
    long long inference_submit_drops = 0;
    double save_fps = 0.0;
    long long consumed_frames = 0;
    long long saved_frames = 0;
    long long skipped_frames = 0;
    long long dropped_frames = 0;
    unsigned long long last_source_sequence = 0;
    long long last_frame_time_ms = 0;
    int writer_queue_depth = 0;
};

struct CameraHubHotStatus {
    bool found = false;
    CameraHubStatus snapshot;
    long long last_update_ms = 0;
};

class ICameraTaskApiControl {
public:
    virtual ~ICameraTaskApiControl() = default;
    virtual bool submitStart(const CameraTaskCommand& command, std::string& error) = 0;
    virtual bool requestStop(const std::string& run_id, std::string& error) = 0;
    virtual bool getRunStatus(
        const std::string& run_id,
        CameraTaskRunHotStatus& status,
        std::string& error) = 0;
    virtual bool getHubStatus(
        const std::string& camera_profile,
        CameraHubHotStatus& status,
        std::string& error) = 0;
};

}  // namespace yolo11_server
