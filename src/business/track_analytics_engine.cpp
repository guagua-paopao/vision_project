#include "business/track_analytics_engine.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace yolo11_server {

    TrackAnalyticsEngine::TrackAnalyticsEngine(TrackAnalyticsConfig config)
        : config_(config) {
    }

    void TrackAnalyticsEngine::pruneHistory(TrackHistory& history, long long now_ms) const {
        while (history.samples.size() > 1 &&
               now_ms - history.samples.front().timestamp_ms > config_.history_window_ms) {
            history.samples.pop_front();
        }
    }

    TrackAnalyticsSnapshot TrackAnalyticsEngine::calculate(
        std::int64_t track_id,
        const TrackHistory& history,
        long long now_ms
    ) const {
        TrackAnalyticsSnapshot snapshot;
        snapshot.track_id = track_id;
        if (history.samples.empty()) return snapshot;
        snapshot.current_point = history.samples.back().point;
        if (history.samples.size() == 1) return snapshot;

        double cumulative = 0.0;
        for (std::size_t index = 1; index < history.samples.size(); ++index) {
            cumulative += pfDistance(history.samples[index - 1].point, history.samples[index].point);
        }
        const Sample& first = history.samples.front();
        const Sample& last = history.samples.back();
        snapshot.observed_ms = std::max(0LL, last.timestamp_ms - first.timestamp_ms);
        snapshot.cumulative_distance_px = cumulative;
        snapshot.displacement_px = pfDistance(first.point, last.point);
        if (snapshot.observed_ms > 0) {
            snapshot.average_speed_px_s = cumulative * 1000.0 / snapshot.observed_ms;
        }

        const Sample& previous = history.samples[history.samples.size() - 2];
        const long long delta_ms = last.timestamp_ms - previous.timestamp_ms;
        if (delta_ms > 0) {
            snapshot.instantaneous_speed_px_s =
                pfDistance(previous.point, last.point) * 1000.0 / delta_ms;
        }
        snapshot.stationary = snapshot.instantaneous_speed_px_s <= config_.stationary_speed_px_s;
        snapshot.stationary_ms = snapshot.stationary && history.stationary_since_ms > 0
            ? std::max(0LL, now_ms - history.stationary_since_ms)
            : 0;
        snapshot.loitering = snapshot.observed_ms >= config_.loiter_window_ms &&
            snapshot.cumulative_distance_px >= config_.loiter_min_path_px &&
            snapshot.displacement_px <= config_.loiter_max_displacement_px;
        return snapshot;
    }

    std::vector<TrackAnalyticsSnapshot> TrackAnalyticsEngine::update(
        const std::vector<PersonTrack>& tracks,
        long long timestamp_ms
    ) {
        std::set<std::int64_t> seen;
        for (const PersonTrack& track : tracks) {
            if (!track.confirmed || track.missed > config_.max_missed_frames) continue;
            seen.insert(track.track_id);
            TrackHistory& history = histories_[track.track_id];
            history.last_seen_ms = timestamp_ms;
            history.missed = track.missed;

            if (history.samples.empty() || history.samples.back().timestamp_ms != timestamp_ms) {
                const double speed = history.samples.empty() ? 0.0 :
                    pfDistance(history.samples.back().point, track.anchor_point) * 1000.0 /
                    std::max(1LL, timestamp_ms - history.samples.back().timestamp_ms);
                if (speed <= config_.stationary_speed_px_s) {
                    if (history.stationary_since_ms == 0) history.stationary_since_ms = timestamp_ms;
                } else {
                    history.stationary_since_ms = 0;
                }
                history.samples.push_back({ timestamp_ms, track.anchor_point });
            }
            pruneHistory(history, timestamp_ms);
        }

        for (auto it = histories_.begin(); it != histories_.end();) {
            if (!seen.count(it->first) && timestamp_ms - it->second.last_seen_ms > config_.history_window_ms) {
                it = histories_.erase(it);
            } else {
                ++it;
            }
        }
        return snapshots(timestamp_ms);
    }

    std::vector<TrackAnalyticsSnapshot> TrackAnalyticsEngine::snapshots(long long timestamp_ms) const {
        std::vector<TrackAnalyticsSnapshot> result;
        result.reserve(histories_.size());
        for (const auto& [track_id, history] : histories_) {
            result.push_back(calculate(track_id, history, timestamp_ms));
        }
        return result;
    }

    void TrackAnalyticsEngine::reset() {
        histories_.clear();
    }

}  // namespace yolo11_server
