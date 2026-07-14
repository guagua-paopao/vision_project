#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace yolo11_server {

    struct PfPoint {
        double x = 0.0;
        double y = 0.0;
    };

    struct PfRect {
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;

        double width() const { return std::max(0.0, x2 - x1); }
        double height() const { return std::max(0.0, y2 - y1); }
        double area() const { return width() * height(); }
        PfPoint center() const { return { (x1 + x2) * 0.5, (y1 + y2) * 0.5 }; }
        PfPoint bottomCenter() const { return { (x1 + x2) * 0.5, y2 }; }
    };

    inline double pfIntersectionOverUnion(const PfRect& a, const PfRect& b) {
        const double left = std::max(a.x1, b.x1);
        const double top = std::max(a.y1, b.y1);
        const double right = std::min(a.x2, b.x2);
        const double bottom = std::min(a.y2, b.y2);
        const double intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
        const double union_area = a.area() + b.area() - intersection;
        return union_area > 0.0 ? intersection / union_area : 0.0;
    }

    inline double pfDistance(const PfPoint& a, const PfPoint& b) {
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    struct PersonDetection {
        PfRect bbox;
        double confidence = 0.0;
        PfPoint anchor_point;
        long long timestamp_ms = 0;
        bool high_confidence = false;
    };

    struct PersonTrack {
        std::int64_t track_id = 0;
        PfRect bbox;
        PfPoint anchor_point;
        double velocity_x = 0.0;
        double velocity_y = 0.0;
        // Time-aware motion state used only when tracker.use_alpha_beta_filter
        // is enabled. Velocity is expressed in pixels per second.
        PfPoint filtered_center;
        double motion_velocity_x_px_s = 0.0;
        double motion_velocity_y_px_s = 0.0;
        bool motion_initialized = false;
        int age = 0;
        int hits = 0;
        int missed = 0;
        bool confirmed = false;
        // Diagnostic-only lifecycle flag. It is reset at the beginning of
        // every tracker update and set only for a detection match/new track.
        bool matched_this_frame = false;
        double confidence = 0.0;
        long long last_timestamp_ms = 0;
        std::deque<PfPoint> trail;
    };

    struct CrossingEvent {
        std::string event_id;
        std::string session_id;
        std::string camera_id;
        std::string line_id;
        std::int64_t track_id = 0;
        std::string direction;
        long long event_time_ms = 0;
        double confidence = 0.0;
        double point_x_norm = 0.0;
        double point_y_norm = 0.0;
        std::string config_version;
        std::string evidence_path;
    };

    struct PeopleFlowCounts {
        long long in_count = 0;
        long long out_count = 0;
        long long occupancy = 0;
        int live_persons = 0;
    };

}  // namespace yolo11_server
