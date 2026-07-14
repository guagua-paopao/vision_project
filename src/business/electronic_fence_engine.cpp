#include "business/electronic_fence_engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace yolo11_server {

    ElectronicFenceEngine::ElectronicFenceEngine(
        std::vector<SecurityZoneConfig> zones,
        std::string session_id,
        std::string camera_id
    ) : zones_(std::move(zones)),
        session_id_(std::move(session_id)),
        camera_id_(std::move(camera_id)) {
        for (const SecurityZoneConfig& zone : zones_) {
            if (zone.zone_id.empty()) throw std::invalid_argument("security zone_id must not be empty");
            if (zone.polygon_norm.size() < 3) throw std::invalid_argument("security zone polygon requires at least 3 points");
            if (zone.enter_confirm_frames < 1 || zone.exit_confirm_frames < 1) {
                throw std::invalid_argument("security zone confirmation frames must be positive");
            }
        }
    }

    bool ElectronicFenceEngine::pointInPolygonNormalized(
        const PfPoint& image_point,
        int frame_width,
        int frame_height,
        const std::vector<PfPoint>& polygon_norm
    ) {
        if (frame_width <= 0 || frame_height <= 0 || polygon_norm.size() < 3) return false;
        const double x = image_point.x / static_cast<double>(frame_width);
        const double y = image_point.y / static_cast<double>(frame_height);
        bool inside = false;
        for (std::size_t i = 0, j = polygon_norm.size() - 1; i < polygon_norm.size(); j = i++) {
            const PfPoint& a = polygon_norm[i];
            const PfPoint& b = polygon_norm[j];
            const double cross = (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
            const double dot = (x - a.x) * (x - b.x) + (y - a.y) * (y - b.y);
            if (std::abs(cross) <= 1e-9 && dot <= 1e-9) return true;
            const bool intersects = ((a.y > y) != (b.y > y)) &&
                (x < (b.x - a.x) * (y - a.y) / ((b.y - a.y) + 1e-15) + a.x);
            if (intersects) inside = !inside;
        }
        return inside;
    }

    bool ElectronicFenceEngine::cooldownReady(
        const ZoneTrackState& state,
        const SecurityZoneConfig& zone,
        long long now_ms
    ) {
        return state.last_event_ms == 0 || now_ms - state.last_event_ms >= zone.cooldown_ms;
    }

    SecurityEvent ElectronicFenceEngine::makeEvent(
        const SecurityZoneConfig& zone,
        const PersonTrack& track,
        const std::string& event_type,
        long long event_time_ms,
        long long start_time_ms
    ) {
        SecurityEvent event;
        event.event_id = "zone_" + std::to_string(next_event_sequence_++);
        event.session_id = session_id_;
        event.camera_id = camera_id_;
        event.track_id = track.track_id;
        event.category = SecurityEventCategory::Zone;
        event.event_type = event_type;
        event.zone_id = zone.zone_id;
        event.start_time_ms = start_time_ms == 0 ? event_time_ms : start_time_ms;
        event.end_time_ms = event_time_ms;
        event.event_time_ms = event_time_ms;
        event.confidence = std::clamp(track.confidence, 0.0, 1.0);
        event.severity = zone.severity;
        return event;
    }

    std::vector<SecurityEvent> ElectronicFenceEngine::update(
        const std::vector<PersonTrack>& tracks,
        int frame_width,
        int frame_height,
        long long timestamp_ms
    ) {
        std::vector<SecurityEvent> events;
        std::map<StateKey, bool> seen;
        for (const PersonTrack& track : tracks) {
            if (!track.confirmed) continue;
            for (const SecurityZoneConfig& zone : zones_) {
                if (track.missed > zone.max_missed_frames) continue;
                const StateKey key{ zone.zone_id, track.track_id };
                seen[key] = true;
                ZoneTrackState& state = states_[key];
                state.last_seen_ms = timestamp_ms;
                const bool point_inside = pointInPolygonNormalized(
                    track.anchor_point, frame_width, frame_height, zone.polygon_norm);

                if (point_inside) {
                    state.exit_streak = 0;
                    if (!state.inside) {
                        ++state.enter_streak;
                        if (state.enter_streak >= zone.enter_confirm_frames) {
                            state.inside = true;
                            state.entered_at_ms = timestamp_ms;
                            state.enter_streak = 0;
                            state.dwell_emitted = false;
                            if (zone.emit_enter && cooldownReady(state, zone, timestamp_ms)) {
                                events.push_back(makeEvent(zone, track, "ZONE_ENTER", timestamp_ms));
                                state.last_event_ms = timestamp_ms;
                            }
                        }
                    } else if (zone.emit_dwell && !state.dwell_emitted &&
                               timestamp_ms - state.entered_at_ms >= zone.dwell_alarm_ms &&
                               cooldownReady(state, zone, timestamp_ms)) {
                        events.push_back(makeEvent(zone, track, "ZONE_DWELL", timestamp_ms, state.entered_at_ms));
                        state.dwell_emitted = true;
                        state.last_event_ms = timestamp_ms;
                    }
                } else {
                    state.enter_streak = 0;
                    if (state.inside) {
                        ++state.exit_streak;
                        if (state.exit_streak >= zone.exit_confirm_frames) {
                            const long long entered_at = state.entered_at_ms;
                            state.inside = false;
                            state.exit_streak = 0;
                            state.entered_at_ms = 0;
                            state.dwell_emitted = false;
                            if (zone.emit_exit && cooldownReady(state, zone, timestamp_ms)) {
                                events.push_back(makeEvent(zone, track, "ZONE_EXIT", timestamp_ms, entered_at));
                                state.last_event_ms = timestamp_ms;
                            }
                        }
                    }
                }
            }
        }

        constexpr long long stale_state_ms = 120000;
        for (auto it = states_.begin(); it != states_.end();) {
            if (!seen[it->first] && timestamp_ms - it->second.last_seen_ms > stale_state_ms) {
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
        return events;
    }

    std::vector<TrackZoneStatus> ElectronicFenceEngine::statuses(long long timestamp_ms) const {
        std::vector<TrackZoneStatus> result;
        for (const auto& [key, state] : states_) {
            TrackZoneStatus status;
            status.zone_id = key.first;
            status.track_id = key.second;
            status.inside = state.inside;
            status.entered_at_ms = state.entered_at_ms;
            status.dwell_ms = state.inside ? std::max(0LL, timestamp_ms - state.entered_at_ms) : 0;
            status.dwell_alarm_emitted = state.dwell_emitted;
            result.push_back(std::move(status));
        }
        return result;
    }

    void ElectronicFenceEngine::reset() {
        states_.clear();
    }

}  // namespace yolo11_server
