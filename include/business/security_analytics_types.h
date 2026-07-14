#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "business/people_flow_types.h"

namespace yolo11_server {

    enum class SecurityEventCategory {
        Zone,
        PoseAction,
        TemporalAction
    };

    inline const char* securityEventCategoryName(SecurityEventCategory category) noexcept {
        switch (category) {
        case SecurityEventCategory::Zone: return "zone";
        case SecurityEventCategory::PoseAction: return "pose_action";
        case SecurityEventCategory::TemporalAction: return "temporal_action";
        }
        return "unknown";
    }

    struct SecurityEvent {
        std::string event_id;
        std::string session_id;
        std::string camera_id;
        std::int64_t track_id = 0;
        SecurityEventCategory category = SecurityEventCategory::Zone;
        std::string event_type;
        std::string zone_id;
        long long start_time_ms = 0;
        long long end_time_ms = 0;
        long long event_time_ms = 0;
        double confidence = 0.0;
        int severity = 1;
        std::string evidence_path;
        bool demo_classifier = false;
    };

    struct SecurityZoneConfig {
        std::string zone_id;
        std::string name;
        std::vector<PfPoint> polygon_norm;
        int enter_confirm_frames = 3;
        int exit_confirm_frames = 3;
        long long dwell_alarm_ms = 10000;
        long long cooldown_ms = 30000;
        int max_missed_frames = 3;
        int severity = 2;
        bool emit_enter = true;
        bool emit_exit = true;
        bool emit_dwell = true;
    };

    struct TrackZoneStatus {
        std::int64_t track_id = 0;
        std::string zone_id;
        bool inside = false;
        long long entered_at_ms = 0;
        long long dwell_ms = 0;
        bool dwell_alarm_emitted = false;
    };

    struct TrackAnalyticsConfig {
        long long history_window_ms = 60000;
        long long loiter_window_ms = 15000;
        double stationary_speed_px_s = 8.0;
        double loiter_min_path_px = 80.0;
        double loiter_max_displacement_px = 45.0;
        int max_missed_frames = 3;
    };

    struct TrackAnalyticsSnapshot {
        std::int64_t track_id = 0;
        PfPoint current_point;
        double instantaneous_speed_px_s = 0.0;
        double average_speed_px_s = 0.0;
        double cumulative_distance_px = 0.0;
        double displacement_px = 0.0;
        long long observed_ms = 0;
        long long stationary_ms = 0;
        bool stationary = false;
        bool loitering = false;
    };

    struct PoseKeypoint {
        double x = 0.0;
        double y = 0.0;
        double confidence = 0.0;
    };

    struct TrackedPose {
        std::int64_t track_id = 0;
        long long timestamp_ms = 0;
        PfRect bbox;
        std::vector<PoseKeypoint> keypoints;
    };

    struct PoseActionConfig {
        double min_keypoint_confidence = 0.35;
        int confirm_frames = 3;
        int release_frames = 3;
        long long cooldown_ms = 3000;
        double fall_trunk_angle_deg = 55.0;
        double fall_bbox_aspect_ratio = 1.05;
        double crouch_knee_angle_deg = 125.0;
        double running_speed_px_s = 180.0;
    };

    struct TemporalFeatureFrame {
        long long timestamp_ms = 0;
        std::vector<double> features;
    };

    struct TemporalActionPrediction {
        std::string label;
        double confidence = 0.0;
    };

    struct TemporalActionConfig {
        std::size_t window_size = 16;
        std::size_t min_samples = 8;
        int confirm_windows = 2;
        int release_windows = 2;
        long long cooldown_ms = 5000;
        double start_threshold = 0.70;
        double end_threshold = 0.40;
        int severity = 3;
    };

}  // namespace yolo11_server
