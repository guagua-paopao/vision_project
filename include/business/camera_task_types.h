#pragma once

#include <optional>
#include <string>
#include <vector>

namespace yolo11_server {

struct CameraTaskDefinition {
    std::string task_id;
    std::string name;
    std::string camera_profile;
    bool enabled = true;
    int frame_interval_ms = 1000;
    std::string output_mode = "latest";
    int jpeg_quality = 90;
    int max_width = 0;
    int max_height = 0;
    int retention_days = 7;
    int max_saved_frames = 100000;
    std::string desired_state = "running";
    bool analysis_enabled = false;
    double target_infer_fps = 5.0;
    std::string algorithm_profile;
    std::vector<std::string> algorithms;
    std::string callback_profile;
    int version = 1;
    long long created_at_ms = 0;
    long long updated_at_ms = 0;
    std::optional<long long> deleted_at_ms;
};

struct CameraTaskPatch {
    std::optional<std::string> name;
    std::optional<std::string> camera_profile;
    std::optional<bool> enabled;
    std::optional<int> frame_interval_ms;
    std::optional<std::string> output_mode;
    std::optional<int> jpeg_quality;
    std::optional<int> max_width;
    std::optional<int> max_height;
    std::optional<int> retention_days;
    std::optional<int> max_saved_frames;
    std::optional<std::string> desired_state;
    std::optional<bool> analysis_enabled;
    std::optional<double> target_infer_fps;
    std::optional<std::string> algorithm_profile;
    std::optional<std::vector<std::string>> algorithms;
    std::optional<std::string> callback_profile;
};

struct CameraTaskRunRecord {
    std::string run_id;
    std::string task_id;
    int definition_version = 0;
    std::string definition_json;
    std::string status = "queued";
    std::string camera_profile;
    std::string hub_instance_id;
    long long create_time_ms = 0;
    long long start_time_ms = 0;
    long long stop_time_ms = 0;
    long long last_update_ms = 0;
    std::string worker_consumer;
    std::string capture_backend;
    double capture_fps = 0.0;
    double save_fps = 0.0;
    long long consumed_frames = 0;
    long long saved_frames = 0;
    long long skipped_frames = 0;
    long long dropped_frames = 0;
    unsigned long long last_source_sequence = 0;
    long long last_frame_time_ms = 0;
    int width = 0;
    int height = 0;
    std::string stop_reason;
    std::string error_code;
    std::string error_message;
};

struct CameraFrameArtifact {
    std::string frame_id;
    std::string task_id;
    std::string run_id;
    unsigned long long source_sequence = 0;
    long long capture_time_ms = 0;
    long long save_time_ms = 0;
    std::string relative_path;
    int width = 0;
    int height = 0;
    long long size_bytes = 0;
};

struct SecurityAlertEventRecord {
    std::string event_id;
    std::string task_id;
    std::string run_id;
    std::string camera_profile;
    std::string event_type;
    std::string category;
    int severity = 1;
    std::optional<double> confidence;
    std::optional<long long> track_id;
    long long occurred_at_ms = 0;
    std::string algorithm_profile;
    std::string model_name;
    std::string config_version;
    bool demo_classifier = false;
    std::string payload_json = "{}";
    std::string evidence_frame_id;
    std::string fingerprint;
    std::string delivery_status = "not_scheduled";
    long long created_at_ms = 0;
};

struct CameraIdempotencyRecord {
    std::string operation_scope;
    std::string idempotency_key;
    std::string request_digest;
    std::string resource_id;
    int response_status = 0;
    std::string response_json;
    long long created_at_ms = 0;
    long long expires_at_ms = 0;
};

inline bool isCameraRunTerminal(const std::string& status) {
    return status == "stopped" || status == "failed";
}

inline bool isCameraRunActive(const std::string& status) {
    return status == "queued" || status == "starting" || status == "running" ||
        status == "reconnecting" || status == "stopping";
}

}  // namespace yolo11_server
