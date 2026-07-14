#include "business/pose_action_engine.h"

#include <algorithm>
#include <cmath>

namespace yolo11_server {

    namespace {
        constexpr double kPi = 3.14159265358979323846;

        PoseKeypoint midpoint(const PoseKeypoint& a, const PoseKeypoint& b) {
            return { (a.x + b.x) * 0.5, (a.y + b.y) * 0.5,
                     std::min(a.confidence, b.confidence) };
        }
    }

    PoseActionEngine::PoseActionEngine(
        PoseActionConfig config,
        std::string session_id,
        std::string camera_id
    ) : config_(config),
        session_id_(std::move(session_id)),
        camera_id_(std::move(camera_id)) {
    }

    bool PoseActionEngine::valid(const TrackedPose& pose, std::size_t index) const {
        return index < pose.keypoints.size() &&
            pose.keypoints[index].confidence >= config_.min_keypoint_confidence;
    }

    double PoseActionEngine::angleAt(
        const PoseKeypoint& a,
        const PoseKeypoint& vertex,
        const PoseKeypoint& c
    ) {
        const double ax = a.x - vertex.x;
        const double ay = a.y - vertex.y;
        const double cx = c.x - vertex.x;
        const double cy = c.y - vertex.y;
        const double denominator = std::sqrt(ax * ax + ay * ay) * std::sqrt(cx * cx + cy * cy);
        if (denominator <= 1e-9) return 180.0;
        const double cosine = std::clamp((ax * cx + ay * cy) / denominator, -1.0, 1.0);
        return std::acos(cosine) * 180.0 / kPi;
    }

    std::map<std::string, PoseActionEngine::Candidate> PoseActionEngine::evaluate(
        const TrackedPose& pose,
        const TrackAnalyticsSnapshot* analytics
    ) const {
        std::map<std::string, Candidate> result{
            { "HANDS_UP", {} }, { "FALL", {} }, { "CROUCH", {} },
            { "RUNNING", {} }, { "LOITERING", {} }
        };

        // COCO keypoints: shoulders 5/6, wrists 9/10, hips 11/12,
        // knees 13/14, ankles 15/16.
        bool left_hand_up = valid(pose, 5) && valid(pose, 9) &&
            pose.keypoints[9].y < pose.keypoints[5].y;
        bool right_hand_up = valid(pose, 6) && valid(pose, 10) &&
            pose.keypoints[10].y < pose.keypoints[6].y;
        if (left_hand_up || right_hand_up) {
            const double confidence = std::max(
                left_hand_up ? std::min(pose.keypoints[5].confidence, pose.keypoints[9].confidence) : 0.0,
                right_hand_up ? std::min(pose.keypoints[6].confidence, pose.keypoints[10].confidence) : 0.0);
            result["HANDS_UP"] = { true, confidence };
        }

        if (valid(pose, 5) && valid(pose, 6) && valid(pose, 11) && valid(pose, 12)) {
            const PoseKeypoint shoulder = midpoint(pose.keypoints[5], pose.keypoints[6]);
            const PoseKeypoint hip = midpoint(pose.keypoints[11], pose.keypoints[12]);
            const double trunk_angle = std::atan2(std::abs(hip.x - shoulder.x),
                                                   std::abs(hip.y - shoulder.y) + 1e-9) * 180.0 / kPi;
            const double bbox_ratio = pose.bbox.height() > 0.0
                ? pose.bbox.width() / pose.bbox.height() : 0.0;
            if (trunk_angle >= config_.fall_trunk_angle_deg &&
                bbox_ratio >= config_.fall_bbox_aspect_ratio) {
                const double angle_score = std::min(1.0, trunk_angle / 90.0);
                const double ratio_score = std::min(1.0, bbox_ratio / 2.0);
                result["FALL"] = { true, 0.5 * angle_score + 0.5 * ratio_score };
            }
        }

        std::vector<double> knee_angles;
        if (valid(pose, 11) && valid(pose, 13) && valid(pose, 15)) {
            knee_angles.push_back(angleAt(pose.keypoints[11], pose.keypoints[13], pose.keypoints[15]));
        }
        if (valid(pose, 12) && valid(pose, 14) && valid(pose, 16)) {
            knee_angles.push_back(angleAt(pose.keypoints[12], pose.keypoints[14], pose.keypoints[16]));
        }
        if (!knee_angles.empty()) {
            double average = 0.0;
            for (double angle : knee_angles) average += angle;
            average /= static_cast<double>(knee_angles.size());
            if (average <= config_.crouch_knee_angle_deg) {
                result["CROUCH"] = { true, std::clamp(1.0 - average / 180.0, 0.0, 1.0) };
            }
        }

        if (analytics != nullptr) {
            if (analytics->instantaneous_speed_px_s >= config_.running_speed_px_s) {
                result["RUNNING"] = { true,
                    std::min(1.0, analytics->instantaneous_speed_px_s /
                                  std::max(1.0, config_.running_speed_px_s * 2.0)) };
            }
            if (analytics->loitering) result["LOITERING"] = { true, 0.85 };
        }
        return result;
    }

