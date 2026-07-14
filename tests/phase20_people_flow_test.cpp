#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include <opencv2/core.hpp>

#include "business/line_crossing_counter.h"
#include "business/person_detector_adapter.h"
#include "business/person_tracker.h"
#include "business/people_flow_renderer.h"
#include "server/detection_geometry.h"

using namespace yolo11_server;

namespace {

    Detection modelDetection(float class_id, float confidence, float x, float y, float width, float height,
        int frame_width = 100, int frame_height = 100) {
        Detection result{};
        result.class_id = class_id;
        result.conf = confidence;
        const double scale = std::min(
            static_cast<double>(kInputW) / frame_width,
            static_cast<double>(kInputH) / frame_height);
        const double pad_x = (kInputW - frame_width * scale) * 0.5;
        const double pad_y = (kInputH - frame_height * scale) * 0.5;
        result.bbox[0] = static_cast<float>(x * scale + pad_x);
        result.bbox[1] = static_cast<float>(y * scale + pad_y);
        result.bbox[2] = static_cast<float>((x + width) * scale + pad_x);
        result.bbox[3] = static_cast<float>((y + height) * scale + pad_y);
        return result;
    }

    PersonDetection personDetection(double x, double y, double w, double h, long long timestamp_ms,
        double confidence = 0.9) {
        PersonDetection result;
        result.bbox = { x, y, x + w, y + h };
        result.anchor_point = result.bbox.bottomCenter();
        result.timestamp_ms = timestamp_ms;
        result.confidence = confidence;
        result.high_confidence = confidence >= 0.45;
        return result;
    }

    PersonTrack personTrack(std::int64_t id, double x, double y, int hits = 5) {
        PersonTrack result;
        result.track_id = id;
        result.bbox = { x - 5, y - 20, x + 5, y };
        result.anchor_point = { x, y };
        result.hits = hits;
        result.confirmed = true;
        result.matched_this_frame = true;
        result.confidence = 0.9;
        result.trail.push_back({ x - 2, y + 3 });
        result.trail.push_back({ x, y });
        return result;
    }

    void testAdapterDebugDecisions() {
        PeopleFlowSection config;
        config.roi.enabled = false;
        config.person.conf_low = 0.20;
        config.person.conf_high = 0.50;
        config.person.min_width_px = 10;
        config.person.min_height_px = 10;
        config.person.max_aspect_ratio = 3.0;
        PersonDetectorAdapter adapter(config);

        ModelOutput output;
        output.detections = {
            modelDetection(2, 0.9f, 50, 50, 20, 30),  // non-person, statistics only
            modelDetection(0, 0.1f, 50, 50, 20, 30),
            modelDetection(0, 0.9f, 50, 50, -1, 30),
            modelDetection(0, 0.9f, 50, 50, 5, 30),
            modelDetection(0, 0.9f, 50, 50, 10, 40),
            modelDetection(0, 0.3f, 50, 50, 20, 30),
            modelDetection(0, 0.8f, 50, 50, 20, 30)
        };

        const PersonFilterResult debug = adapter.filterWithDebug(output, cv::Size(100, 100), 123);
        assert(debug.statistics.raw_all_classes == 7);
        assert(debug.statistics.raw_person == 6);
        assert(debug.statistics.rejected_low_confidence == 1);
        assert(debug.statistics.rejected_invalid_bbox == 1);
        assert(debug.statistics.rejected_minimum_size == 1);
        assert(debug.statistics.rejected_aspect_ratio == 1);
        assert(debug.statistics.rejected_roi == 0);
        assert(debug.statistics.accepted_low == 1);
        assert(debug.statistics.accepted_high == 1);
        assert(debug.accepted.size() == 2);
        assert(debug.debug_items.size() == 6);
        assert(debug.debug_items.front().clipped_bbox.area() > 0.0);  // low-confidence box remains drawable.

        const auto lightweight = adapter.filter(output, cv::Size(100, 100), 123);
        assert(lightweight.size() == debug.accepted.size());
        for (std::size_t i = 0; i < lightweight.size(); ++i) {
            assert(std::abs(lightweight[i].confidence - debug.accepted[i].confidence) < 1e-9);
            assert(lightweight[i].high_confidence == debug.accepted[i].high_confidence);
        }

        config.roi.enabled = true;
        config.roi.polygon_norm = { { 0.0, 0.0 }, { 0.4, 0.0 }, { 0.4, 1.0 }, { 0.0, 1.0 } };
        PersonDetectorAdapter roi_adapter(config);
        ModelOutput roi_output;
        roi_output.detections = { modelDetection(0, 0.8f, 80, 50, 20, 30) };
        const auto roi_debug = roi_adapter.filterWithDebug(roi_output, cv::Size(100, 100), 456);
        assert(roi_debug.accepted.empty());
        assert(roi_debug.statistics.rejected_roi == 1);
        assert(roi_debug.debug_items.front().decision == DetectionFilterDecision::RejectedRoi);

        assert(detectionFilterDecisionToString(DetectionFilterDecision::AcceptedHigh) == "accepted_high");
        assert(detectionFilterDecisionToString(DetectionFilterDecision::RejectedRoi) == "rejected_roi");
    }

