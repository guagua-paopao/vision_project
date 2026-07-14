#include "business/security_live_pipeline.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <utility>

#include "config.h"
#include "server/detection_geometry.h"

namespace yolo11_server {

    namespace {

        struct PoseCandidate {
            double cost = 0.0;
            std::size_t track_index = 0;
            std::size_t detection_index = 0;
        };

        PoseKeypoint mapKeypoint(const Detection& detection, int index, const cv::Size& size) {
            PoseKeypoint point;
            if (index < 0 || index >= kNumberOfPoints || size.width <= 0 || size.height <= 0) return point;
            const int base = index * 3;
            const double raw_x = detection.keypoints[base];
            const double raw_y = detection.keypoints[base + 1];
            point.confidence = detection.keypoints[base + 2];
            if (!std::isfinite(raw_x) || !std::isfinite(raw_y) || !std::isfinite(point.confidence)) {
                point.confidence = 0.0;
                return point;
            }
            const double scale = std::min(
                static_cast<double>(kInputW) / size.width,
                static_cast<double>(kInputH) / size.height);
            const double pad_x = (kInputW - size.width * scale) * 0.5;
            const double pad_y = (kInputH - size.height * scale) * 0.5;
            point.x = std::clamp((raw_x - pad_x) / scale, 0.0, static_cast<double>(size.width - 1));
            point.y = std::clamp((raw_y - pad_y) / scale, 0.0, static_cast<double>(size.height - 1));
            return point;
        }

        bool stripSuffix(const std::string& event_type, const std::string& suffix, std::string& label) {
            if (event_type.size() <= suffix.size() ||
                event_type.compare(event_type.size() - suffix.size(), suffix.size(), suffix) != 0) {
                return false;
            }
            label = event_type.substr(0, event_type.size() - suffix.size());
            return true;
        }

    }  // namespace

    SecurityLivePipeline::SecurityLivePipeline(
        const PeopleFlowSecuritySection& config,
        std::string session_id,
        std::string camera_id
    ) : config_(config),
        fence_(config.zones, session_id, camera_id),
        analytics_(),
        pose_actions_(config.pose, session_id, camera_id),
        temporal_actions_(
            config.temporal,
            std::make_unique<FeatureThresholdTemporalClassifier>(config.temporal_demo_label, 0, 1.0),
            std::move(session_id),
            std::move(camera_id)) {
    }

    std::vector<TrackedPose> SecurityLivePipeline::associatePoses(
        const std::vector<PersonTrack>& tracks,
        const ModelOutput& pose_output,
        const cv::Size& frame_size,
        long long timestamp_ms
    ) const {
        std::vector<PfRect> pose_boxes;
        std::vector<const Detection*> pose_detections;
        for (const Detection& detection : pose_output.detections) {
            const DetectionImageGeometry geometry = detectionToImageGeometry(detection, frame_size);
            if (!geometry.valid) continue;
            pose_boxes.push_back({
                static_cast<double>(geometry.clipped_bbox.x),
                static_cast<double>(geometry.clipped_bbox.y),
                static_cast<double>(geometry.clipped_bbox.x + geometry.clipped_bbox.width),
                static_cast<double>(geometry.clipped_bbox.y + geometry.clipped_bbox.height)
            });
            pose_detections.push_back(&detection);
        }

        const double diagonal = std::max(1.0, std::hypot(
            static_cast<double>(frame_size.width), static_cast<double>(frame_size.height)));
        const double distance_gate = diagonal * config_.pose_match_distance_norm;
        std::vector<PoseCandidate> candidates;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            if (!tracks[ti].confirmed || tracks[ti].missed > 0) continue;
            for (std::size_t di = 0; di < pose_boxes.size(); ++di) {
                const double iou = pfIntersectionOverUnion(tracks[ti].bbox, pose_boxes[di]);
                const double distance = pfDistance(tracks[ti].bbox.center(), pose_boxes[di].center());
                if (iou < config_.pose_match_iou_threshold && distance > distance_gate) continue;
                candidates.push_back({ (1.0 - iou) + distance / distance_gate, ti, di });
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const PoseCandidate& left, const PoseCandidate& right) {
            return std::tie(left.cost, left.track_index, left.detection_index) <
                std::tie(right.cost, right.track_index, right.detection_index);
        });

