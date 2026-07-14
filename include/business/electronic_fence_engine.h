#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "business/security_analytics_types.h"

namespace yolo11_server {

    class ElectronicFenceEngine {
    public:
        ElectronicFenceEngine(
            std::vector<SecurityZoneConfig> zones,
            std::string session_id,
            std::string camera_id
        );

        std::vector<SecurityEvent> update(
            const std::vector<PersonTrack>& tracks,
            int frame_width,
            int frame_height,
            long long timestamp_ms
        );

        std::vector<TrackZoneStatus> statuses(long long timestamp_ms) const;
        void reset();

        static bool pointInPolygonNormalized(
            const PfPoint& image_point,
            int frame_width,
            int frame_height,
            const std::vector<PfPoint>& polygon_norm
        );

    private:
        struct ZoneTrackState {
            bool inside = false;
            int enter_streak = 0;
            int exit_streak = 0;
            long long entered_at_ms = 0;
            long long last_seen_ms = 0;
            long long last_event_ms = 0;
            bool dwell_emitted = false;
        };

        using StateKey = std::pair<std::string, std::int64_t>;

        SecurityEvent makeEvent(
            const SecurityZoneConfig& zone,
            const PersonTrack& track,
            const std::string& event_type,
            long long event_time_ms,
            long long start_time_ms = 0
        );
        static bool cooldownReady(const ZoneTrackState& state, const SecurityZoneConfig& zone, long long now_ms);

        std::vector<SecurityZoneConfig> zones_;
        std::string session_id_;
        std::string camera_id_;
        std::map<StateKey, ZoneTrackState> states_;
        std::uint64_t next_event_sequence_ = 1;
    };

}  // namespace yolo11_server
