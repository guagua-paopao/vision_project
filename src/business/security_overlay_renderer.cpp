#include "business/security_overlay_renderer.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace yolo11_server {

    namespace {
        const std::vector<std::pair<int, int>> kSkeleton{
            {0, 1}, {0, 2}, {0, 5}, {0, 6}, {1, 3}, {2, 4},
            {5, 6}, {5, 7}, {6, 8}, {5, 11}, {6, 12}, {7, 9},
            {8, 10}, {11, 12}, {11, 13}, {12, 14}, {13, 15}, {14, 16}
        };

        cv::Point point(const PfPoint& value, const cv::Size& size) {
            return {
                std::clamp(static_cast<int>(std::lround(value.x)), 0, std::max(0, size.width - 1)),
                std::clamp(static_cast<int>(std::lround(value.y)), 0, std::max(0, size.height - 1))
            };
        }

        cv::Point normalizedPoint(const PfPoint& value, const cv::Size& size) {
            return {
                std::clamp(static_cast<int>(std::lround(value.x * size.width)), 0, std::max(0, size.width - 1)),
                std::clamp(static_cast<int>(std::lround(value.y * size.height)), 0, std::max(0, size.height - 1))
            };
        }

        int activeCount(const std::map<std::int64_t, std::vector<std::string>>& actions) {
            int count = 0;
            for (const auto& [track_id, labels] : actions) {
                (void)track_id;
                count += static_cast<int>(labels.size());
            }
            return count;
        }

        std::string eventLabel(const SecurityEvent& event) {
            std::ostringstream text;
            text << event.event_type << " T" << event.track_id;
            if (event.demo_classifier) text << " [DEMO]";
            return text.str();
        }
    }

    SecurityOverlayRenderer::SecurityOverlayRenderer(PeopleFlowSecuritySection config)
        : config_(std::move(config)) {
    }

    cv::Mat SecurityOverlayRenderer::render(
        const cv::Mat& frame,
        const SecurityFrameResult& security
    ) const {
        if (frame.empty()) return {};
        cv::Mat canvas = frame.clone();
        if (config_.draw_zones) {
            for (const SecurityZoneConfig& zone : config_.zones) {
                bool occupied = false;
                for (const TrackZoneStatus& status : security.zone_statuses) {
                    occupied = occupied || (status.zone_id == zone.zone_id && status.inside);
                }
                const cv::Scalar color = occupied ? cv::Scalar(0, 80, 255) : cv::Scalar(0, 210, 255);
                std::vector<cv::Point> polygon;
                for (const PfPoint& value : zone.polygon_norm) polygon.push_back(normalizedPoint(value, canvas.size()));
                if (polygon.size() >= 3) {
                    cv::Mat overlay = canvas.clone();
                    cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{ polygon }, color);
                    cv::addWeighted(overlay, occupied ? 0.18 : 0.09, canvas, occupied ? 0.82 : 0.91, 0.0, canvas);
                    cv::polylines(canvas, polygon, true, color, 2, cv::LINE_AA);
                    cv::putText(canvas, "S1 " + zone.zone_id + (occupied ? " ALERT" : " ARMED"),
                        polygon.front() + cv::Point(4, 18), cv::FONT_HERSHEY_SIMPLEX, 0.52,
                        color, 2, cv::LINE_AA);
                }
            }
        }

        if (config_.draw_pose) {
            for (const TrackedPose& pose : security.poses) {
                const cv::Scalar color(255, 120, 30);
                for (const auto& [from, to] : kSkeleton) {
                    if (from >= static_cast<int>(pose.keypoints.size()) || to >= static_cast<int>(pose.keypoints.size())) continue;
                    const PoseKeypoint& first = pose.keypoints[from];
                    const PoseKeypoint& second = pose.keypoints[to];
                    if (first.confidence < config_.pose.min_keypoint_confidence ||
                        second.confidence < config_.pose.min_keypoint_confidence) continue;
                    cv::line(canvas, point({ first.x, first.y }, canvas.size()),
                        point({ second.x, second.y }, canvas.size()), color, 2, cv::LINE_AA);
                }
                for (const PoseKeypoint& keypoint : pose.keypoints) {
                    if (keypoint.confidence < config_.pose.min_keypoint_confidence) continue;
                    cv::circle(canvas, point({ keypoint.x, keypoint.y }, canvas.size()), 3,
                        cv::Scalar(30, 220, 255), cv::FILLED, cv::LINE_AA);
                }
                std::ostringstream label;
                label << "S3 POSE T" << pose.track_id;
                const auto pose_actions = security.active_pose_actions.find(pose.track_id);
                if (pose_actions != security.active_pose_actions.end()) {
                    for (const std::string& action : pose_actions->second) label << ' ' << action;
                }
                const auto temporal_actions = security.active_temporal_actions.find(pose.track_id);
                if (temporal_actions != security.active_temporal_actions.end()) {
                    for (const std::string& action : temporal_actions->second) label << " S4:" << action;
                }
                cv::putText(canvas, label.str(),
                    point({ pose.bbox.x1, std::max(15.0, pose.bbox.y1 - 8.0) }, canvas.size()),
                    cv::FONT_HERSHEY_SIMPLEX, 0.46, color, 2, cv::LINE_AA);
            }
        }

        for (const SecurityEvent& event : security.new_events) {
            const auto duplicate = std::find_if(markers_.begin(), markers_.end(), [&](const EventMarker& marker) {
                return marker.event.event_id == event.event_id;
            });
            if (duplicate == markers_.end()) markers_.push_back({ event, config_.event_marker_hold_frames });
        }
        int toast_y = std::max(26, canvas.rows - 24);
        for (EventMarker& marker : markers_) {
            const cv::Scalar color = marker.event.demo_classifier
                ? cv::Scalar(0, 165, 255) : cv::Scalar(40, 40, 255);
            cv::putText(canvas, eventLabel(marker.event), cv::Point(12, toast_y),
                cv::FONT_HERSHEY_SIMPLEX, 0.58, color, 2, cv::LINE_AA);
            toast_y -= 24;
            --marker.remaining_frames;
        }
        markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
            [](const EventMarker& marker) { return marker.remaining_frames <= 0; }), markers_.end());

        if (config_.draw_stage_panel) {
            int inside_count = 0;
            for (const TrackZoneStatus& status : security.zone_statuses) inside_count += status.inside ? 1 : 0;
            const int panel_width = std::min(390, std::max(250, canvas.cols - 20));
            const int panel_height = 126;
            const int x = std::max(5, canvas.cols - panel_width - 8);
            const int y = 8;
            cv::Mat overlay = canvas.clone();
            cv::rectangle(overlay, cv::Rect(x, y, panel_width, std::min(panel_height, canvas.rows - y)),
                cv::Scalar(12, 18, 28), cv::FILLED);
            cv::addWeighted(overlay, 0.86, canvas, 0.14, 0.0, canvas);
            const std::vector<std::pair<std::string, cv::Scalar>> rows{
                { "S1 FENCE   READY  inside=" + std::to_string(inside_count), cv::Scalar(0, 210, 255) },
                { "S2 TRACK   READY  tracks=" + std::to_string(security.track_analytics.size()), cv::Scalar(60, 220, 60) },
                { "S3 POSE    READY  poses=" + std::to_string(security.poses.size()) +
                    " actions=" + std::to_string(activeCount(security.active_pose_actions)), cv::Scalar(255, 150, 50) },
                { "S4 TEMP    DEMO   actions=" + std::to_string(activeCount(security.active_temporal_actions)), cv::Scalar(0, 165, 255) }
            };
            for (std::size_t index = 0; index < rows.size(); ++index) {
                cv::putText(canvas, rows[index].first, cv::Point(x + 10, y + 25 + static_cast<int>(index) * 26),
                    cv::FONT_HERSHEY_SIMPLEX, 0.52, rows[index].second, 1, cv::LINE_AA);
            }
        }
        return canvas;
    }

    void SecurityOverlayRenderer::reset() const {
        markers_.clear();
    }

}  // namespace yolo11_server