        std::vector<bool> used_tracks(tracks.size(), false);
        std::vector<bool> used_detections(pose_boxes.size(), false);
        std::vector<TrackedPose> result;
        for (const PoseCandidate& candidate : candidates) {
            if (used_tracks[candidate.track_index] || used_detections[candidate.detection_index]) continue;
            used_tracks[candidate.track_index] = true;
            used_detections[candidate.detection_index] = true;
            TrackedPose pose;
            pose.track_id = tracks[candidate.track_index].track_id;
            pose.timestamp_ms = timestamp_ms;
            pose.bbox = pose_boxes[candidate.detection_index];
            pose.keypoints.reserve(kNumberOfPoints);
            for (int index = 0; index < kNumberOfPoints; ++index) {
                pose.keypoints.push_back(mapKeypoint(
                    *pose_detections[candidate.detection_index], index, frame_size));
            }
            result.push_back(std::move(pose));
        }
        return result;
    }

    void SecurityLivePipeline::applyActionEvents(
        const std::vector<SecurityEvent>& events,
        std::map<std::int64_t, std::vector<std::string>>& active
    ) {
        for (const SecurityEvent& event : events) {
            std::string label;
            if (stripSuffix(event.event_type, "_START", label)) {
                auto& labels = active[event.track_id];
                if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
            }
            else if (stripSuffix(event.event_type, "_END", label)) {
                auto it = active.find(event.track_id);
                if (it == active.end()) continue;
                auto& labels = it->second;
                labels.erase(std::remove(labels.begin(), labels.end(), label), labels.end());
                if (labels.empty()) active.erase(it);
            }
        }
    }

    void SecurityLivePipeline::appendRecent(const std::vector<SecurityEvent>& events) {
        for (const SecurityEvent& event : events) recent_events_.push_back(event);
        while (recent_events_.size() > static_cast<std::size_t>(config_.max_recent_events)) {
            recent_events_.pop_front();
        }
    }

    SecurityFrameResult SecurityLivePipeline::update(
        const std::vector<PersonTrack>& tracks,
        const ModelOutput& pose_output,
        const cv::Size& frame_size,
        long long timestamp_ms
    ) {
        SecurityFrameResult result;
        result.timestamp_ms = timestamp_ms;
        result.track_analytics = analytics_.update(tracks, timestamp_ms);
        result.poses = associatePoses(tracks, pose_output, frame_size, timestamp_ms);

        auto append = [&](std::vector<SecurityEvent> events) {
            result.new_events.insert(result.new_events.end(), events.begin(), events.end());
            return events;
        };
        append(fence_.update(tracks, frame_size.width, frame_size.height, timestamp_ms));
        const auto pose_events = append(pose_actions_.update(result.poses, result.track_analytics, timestamp_ms));
        applyActionEvents(pose_events, active_pose_actions_);

        std::vector<SecurityEvent> temporal_events;
        for (const TrackAnalyticsSnapshot& analytics : result.track_analytics) {
            const double motion_score = std::clamp(
                analytics.instantaneous_speed_px_s / config_.temporal_motion_speed_px_s, 0.0, 1.0);
            auto events = temporal_actions_.update(
                analytics.track_id, { timestamp_ms, { motion_score } });
            temporal_events.insert(temporal_events.end(), events.begin(), events.end());
        }
        append(temporal_events);
        applyActionEvents(temporal_events, active_temporal_actions_);
        appendRecent(result.new_events);

        result.zone_statuses = fence_.statuses(timestamp_ms);
        result.recent_events.assign(recent_events_.begin(), recent_events_.end());
        result.active_pose_actions = active_pose_actions_;
        result.active_temporal_actions = active_temporal_actions_;
        result.temporal_demo_classifier = true;
        return result;
    }

    void SecurityLivePipeline::reset() {
        fence_.reset();
        analytics_.reset();
        pose_actions_.reset();
        temporal_actions_.reset();
        recent_events_.clear();
        active_pose_actions_.clear();
        active_temporal_actions_.clear();
    }

}  // namespace yolo11_server
