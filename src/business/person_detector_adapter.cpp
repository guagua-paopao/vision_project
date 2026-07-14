#include "business/person_detector_adapter.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "server/detection_geometry.h"

namespace yolo11_server {

    namespace {

        bool pointInPolygon(const PfPoint& point, const std::vector<NormalizedPoint>& polygon, const cv::Size& size) {
            if (polygon.size() < 3 || size.width <= 0 || size.height <= 0) return false;
            bool inside = false;
            for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
                const double xi = polygon[i].x * size.width;
                const double yi = polygon[i].y * size.height;
                const double xj = polygon[j].x * size.width;
                const double yj = polygon[j].y * size.height;
                const bool crosses = ((yi > point.y) != (yj > point.y)) &&
                    (point.x < (xj - xi) * (point.y - yi) / ((yj - yi) + 1e-12) + xi);
                if (crosses) inside = !inside;
            }
            return inside;
        }

    }  // namespace

    PersonDetectorAdapter::PersonDetectorAdapter(const PeopleFlowSection& config)
        : config_(config) {
    }

    bool PersonDetectorAdapter::acceptsAnchor(const PfPoint& point, const cv::Size& frame_size) const {
        if (!config_.roi.enabled) return true;
        return pointInPolygon(point, config_.roi.polygon_norm, frame_size);
    }

    std::vector<PersonDetection> PersonDetectorAdapter::filter(
        const ModelOutput& output,
        const cv::Size& frame_size,
        long long timestamp_ms
    ) const {
        return filter(output, frame_size, timestamp_ms, nullptr);
    }

    PersonFilterResult PersonDetectorAdapter::filterWithDebug(
        const ModelOutput& output,
        const cv::Size& frame_size,
        long long timestamp_ms
    ) const {
        PersonFilterResult result;
        filter(output, frame_size, timestamp_ms, &result);
        return result;
    }

    std::vector<PersonDetection> PersonDetectorAdapter::filter(
        const ModelOutput& output,
        const cv::Size& frame_size,
        long long timestamp_ms,
        PersonFilterResult* debug_result
    ) const {
        std::vector<PersonDetection> persons;
        if (debug_result) {
            debug_result->accepted.clear();
            debug_result->debug_items.clear();
            debug_result->statistics = {};
            debug_result->statistics.raw_all_classes = static_cast<int>(output.detections.size());
            debug_result->debug_items.reserve(output.detections.size());
        }
        if (frame_size.width <= 0 || frame_size.height <= 0) return persons;

        auto record = [&](DetectionDebugItem&& item, DetectionFilterDecision decision) {
            if (!debug_result) return;
            item.decision = decision;
            item.accepted = decision == DetectionFilterDecision::AcceptedHigh ||
                decision == DetectionFilterDecision::AcceptedLow;
            item.high_confidence = decision == DetectionFilterDecision::AcceptedHigh;
            switch (decision) {
            case DetectionFilterDecision::AcceptedHigh: ++debug_result->statistics.accepted_high; break;
            case DetectionFilterDecision::AcceptedLow: ++debug_result->statistics.accepted_low; break;
            case DetectionFilterDecision::RejectedLowConfidence: ++debug_result->statistics.rejected_low_confidence; break;
            case DetectionFilterDecision::RejectedInvalidBBox: ++debug_result->statistics.rejected_invalid_bbox; break;
            case DetectionFilterDecision::RejectedMinimumSize: ++debug_result->statistics.rejected_minimum_size; break;
            case DetectionFilterDecision::RejectedAspectRatio: ++debug_result->statistics.rejected_aspect_ratio; break;
            case DetectionFilterDecision::RejectedRoi: ++debug_result->statistics.rejected_roi; break;
            }
            debug_result->debug_items.push_back(std::move(item));
        };

        persons.reserve(output.detections.size());
        for (const Detection& detection : output.detections) {
            if (!std::isfinite(detection.class_id)) continue;
            const int class_id = static_cast<int>(std::lround(detection.class_id));
            if (class_id != config_.person.class_id) continue;
            if (debug_result) ++debug_result->statistics.raw_person;

            DetectionDebugItem debug_item;
            debug_item.class_id = class_id;
            const DetectionImageGeometry geometry = detectionToImageGeometry(detection, frame_size);
            const bool debug_bbox_valid = geometry.valid;
            if (std::isfinite(geometry.mapped_bbox.x) && std::isfinite(geometry.mapped_bbox.y) &&
                std::isfinite(geometry.mapped_bbox.width) && std::isfinite(geometry.mapped_bbox.height) &&
                geometry.mapped_bbox.width > 0.0 && geometry.mapped_bbox.height > 0.0) {
                debug_item.raw_bbox = {
                    geometry.mapped_bbox.x,
                    geometry.mapped_bbox.y,
                    geometry.mapped_bbox.x + geometry.mapped_bbox.width,
                    geometry.mapped_bbox.y + geometry.mapped_bbox.height
                };
            }
            if (debug_bbox_valid) {
                debug_item.clipped_bbox = {
                    static_cast<double>(geometry.clipped_bbox.x),
                    static_cast<double>(geometry.clipped_bbox.y),
                    static_cast<double>(geometry.clipped_bbox.x + geometry.clipped_bbox.width),
                    static_cast<double>(geometry.clipped_bbox.y + geometry.clipped_bbox.height)
                };
            }
            const double confidence = detection.conf;
            debug_item.confidence = std::isfinite(confidence) ? confidence : 0.0;
            if (!std::isfinite(confidence) || confidence < config_.person.conf_low) {
                record(std::move(debug_item), DetectionFilterDecision::RejectedLowConfidence);
                continue;
            }

            if (!debug_bbox_valid) {
                record(std::move(debug_item), DetectionFilterDecision::RejectedInvalidBBox);
                continue;
            }

            const PfRect box = debug_item.clipped_bbox;

            const double clipped_width = box.width();
            const double clipped_height = box.height();
            if (clipped_width < config_.person.min_width_px || clipped_height < config_.person.min_height_px) {
                record(std::move(debug_item), DetectionFilterDecision::RejectedMinimumSize);
                continue;
            }
            const double aspect = std::max(clipped_width / std::max(1.0, clipped_height),
                clipped_height / std::max(1.0, clipped_width));
            if (aspect > config_.person.max_aspect_ratio) {
                record(std::move(debug_item), DetectionFilterDecision::RejectedAspectRatio);
                continue;
            }

            const PfPoint anchor = config_.person.anchor_point == "center" ? box.center() : box.bottomCenter();
            debug_item.anchor_point = anchor;
            if (!acceptsAnchor(anchor, frame_size)) {
                record(std::move(debug_item), DetectionFilterDecision::RejectedRoi);
                continue;
            }

            PersonDetection person;
            person.bbox = box;
            person.confidence = confidence;
            person.anchor_point = anchor;
            person.timestamp_ms = timestamp_ms;
            person.high_confidence = confidence >= config_.person.conf_high;
            persons.push_back(person);
            record(std::move(debug_item), person.high_confidence
                ? DetectionFilterDecision::AcceptedHigh
                : DetectionFilterDecision::AcceptedLow);
        }
        if (debug_result) debug_result->accepted = persons;
        return persons;
    }

}  // namespace yolo11_server
