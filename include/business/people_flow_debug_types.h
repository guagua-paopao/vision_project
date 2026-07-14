#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "business/people_flow_types.h"

namespace yolo11_server {

    // Stable decisions exported by frame_debug.jsonl. Do not rename the
    // corresponding strings without versioning the debug artifact contract.
    enum class DetectionFilterDecision {
        AcceptedHigh,
        AcceptedLow,
        RejectedLowConfidence,
        RejectedInvalidBBox,
        RejectedMinimumSize,
        RejectedAspectRatio,
        RejectedRoi
    };

    constexpr std::string_view detectionFilterDecisionToString(DetectionFilterDecision decision) noexcept {
        switch (decision) {
        case DetectionFilterDecision::AcceptedHigh: return "accepted_high";
        case DetectionFilterDecision::AcceptedLow: return "accepted_low";
        case DetectionFilterDecision::RejectedLowConfidence: return "rejected_low_confidence";
        case DetectionFilterDecision::RejectedInvalidBBox: return "rejected_invalid_bbox";
        case DetectionFilterDecision::RejectedMinimumSize: return "rejected_minimum_size";
        case DetectionFilterDecision::RejectedAspectRatio: return "rejected_aspect_ratio";
        case DetectionFilterDecision::RejectedRoi: return "rejected_roi";
        }
        return "rejected_invalid_bbox";
    }

    struct DetectionDebugItem {
        int class_id = -1;
        double confidence = 0.0;
        PfRect raw_bbox;
        PfRect clipped_bbox;
        PfPoint anchor_point;
        DetectionFilterDecision decision = DetectionFilterDecision::RejectedInvalidBBox;
        bool accepted = false;
        bool high_confidence = false;
    };

    struct DetectionFilterStatistics {
        int raw_all_classes = 0;
        int raw_person = 0;
        int accepted_high = 0;
        int accepted_low = 0;
        int rejected_low_confidence = 0;
        int rejected_invalid_bbox = 0;
        int rejected_minimum_size = 0;
        int rejected_aspect_ratio = 0;
        int rejected_roi = 0;
    };

    struct PersonFilterResult {
        std::vector<PersonDetection> accepted;
        std::vector<DetectionDebugItem> debug_items;
        DetectionFilterStatistics statistics;
    };

    struct TrackCounterDebugState {
        std::int64_t track_id = 0;
        int current_side = 0;
        int stable_side = 0;
        bool counted = false;
        bool has_previous_point = false;
        bool min_hits_ready = false;
        PfPoint previous_point;
        PfPoint current_point;
    };

    struct PeopleFlowFrameDebug {
        long long frame_index = 0;
        long long timestamp_ms = 0;
        bool inference_frame = false;
        bool warmup_active = false;
        int warmup_frames_remaining = 0;

        DetectionFilterStatistics filter_statistics;
        std::vector<DetectionDebugItem> detection_items;
        std::vector<PersonTrack> all_tracks;
        std::vector<TrackCounterDebugState> counter_states;
        std::vector<CrossingEvent> new_events;
    };

}  // namespace yolo11_server
