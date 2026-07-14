#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "business/people_flow_debug_types.h"
#include "business/people_flow_types.h"
#include "server/app_config.h"

namespace yolo11_server {

    class LineCrossingCounter {
    public:
        LineCrossingCounter(
            const PeopleFlowCountingSection& config,
            std::string session_id,
            std::string camera_id,
            std::string config_version,
            long long initial_occupancy
        );

        std::vector<CrossingEvent> update(
            const std::vector<PersonTrack>& tracks,
            int frame_width,
            int frame_height,
            long long timestamp_ms
        );

        PeopleFlowCounts counts() const;
        void resetTrackState();
        void calibrateOccupancy(long long occupancy);
        void setSession(const std::string& session_id, const std::string& camera_id);

        int classifySide(const PfPoint& point, int frame_width, int frame_height) const;

        // Read-only projection of private per-track counter state for overlays
        // and frame_debug.jsonl. Calling this method never advances counting.
        std::vector<TrackCounterDebugState> debugStates(
            const std::vector<PersonTrack>& tracks,
            int frame_width,
            int frame_height
        ) const;

    private:
        struct TrackCounterState {
            int stable_side = 0;
            PfPoint previous_point;
            bool has_previous_point = false;
            long long previous_timestamp_ms = 0;
            // counted means temporarily disarmed after an event. It is reset
            // after the track moves far enough onto the new side for the
            // configured number of frames, allowing a genuine later re-entry.
            bool counted = false;
            int rearm_side = 0;
            int rearm_streak = 0;
            long long last_event_ms = 0;
            long long last_seen_ms = 0;
        };

        double signedDistance(const PfPoint& point, int frame_width, int frame_height) const;
        bool crossingTouchesFiniteLine(
            const PfPoint& from,
            const PfPoint& to,
            int frame_width,
            int frame_height,
            PfPoint& intersection
        ) const;
        std::string directionForTransition(int from_side, int to_side) const;
        CrossingEvent makeEvent(const PersonTrack& track, const std::string& direction,
            const PfPoint& point, int frame_width, int frame_height, long long timestamp_ms);

    private:
        PeopleFlowCountingSection config_;
        std::string session_id_;
        std::string camera_id_;
        std::string config_version_;
        std::map<std::int64_t, TrackCounterState> states_;
        PeopleFlowCounts counts_;
        std::uint64_t next_event_sequence_ = 1;
    };

}  // namespace yolo11_server
