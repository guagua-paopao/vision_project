#include "business/person_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace yolo11_server {

    namespace {

        struct MatchCandidate {
            double cost = 0.0;
            std::size_t track_index = 0;
            std::size_t detection_index = 0;
        };

    }  // namespace

    PersonTracker::PersonTracker(const PeopleFlowTrackerSection& config)
        : config_(config) {
    }

    PfRect PersonTracker::predict(const PersonTrack& track, long long timestamp_ms) const {
        PfRect predicted = track.bbox;
        if (config_.use_alpha_beta_filter && track.motion_initialized) {
            const long long elapsed_ms = std::clamp(
                timestamp_ms - track.last_timestamp_ms,
                0LL,
                static_cast<long long>(config_.max_prediction_ms));
            const double elapsed_s = static_cast<double>(elapsed_ms) / 1000.0;
            const PfPoint predicted_center{
                track.filtered_center.x + track.motion_velocity_x_px_s * elapsed_s,
                track.filtered_center.y + track.motion_velocity_y_px_s * elapsed_s
            };
            const PfPoint current_center = track.bbox.center();
            const double dx = predicted_center.x - current_center.x;
            const double dy = predicted_center.y - current_center.y;
            predicted.x1 += dx;
            predicted.x2 += dx;
            predicted.y1 += dy;
            predicted.y2 += dy;
            return predicted;
        }
        predicted.x1 += track.velocity_x;
        predicted.x2 += track.velocity_x;
        predicted.y1 += track.velocity_y;
        predicted.y2 += track.velocity_y;
        return predicted;
    }

    void PersonTracker::applyDetection(PersonTrack& track, const PersonDetection& detection, long long timestamp_ms) {
        const PfPoint previous_center = track.bbox.center();
        const PfPoint current_center = detection.bbox.center();
        const double observed_vx = current_center.x - previous_center.x;
        const double observed_vy = current_center.y - previous_center.y;
        const double alpha = config_.velocity_smoothing;
        track.velocity_x = alpha * observed_vx + (1.0 - alpha) * track.velocity_x;
        track.velocity_y = alpha * observed_vy + (1.0 - alpha) * track.velocity_y;
        if (config_.use_alpha_beta_filter) {
            if (!track.motion_initialized) {
                track.filtered_center = previous_center;
                track.motion_initialized = true;
            }
            const long long elapsed_ms = std::clamp(
                timestamp_ms - track.last_timestamp_ms,
                1LL,
                static_cast<long long>(config_.max_prediction_ms));
            const double elapsed_s = static_cast<double>(elapsed_ms) / 1000.0;
            const PfPoint predicted_center{
                track.filtered_center.x + track.motion_velocity_x_px_s * elapsed_s,
                track.filtered_center.y + track.motion_velocity_y_px_s * elapsed_s
            };
            const PfPoint residual{
                current_center.x - predicted_center.x,
                current_center.y - predicted_center.y
            };
            const PfPoint filtered_center{
                predicted_center.x + config_.motion_alpha * residual.x,
                predicted_center.y + config_.motion_alpha * residual.y
            };
            track.motion_velocity_x_px_s += config_.motion_beta * residual.x / elapsed_s;
            track.motion_velocity_y_px_s += config_.motion_beta * residual.y / elapsed_s;
            track.filtered_center = filtered_center;

            const double half_width = detection.bbox.width() * 0.5;
            const double half_height = detection.bbox.height() * 0.5;
            track.bbox = {
                filtered_center.x - half_width,
                filtered_center.y - half_height,
                filtered_center.x + half_width,
                filtered_center.y + half_height
            };
            track.anchor_point = {
                detection.anchor_point.x + filtered_center.x - current_center.x,
                detection.anchor_point.y + filtered_center.y - current_center.y
            };
        }
        else {
            track.bbox = detection.bbox;
            track.anchor_point = detection.anchor_point;
        }
        track.confidence = detection.confidence;
        track.last_timestamp_ms = timestamp_ms;
        track.missed = 0;
        track.matched_this_frame = true;
        ++track.hits;
        track.confirmed = track.confirmed || track.hits >= config_.min_hits;
        track.trail.push_back(track.anchor_point);
        while (track.trail.size() > static_cast<std::size_t>(config_.trail_length)) track.trail.pop_front();
    }

    const std::vector<PersonTrack>& PersonTracker::update(
        const std::vector<PersonDetection>& detections,
        int frame_width,
        int frame_height,
        long long timestamp_ms
    ) {
        for (PersonTrack& track : tracks_) {
            ++track.age;
            track.matched_this_frame = false;
        }

        const double diagonal = std::sqrt(
            static_cast<double>(frame_width) * frame_width + static_cast<double>(frame_height) * frame_height
        );
        const double center_gate = std::max(1.0, diagonal * config_.center_distance_gate_norm);

        std::vector<bool> track_matched(tracks_.size(), false);
        std::vector<bool> detection_matched(detections.size(), false);

        auto run_match_pass = [&](bool high_confidence_pass) {
            std::vector<MatchCandidate> candidates;
            for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
                if (track_matched[ti]) continue;
                const PfRect predicted = predict(tracks_[ti], timestamp_ms);
                for (std::size_t di = 0; di < detections.size(); ++di) {
                    if (detection_matched[di] || detections[di].high_confidence != high_confidence_pass) continue;
                    const double iou = pfIntersectionOverUnion(predicted, detections[di].bbox);
                    const double center_distance = pfDistance(predicted.center(), detections[di].bbox.center());
                    if (center_distance > center_gate) continue;
                    if (iou < config_.match_iou_threshold && center_distance > center_gate * 0.5) continue;
                    MatchCandidate candidate;
                    candidate.cost = (1.0 - iou) + center_distance / center_gate;
                    candidate.track_index = ti;
                    candidate.detection_index = di;
                    candidates.push_back(candidate);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const MatchCandidate& a, const MatchCandidate& b) {
                return std::tie(a.cost, a.track_index, a.detection_index) <
                    std::tie(b.cost, b.track_index, b.detection_index);
            });
            for (const MatchCandidate& candidate : candidates) {
                if (track_matched[candidate.track_index] || detection_matched[candidate.detection_index]) continue;
                applyDetection(tracks_[candidate.track_index], detections[candidate.detection_index], timestamp_ms);
                track_matched[candidate.track_index] = true;
                detection_matched[candidate.detection_index] = true;
            }
        };

        run_match_pass(true);
        run_match_pass(false);

        for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
            if (track_matched[ti]) continue;
            PersonTrack& track = tracks_[ti];
            const PfPoint previous_center = track.bbox.center();
            track.bbox = predict(track, timestamp_ms);
            const PfPoint predicted_center = track.bbox.center();
            track.anchor_point.x += predicted_center.x - previous_center.x;
            track.anchor_point.y += predicted_center.y - previous_center.y;
            if (config_.use_alpha_beta_filter && track.motion_initialized) {
                track.filtered_center = predicted_center;
            }
            ++track.missed;
            track.last_timestamp_ms = timestamp_ms;
            track.trail.push_back(track.anchor_point);
            while (track.trail.size() > static_cast<std::size_t>(config_.trail_length)) track.trail.pop_front();
        }

        for (std::size_t di = 0; di < detections.size(); ++di) {
            if (detection_matched[di] || !detections[di].high_confidence) continue;
            PersonTrack track;
            track.track_id = next_track_id_++;
            track.bbox = detections[di].bbox;
            track.anchor_point = detections[di].anchor_point;
            track.age = 1;
            track.hits = 1;
            track.confirmed = config_.min_hits <= 1;
            track.matched_this_frame = true;
            track.confidence = detections[di].confidence;
            track.last_timestamp_ms = timestamp_ms;
            track.filtered_center = detections[di].bbox.center();
            track.motion_initialized = config_.use_alpha_beta_filter;
            track.trail.push_back(track.anchor_point);
            tracks_.push_back(std::move(track));
        }

        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(), [&](const PersonTrack& track) {
                return track.missed > config_.max_age_frames;
            }),
            tracks_.end()
        );
        return tracks_;
    }

    std::vector<PersonTrack> PersonTracker::confirmedTracks() const {
        std::vector<PersonTrack> result;
        for (const PersonTrack& track : tracks_) {
            if (track.confirmed && track.missed <= config_.max_age_frames) result.push_back(track);
        }
        return result;
    }

    void PersonTracker::reset() {
        tracks_.clear();
    }

    std::size_t PersonTracker::activeCount() const {
        return tracks_.size();
    }

}  // namespace yolo11_server