    void testAdapterMapsTensorRtXyxyToOriginalImage() {
        PeopleFlowSection config;
        config.roi.enabled = false;
        config.person.conf_low = 0.10;
        config.person.conf_high = 0.25;
        config.person.min_width_px = 1;
        config.person.min_height_px = 1;
        PersonDetectorAdapter adapter(config);

        ModelOutput output;
        output.detections = {
            modelDetection(0, 0.8f, 200, 100, 40, 120, 384, 288)
        };
        const auto result = adapter.filterWithDebug(output, cv::Size(384, 288), 1000);
        assert(result.accepted.size() == 1);
        const PfRect& box = result.accepted.front().bbox;
        assert(std::abs(box.x1 - 200.0) <= 1.0);
        assert(std::abs(box.y1 - 100.0) <= 1.0);
        assert(std::abs(box.x2 - 240.0) <= 1.0);
        assert(std::abs(box.y2 - 220.0) <= 1.0);
        assert(std::abs(result.accepted.front().anchor_point.x - 220.0) <= 1.0);
        assert(std::abs(result.accepted.front().anchor_point.y - 220.0) <= 1.0);
    }

    void testTrackerLifecycleAndMatchFlag() {
        PeopleFlowTrackerSection config;
        config.min_hits = 2;
        config.max_age_frames = 2;
        config.match_iou_threshold = 0.1;
        config.center_distance_gate_norm = 0.5;
        PersonTracker tracker(config);

        const auto& tentative = tracker.update(
            { personDetection(10, 10, 20, 40, 0) }, 100, 100, 0);
        assert(tentative.size() == 1);
        assert(!tentative.front().confirmed && tentative.front().matched_this_frame);

        const auto& confirmed = tracker.update(
            { personDetection(12, 12, 20, 40, 100) }, 100, 100, 100);
        assert(confirmed.size() == 1);
        const auto stable_id = confirmed.front().track_id;
        assert(confirmed.front().confirmed && confirmed.front().matched_this_frame);

        const auto& predicted = tracker.update({}, 100, 100, 200);
        assert(predicted.size() == 1);
        assert(predicted.front().track_id == stable_id);
        assert(predicted.front().missed == 1 && !predicted.front().matched_this_frame);

        const auto& recovered = tracker.update(
            { personDetection(16, 14, 20, 40, 300) }, 100, 100, 300);
        assert(recovered.size() == 1);
        assert(recovered.front().track_id == stable_id);
        assert(recovered.front().missed == 0 && recovered.front().matched_this_frame);

        tracker.update({}, 100, 100, 400);
        tracker.update({}, 100, 100, 500);
        const auto& removed = tracker.update({}, 100, 100, 600);
        assert(removed.empty());
    }

