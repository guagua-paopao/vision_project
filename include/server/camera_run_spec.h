#pragma once

#include <string>
#include <vector>

#include "business/camera_task_types.h"
#include "server/camera_task_manager.h"

namespace yolo11_server {

inline constexpr const char* kCameraRunOriginCameraApi = "camera_api";
inline constexpr const char* kCameraRunOriginPeopleFlowCompat = "people_flow_compat";

struct CameraRunFrameOutputSpec {
    bool enabled = true;
    int interval_ms = 1000;
    std::string mode = "latest";
    int jpeg_quality = 90;
    int max_width = 0;
    int max_height = 0;
    int retention_days = 7;
    int max_saved_frames = 100000;
};

struct CameraRunAnalysisSpec {
    bool enabled = false;
    double target_infer_fps = 5.0;
    std::string algorithm_profile;
    std::vector<std::string> algorithms;
    std::string config_version;
    long long initial_occupancy = 0;
    int snapshot_fps = 0;
    std::string algorithm_parameters_json = "{}";
};

struct CameraRunCompatibilitySpec {
    std::string legacy_session_id;
    bool preserve_pf_projection = false;
    int legacy_response_version = 0;
};

struct CameraRunSpecOptions {
    std::string origin = kCameraRunOriginCameraApi;
    std::string analysis_config_version;
    long long initial_occupancy = 0;
    int snapshot_fps = 0;
    std::string algorithm_parameters_json = "{}";
    CameraRunCompatibilitySpec compatibility;
};

// Immutable snapshot of one Camera execution. CameraTaskDefinition may change
// after start; this value deliberately cannot be assigned or mutated.
class CameraRunSpec final {
public:
    CameraRunSpec(const CameraRunSpec&) = default;
    CameraRunSpec(CameraRunSpec&&) = default;
    CameraRunSpec& operator=(const CameraRunSpec&) = delete;
    CameraRunSpec& operator=(CameraRunSpec&&) = delete;

    const std::string& runId() const noexcept { return run_id_; }
    const std::string& taskId() const noexcept { return task_id_; }
    const std::string& origin() const noexcept { return origin_; }
    const std::string& cameraProfile() const noexcept { return camera_profile_; }
    int definitionVersion() const noexcept { return definition_version_; }
    const CameraRunFrameOutputSpec& frameOutput() const noexcept { return frame_output_; }
    const CameraRunAnalysisSpec& analysis() const noexcept { return analysis_; }
    const std::string& callbackProfile() const noexcept { return callback_profile_; }
    long long createTimeMs() const noexcept { return create_time_ms_; }
    const CameraRunCompatibilitySpec& compatibility() const noexcept {
        return compatibility_;
    }

    std::string toDefinitionJson() const;
    CameraTaskRunRecord toRunRecord() const;
    CameraTaskCommand toStartCommand() const;

private:
    friend CameraRunSpec makeCameraRunSpec(
        const CameraTaskDefinition&,
        std::string,
        long long,
        CameraRunSpecOptions);

    CameraRunSpec(
        std::string run_id,
        std::string task_id,
        std::string origin,
        std::string camera_profile,
        int definition_version,
        CameraRunFrameOutputSpec frame_output,
        CameraRunAnalysisSpec analysis,
        std::string callback_profile,
        long long create_time_ms,
        CameraRunCompatibilitySpec compatibility);

    const std::string run_id_;
    const std::string task_id_;
    const std::string origin_;
    const std::string camera_profile_;
    const int definition_version_;
    const CameraRunFrameOutputSpec frame_output_;
    const CameraRunAnalysisSpec analysis_;
    const std::string callback_profile_;
    const long long create_time_ms_;
    const CameraRunCompatibilitySpec compatibility_;
};

CameraRunSpec makeCameraRunSpec(
    const CameraTaskDefinition& definition,
    std::string run_id,
    long long create_time_ms,
    CameraRunSpecOptions options = {});

}  // namespace yolo11_server
