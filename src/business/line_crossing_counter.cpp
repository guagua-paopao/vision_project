#include "business/line_crossing_counter.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace yolo11_server {

    LineCrossingCounter::LineCrossingCounter(
        const PeopleFlowCountingSection& config,
        std::string session_id,
        std::string camera_id,
        std::string config_version,
        long long initial_occupancy
    ) : config_(config),
        session_id_(std::move(session_id)),
        camera_id_(std::move(camera_id)),
        config_version_(std::move(config_version)) {
        counts_.occupancy = std::max(0LL, initial_occupancy);
    }

    double LineCrossingCounter::signedDistance(
        const PfPoint& point,
        int frame_width,
        int frame_height
    ) const {
        if (frame_width <= 0 || frame_height <= 0) return 0;
        const PfPoint a{ config_.line_a_norm.x * frame_width, config_.line_a_norm.y * frame_height };
        const PfPoint b{ config_.line_b_norm.x * frame_width, config_.line_b_norm.y * frame_height };
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double line_length = std::sqrt(dx * dx + dy * dy);
        if (line_length <= 1e-9) return 0.0;
        return (dx * (point.y - a.y) - dy * (point.x - a.x)) / line_length;
    }

    int LineCrossingCounter::classifySide(const PfPoint& point, int frame_width, int frame_height) const {
        const double signed_distance = signedDistance(point, frame_width, frame_height);
        if (signed_distance > config_.hysteresis_px) return 1;
        if (signed_distance < -config_.hysteresis_px) return -1;
        return 0;
    }

    bool LineCrossingCounter::crossingTouchesFiniteLine(
        const PfPoint& from,
        const PfPoint& to,
        int frame_width,
        int frame_height,
        PfPoint& intersection
    ) const {
        const PfPoint a{ config_.line_a_norm.x * frame_width, config_.line_a_norm.y * frame_height };
        const PfPoint b{ config_.line_b_norm.x * frame_width, config_.line_b_norm.y * frame_height };
        const double line_dx = b.x - a.x;
        const double line_dy = b.y - a.y;
        const double len2 = line_dx * line_dx + line_dy * line_dy;
        if (len2 <= 1e-9) return false;

        const double s_from = line_dx * (from.y - a.y) - line_dy * (from.x - a.x);
        const double s_to = line_dx * (to.y - a.y) - line_dy * (to.x - a.x);
        const double denominator = s_from - s_to;
        if (std::abs(denominator) <= 1e-9) return false;
        const double t = std::clamp(s_from / denominator, 0.0, 1.0);
        intersection.x = from.x + (to.x - from.x) * t;
        intersection.y = from.y + (to.y - from.y) * t;
        const double projection = ((intersection.x - a.x) * line_dx + (intersection.y - a.y) * line_dy) / len2;
        return projection >= -config_.finite_segment_extension_norm &&
            projection <= 1.0 + config_.finite_segment_extension_norm;
    }

    std::string LineCrossingCounter::directionForTransition(int from_side, int to_side) const {
        const bool positive_to_negative = from_side > 0 && to_side < 0;
        if (positive_to_negative) return config_.transition_positive_to_negative;
        return config_.transition_positive_to_negative == "IN" ? "OUT" : "IN";
    }

    CrossingEvent LineCrossingCounter::makeEvent(
        const PersonTrack& track,
        const std::string& direction,
        const PfPoint& point,
        int frame_width,
        int frame_height,
        long long timestamp_ms
    ) {
        std::ostringstream id;
        id << session_id_ << '_' << config_.line_id << '_' << std::setw(8) << std::setfill('0')
           << next_event_sequence_++;
        CrossingEvent event;
        event.event_id = id.str();
        event.session_id = session_id_;
        event.camera_id = camera_id_;
        event.line_id = config_.line_id;
        event.track_id = track.track_id;
        event.direction = direction;
        event.event_time_ms = timestamp_ms;
        event.confidence = track.confidence;
        event.point_x_norm = frame_width > 0 ? std::clamp(point.x / frame_width, 0.0, 1.0) : 0.0;
        event.point_y_norm = frame_height > 0 ? std::clamp(point.y / frame_height, 0.0, 1.0) : 0.0;
        event.config_version = config_version_;
        return event;
    }

    std::vector<CrossingEvent> LineCrossingCounter::update(
        const std::vector<PersonTrack>& tracks,
        int frame_width,
        int frame_height,
        long long timestamp_ms
    ) {
        std::vector<CrossingEvent> events;
        std::set<std::int64_t> live_ids;
        counts_.live_persons = 0;

        for (const PersonTrack& track : tracks) {
            live_ids.insert(track.track_id);
            if (!track.confirmed || track.missed > 0) continue;
            ++counts_.live_persons;
            TrackCounterState& state = states_[track.track_id];
            state.last_seen_ms = timestamp_ms;
            const double current_distance = signedDistance(track.anchor_point, frame_width, frame_height);
            const int side = classifySide(track.anchor_point, frame_width, frame_height);
            if (!state.has_previous_point) {
                state.previous_point = track.anchor_point;
                state.previous_timestamp_ms = timestamp_ms;
                state.has_previous_point = true;
                if (side != 0) state.stable_side = side;
                continue;
            }
            if (side != 0 && state.stable_side == 0) state.stable_side = side;
            bool counted_this_frame = false;
            if (side != 0 && state.stable_side != 0 && side != state.stable_side &&
                !state.counted && track.hits >= config_.min_hits_for_count &&
                timestamp_ms > state.previous_timestamp_ms &&
                timestamp_ms - state.previous_timestamp_ms <= config_.max_crossing_gap_ms) {
                PfPoint intersection;
                if (crossingTouchesFiniteLine(state.previous_point, track.anchor_point,
                        frame_width, frame_height, intersection)) {
                    const std::string direction = directionForTransition(state.stable_side, side);
                    long long event_time_ms = timestamp_ms;
                    const double previous_distance = signedDistance(
                        state.previous_point, frame_width, frame_height);
                    const double distance_sum = std::abs(previous_distance) + std::abs(current_distance);
                    if (distance_sum > 1e-9) {
                        const double ratio = std::clamp(std::abs(previous_distance) / distance_sum, 0.0, 1.0);
                        event_time_ms = state.previous_timestamp_ms + static_cast<long long>(std::llround(
                            ratio * (timestamp_ms - state.previous_timestamp_ms)));
                    }
                    events.push_back(makeEvent(track, direction, intersection,
                        frame_width, frame_height, event_time_ms));
                    state.counted = true;
                    state.rearm_side = side;
                    state.rearm_streak = 0;
                    state.last_event_ms = event_time_ms;
                    counted_this_frame = true;
                    if (direction == "IN") {
                        ++counts_.in_count;
                        ++counts_.occupancy;
                    }
                    else {
                        ++counts_.out_count;
                        counts_.occupancy = std::max(0LL, counts_.occupancy - 1);
                    }
                }
                state.stable_side = side;
            }
            else if (side != 0) {
                state.stable_side = side;
            }
            if (state.counted && !counted_this_frame) {
                const bool interval_ready = timestamp_ms >= state.last_event_ms &&
                    timestamp_ms - state.last_event_ms >= config_.min_crossing_interval_ms;
                const bool far_on_counted_side = side == state.rearm_side &&
                    std::abs(current_distance) >= config_.rearm_distance_px;
                if (interval_ready && far_on_counted_side) {
                    ++state.rearm_streak;
                    if (state.rearm_streak >= config_.rearm_frames) {
                        state.counted = false;
                        state.rearm_streak = 0;
                    }
                }
                else {
                    state.rearm_streak = 0;
                }
            }
            state.previous_point = track.anchor_point;
            state.previous_timestamp_ms = timestamp_ms;
        }

        for (auto it = states_.begin(); it != states_.end();) {
            if (!live_ids.count(it->first) && timestamp_ms - it->second.last_seen_ms > 60000) {
                it = states_.erase(it);
            }
            else {
                ++it;
            }
        }
        return events;
    }

    PeopleFlowCounts LineCrossingCounter::counts() const {
        return counts_;
    }

    std::vector<TrackCounterDebugState> LineCrossingCounter::debugStates(
        const std::vector<PersonTrack>& tracks,
        int frame_width,
        int frame_height
    ) const {
        std::vector<TrackCounterDebugState> result;
        result.reserve(tracks.size());
        for (const PersonTrack& track : tracks) {
            TrackCounterDebugState debug;
            debug.track_id = track.track_id;
            debug.current_side = classifySide(track.anchor_point, frame_width, frame_height);
            debug.current_point = track.anchor_point;
            debug.min_hits_ready = track.hits >= config_.min_hits_for_count;
            const auto state_it = states_.find(track.track_id);
            if (state_it != states_.end()) {
                debug.stable_side = state_it->second.stable_side;
                debug.counted = state_it->second.counted;
                debug.has_previous_point = state_it->second.has_previous_point;
                debug.previous_point = state_it->second.previous_point;
            }
            result.push_back(debug);
        }
        return result;
    }

    void LineCrossingCounter::resetTrackState() {
        states_.clear();
        counts_.live_persons = 0;
    }

    void LineCrossingCounter::calibrateOccupancy(long long occupancy) {
        counts_.occupancy = std::max(0LL, occupancy);
    }

    void LineCrossingCounter::setSession(const std::string& session_id, const std::string& camera_id) {
        session_id_ = session_id;
        camera_id_ = camera_id;
        resetTrackState();
        next_event_sequence_ = 1;
    }

}  // namespace yolo11_server