    void testCounterDebugAndCounting() {
        PeopleFlowCountingSection config;
        config.line_a_norm = { 0.1, 0.5 };
        config.line_b_norm = { 0.9, 0.5 };
        config.hysteresis_px = 4.0;
        config.min_hits_for_count = 3;
        config.transition_positive_to_negative = "IN";
        config.rearm_distance_px = 8.0;
        config.rearm_frames = 2;
        config.min_crossing_interval_ms = 100;
        config.max_crossing_gap_ms = 1000;
        LineCrossingCounter counter(config, "test_session", "entry_camera_01", "test-v1", 0);

        assert(counter.classifySide({ 50, 55 }, 100, 100) == 1);
        assert(counter.classifySide({ 50, 54 }, 100, 100) == 0);   // exact +hysteresis boundary
        assert(counter.classifySide({ 50, 46 }, 100, 100) == 0);   // exact -hysteresis boundary
        assert(counter.classifySide({ 50, 45 }, 100, 100) == -1);

        const auto unknown = counter.debugStates({ personTrack(99, 50, 70) }, 100, 100);
        assert(unknown.size() == 1);
        assert(unknown.front().stable_side == 0 && !unknown.front().has_previous_point);
        // If debugStates inserted/mutated state, this first update would count a crossing.
        assert(counter.update({ personTrack(99, 50, 30) }, 100, 100, 1).empty());

        assert(counter.update({ personTrack(1, 50, 70) }, 100, 100, 100).empty());
        const auto before_debug = counter.counts();
        const auto states = counter.debugStates({ personTrack(1, 50, 70) }, 100, 100);
        const auto after_debug = counter.counts();
        assert(before_debug.in_count == after_debug.in_count && before_debug.out_count == after_debug.out_count);
        assert(states.front().current_side == 1 && states.front().stable_side == 1);
        assert(states.front().has_previous_point && states.front().min_hits_ready);

        assert(counter.update({ personTrack(1, 50, 52) }, 100, 100, 200).empty());
        const auto in_events = counter.update({ personTrack(1, 50, 35) }, 100, 100, 300);
        assert(in_events.size() == 1 && in_events.front().direction == "IN");
        const auto counted = counter.debugStates({ personTrack(1, 50, 35) }, 100, 100);
        assert(counted.front().counted && counted.front().stable_side == -1);
        assert(counter.update({ personTrack(1, 50, 70) }, 100, 100, 400).empty());
        assert(counter.counts().in_count == 1);  // per-track deduplication

        assert(counter.update({ personTrack(2, 60, 30) }, 100, 100, 500).empty());
        const auto out_events = counter.update({ personTrack(2, 60, 70) }, 100, 100, 600);
        assert(out_events.size() == 1 && out_events.front().direction == "OUT");
        assert(counter.counts().out_count == 1 && counter.counts().occupancy == 0);

        LineCrossingCounter reentry_counter(config, "reentry", "entry_camera_01", "test-v2", 1);
        assert(reentry_counter.update({ personTrack(3, 50, 70) }, 100, 100, 0).empty());
        const auto first_crossing = reentry_counter.update(
            { personTrack(3, 50, 30) }, 100, 100, 100);
        assert(first_crossing.size() == 1 && first_crossing.front().direction == "IN");
        assert(reentry_counter.update({ personTrack(3, 50, 25) }, 100, 100, 200).empty());
        assert(reentry_counter.update({ personTrack(3, 50, 20) }, 100, 100, 300).empty());
        const auto second_crossing = reentry_counter.update(
            { personTrack(3, 50, 70) }, 100, 100, 500);
        assert(second_crossing.size() == 1 && second_crossing.front().direction == "OUT");
        assert(reentry_counter.counts().in_count == 1 && reentry_counter.counts().out_count == 1);
        assert(reentry_counter.counts().occupancy == 1);

        LineCrossingCounter jitter_counter(config, "jitter", "entry_camera_01", "test-v1", 0);
        for (int i = 0; i < 20; ++i) {
            const double y = (i % 2 == 0) ? 52.0 : 48.0;
            assert(jitter_counter.update(
                { personTrack(10, 50, y) }, 100, 100, 1000 + i * 100).empty());
        }

        LineCrossingCounter finite_counter(config, "finite", "entry_camera_01", "test-v1", 0);
        assert(finite_counter.update({ personTrack(20, 0, 70) }, 100, 100, 0).empty());
        assert(finite_counter.update({ personTrack(20, 0, 30) }, 100, 100, 100).empty());

        LineCrossingCounter consecutive_counter(config, "consecutive", "entry_camera_01", "test-v1", 0);
        assert(consecutive_counter.update({ personTrack(30, 40, 70) }, 100, 100, 0).empty());
        assert(consecutive_counter.update({ personTrack(30, 40, 30) }, 100, 100, 100).size() == 1);
        assert(consecutive_counter.update({ personTrack(31, 60, 70) }, 100, 100, 200).empty());
        assert(consecutive_counter.update({ personTrack(31, 60, 30) }, 100, 100, 300).size() == 1);
        assert(consecutive_counter.counts().in_count == 2);
        assert(consecutive_counter.update({}, 100, 100, 400).empty());  // empty scene

        config.transition_positive_to_negative = "OUT";
        LineCrossingCounter reversed_counter(config, "reversed", "entry_camera_01", "test-v1", 0);
        assert(reversed_counter.update({ personTrack(40, 50, 70) }, 100, 100, 0).empty());
        const auto reversed_event = reversed_counter.update(
            { personTrack(40, 50, 30) }, 100, 100, 100);
        assert(reversed_event.size() == 1 && reversed_event.front().direction == "OUT");

        counter.resetTrackState();
        assert(counter.counts().in_count == 1 && counter.counts().out_count == 1);
        counter.calibrateOccupancy(7);
        assert(counter.counts().occupancy == 7);
    }