    SecurityEvent PoseActionEngine::makeEvent(
        std::int64_t track_id,
        const std::string& event_type,
        long long event_time_ms,
        long long start_time_ms,
        double confidence
    ) {
        SecurityEvent event;
        event.event_id = "pose_" + std::to_string(next_event_sequence_++);
        event.session_id = session_id_;
        event.camera_id = camera_id_;
        event.track_id = track_id;
        event.category = SecurityEventCategory::PoseAction;
        event.event_type = event_type;
        event.start_time_ms = start_time_ms;
        event.end_time_ms = event_time_ms;
        event.event_time_ms = event_time_ms;
        event.confidence = std::clamp(confidence, 0.0, 1.0);
        event.severity = event_type.rfind("FALL_", 0) == 0 ? 3 : 2;
        return event;
    }

    std::vector<SecurityEvent> PoseActionEngine::update(
        const std::vector<TrackedPose>& poses,
        const std::vector<TrackAnalyticsSnapshot>& track_analytics,
        long long timestamp_ms
    ) {
        std::map<std::int64_t, const TrackAnalyticsSnapshot*> analytics_by_track;
        for (const TrackAnalyticsSnapshot& analytics : track_analytics) {
            analytics_by_track[analytics.track_id] = &analytics;
        }

        std::vector<SecurityEvent> events;
        for (const TrackedPose& pose : poses) {
            const auto analytics_it = analytics_by_track.find(pose.track_id);
            const TrackAnalyticsSnapshot* analytics = analytics_it == analytics_by_track.end()
                ? nullptr : analytics_it->second;
            const auto candidates = evaluate(pose, analytics);
            for (const auto& [label, candidate] : candidates) {
                ActionState& state = states_[{ pose.track_id, label }];
                state.last_seen_ms = timestamp_ms;
                if (candidate.positive) {
                    state.release_hits = 0;
                    state.last_confidence = candidate.confidence;
                    if (!state.active &&
                        (state.last_end_ms == 0 || timestamp_ms - state.last_end_ms >= config_.cooldown_ms)) {
                        ++state.candidate_hits;
                        if (state.candidate_hits >= config_.confirm_frames) {
                            state.active = true;
                            state.start_time_ms = timestamp_ms;
                            state.candidate_hits = 0;
                            events.push_back(makeEvent(
                                pose.track_id, label + "_START", timestamp_ms,
                                timestamp_ms, candidate.confidence));
                        }
                    }
                } else {
                    state.candidate_hits = 0;
                    if (state.active) {
                        ++state.release_hits;
                        if (state.release_hits >= config_.release_frames) {
                            events.push_back(makeEvent(
                                pose.track_id, label + "_END", timestamp_ms,
                                state.start_time_ms, state.last_confidence));
                            state.active = false;
                            state.release_hits = 0;
                            state.last_end_ms = timestamp_ms;
                        }
                    }
                }
            }
        }

        constexpr long long stale_action_ms = 120000;
        for (auto it = states_.begin(); it != states_.end();) {
            if (timestamp_ms - it->second.last_seen_ms > stale_action_ms) it = states_.erase(it);
            else ++it;
        }
        return events;
    }

    void PoseActionEngine::reset() {
        states_.clear();
    }

}  // namespace yolo11_server
