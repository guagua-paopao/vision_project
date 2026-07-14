#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "business/security_analytics_types.h"

namespace yolo11_server {

    class TrackAnalyticsEngine {
    public:
        explicit TrackAnalyticsEngine(TrackAnalyticsConfig config = {});

        std::vector<TrackAnalyticsSnapshot> update(
            const std::vector<PersonTrack>& tracks,
            long long timestamp_ms
        );

        std::vector<TrackAnalyticsSnapshot> snapshots(long long timestamp_ms) const;
        void reset();

    private:
        struct Sample {
            long long timestamp_ms = 0;
            PfPoint point;
        };

        struct TrackHistory {
            std::deque<Sample> samples;
            long long last_seen_ms = 0;
            long long stationary_since_ms = 0;
            int missed = 0;
        };

        TrackAnalyticsSnapshot calculate(std::int64_t track_id, const TrackHistory& history, long long now_ms) const;
        void pruneHistory(TrackHistory& history, long long now_ms) const;

        TrackAnalyticsConfig config_;
        std::map<std::int64_t, TrackHistory> histories_;
    };

}  // namespace yolo11_server