    void testRendererAtSupportedResolutions() {
        PeopleFlowSection config;
        config.visualization.enabled = true;
        config.visualization.fill_roi = true;
        config.visualization.draw_filter_statistics = true;
        config.visualization.draw_legend = true;
        config.visualization.draw_velocity = true;
        config.visualization.event_marker_hold_frames = 2;
        PeopleFlowRenderer renderer(config);

        PeopleFlowFrameDebug debug;
        debug.frame_index = 120;
        debug.timestamp_ms = 4800;
        debug.inference_frame = true;
        debug.filter_statistics.raw_all_classes = 2;
        debug.filter_statistics.raw_person = 1;
        debug.filter_statistics.accepted_high = 1;
        DetectionDebugItem item;
        item.class_id = 0;
        item.confidence = 0.82;
        item.raw_bbox = { 20, 20, 80, 140 };
        item.clipped_bbox = item.raw_bbox;
        item.anchor_point = item.clipped_bbox.bottomCenter();
        item.decision = DetectionFilterDecision::AcceptedHigh;
        item.accepted = true;
        item.high_confidence = true;
        debug.detection_items.push_back(item);
        debug.all_tracks.push_back(personTrack(3, 50, 140));
        TrackCounterDebugState state;
        state.track_id = 3;
        state.current_side = 1;
        state.stable_side = 1;
        state.min_hits_ready = true;
        state.current_point = debug.all_tracks.front().anchor_point;
        debug.counter_states.push_back(state);
        CrossingEvent event;
        event.event_id = "render_event_1";
        event.track_id = 3;
        event.direction = "IN";
        event.event_time_ms = 4800;
        event.point_x_norm = 0.5;
        event.point_y_norm = 0.5;
        debug.new_events.push_back(event);

        for (const cv::Size size : { cv::Size(384, 288), cv::Size(640, 480), cv::Size(1920, 1080) }) {
            cv::Mat frame(size.height, size.width, CV_8UC3, cv::Scalar(0, 0, 0));
            const cv::Mat rendered = renderer.render(
                frame, {}, PeopleFlowCounts{}, PeopleFlowRenderMetrics{}, &debug);
            assert(!rendered.empty());
            assert(rendered.cols == size.width && rendered.rows == size.height);
        }
        renderer.resetEventMarkers();

        // Disabled visualization keeps the legacy overlay path available and
        // cannot alter tracking/counting state.
        config.visualization.enabled = false;
        PeopleFlowRenderer legacy_renderer(config);
        cv::Mat frame(288, 384, CV_8UC3, cv::Scalar(0, 0, 0));
        const cv::Mat rendered = legacy_renderer.render(
            frame, debug.all_tracks, PeopleFlowCounts{}, PeopleFlowRenderMetrics{});
        assert(!rendered.empty() && rendered.cols == frame.cols && rendered.rows == frame.rows);
    }

}  // namespace

int main() {
    testAdapterDebugDecisions();
    testAdapterMapsTensorRtXyxyToOriginalImage();
    testTrackerLifecycleAndMatchFlag();
    testCounterDebugAndCounting();
    testRendererAtSupportedResolutions();
    std::cout << "Phase 20 people-flow visual/core tests passed\n";
    return 0;
}
