#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "business/camera_hub_status.h"

namespace yolo11_server {

struct FrameEnvelope {
    cv::Mat image;
    std::uint64_t sequence = 0;
    long long capture_time_ms = 0;
    std::chrono::steady_clock::time_point publish_time{};
    bool resolution_changed = false;
};

using SharedCameraFrame = std::shared_ptr<const FrameEnvelope>;

struct SubscriberDescriptor {
    std::string subscriber_id;
    std::string subscriber_type;
};

struct SubscriptionMetrics {
    std::string subscriber_id;
    std::string subscriber_type;
    std::uint64_t last_seen_sequence = 0;
    long long consumed_frames = 0;
    long long skipped_frames = 0;
    long long last_consume_time_ms = 0;
};

struct FrameReadResult {
    SharedCameraFrame frame;
    std::uint64_t skipped_since_last_read = 0;
};

}  // namespace yolo11_server
