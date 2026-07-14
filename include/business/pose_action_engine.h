#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "business/security_analytics_types.h"

namespace yolo11_server {

    class PoseActionEngine {
    public:
        PoseActionEngine(
            PoseActionConfig config,
            std::string session_id,
            std::string camera_id
        );

        std::vector<SecurityEvent> update(
            const std::vector<TrackedPose>& poses,
            const std::vector<TrackAnalyticsSnapshot>& track_analytics,
            long long timestamp_ms
        );

        void reset();

    private:
        struct ActionState {
            bool active = false;
            int candidate_hits = 0;
            int release_hits = 0;
            long long start_time_ms = 0;
            long long last_end_ms = 0;
            long long last_seen_ms = 0;
            double last_confidence = 0.0;
        };

        struct Candidate {
            bool positive = false;
            double confidence = 0.0;
        };

        using StateKey = std::pair<std::int64_t, std::string>;

        std::map<std::string, Candidate> evaluate(
            const TrackedPose& pose,
            const TrackAnalyticsSnapshot* analytics
        ) const;
        SecurityEvent makeEvent(
            std::int64_t track_id,
            const std::string& event_type,
            long long event_time_ms,
            long long start_time_ms,
            double confidence
        );

        bool valid(const TrackedPose& pose, std::size_t index) const;
        static double angleAt(const PoseKeypoint& a, const PoseKeypoint& vertex, const PoseKeypoint& c);

        PoseActionConfig config_;
        std::string session_id_;
        std::string camera_id_;
        std::map<StateKey, ActionState> states_;
        std::uint64_t next_event_sequence_ = 1;
    };

}  // namespace yolo11_server
