#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace yolo11_server {

// Control-plane snapshot kept independent from OpenCV frame payload types.
struct CameraHubStatus {
    std::string hub_instance_id;
    std::string camera_profile;
    std::string state = "stopped";
    std::string backend_name;
    std::string last_error;
    int subscriber_count = 0;
    std::map<std::string, int> subscriber_types;
    long long open_count = 0;
    int reconnect_count = 0;
    double capture_fps = 0.0;
    double source_fps = 0.0;
    long long latest_frame_age_ms = -1;
    long long last_frame_time_ms = 0;
    std::uint64_t latest_sequence = 0;
    int width = 0;
    int height = 0;
    bool resolution_changed = false;
    long long resolution_change_count = 0;
};

}  // namespace yolo11_server
