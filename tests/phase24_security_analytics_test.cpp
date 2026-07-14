#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "business/electronic_fence_engine.h"
#include "business/person_tracker.h"
#include "business/pose_action_engine.h"
#include "business/security_live_pipeline.h"
#include "business/security_overlay_renderer.h"
#include "business/temporal_action_engine.h"
#include "business/track_analytics_engine.h"

using namespace yolo11_server;

namespace {

    PersonTrack track(std::int64_t id, double x, double y, int missed = 0) {
        PersonTrack value;
        value.track_id = id;
        value.bbox = { x - 8, y - 30, x + 8, y };
        value.anchor_point = { x, y };
        value.hits = 5;
        value.confirmed = true;
        value.matched_this_frame = missed == 0;
        value.missed = missed;
        value.confidence = 0.9;
        return value;
    }

    PersonDetection detection(double x, double y, long long timestamp_ms, bool high_confidence) {
        PersonDetection value;
        value.bbox = { x - 5, y - 10, x + 5, y + 10 };
        value.anchor_point = { x, y + 10 };
        value.confidence = high_confidence ? 0.90 : 0.25;
        value.high_confidence = high_confidence;
        value.timestamp_ms = timestamp_ms;
        return value;
    }

    bool hasType(const std::vector<SecurityEvent>& events, const std::string& type) {
        for (const SecurityEvent& event : events) {
            if (event.event_type == type) return true;
        }
        return false;
    }

    TrackedPose standingPose(std::int64_t track_id, long long timestamp_ms, bool hands_up) {
        TrackedPose pose;
        pose.track_id = track_id;
        pose.timestamp_ms = timestamp_ms;
        pose.bbox = { 30, 10, 70, 100 };
        pose.keypoints.resize(17);
        for (PoseKeypoint& point : pose.keypoints) point.confidence = 0.95;
        pose.keypoints[5] = { 42, 35, 0.95 };
        pose.keypoints[6] = { 58, 35, 0.95 };
        pose.keypoints[9] = { 40, hands_up ? 15.0 : 52.0, 0.95 };
        pose.keypoints[10] = { 60, hands_up ? 15.0 : 52.0, 0.95 };
        pose.keypoints[11] = { 44, 60, 0.95 };
        pose.keypoints[12] = { 56, 60, 0.95 };
        pose.keypoints[13] = { 44, 78, 0.95 };
        pose.keypoints[14] = { 56, 78, 0.95 };
        pose.keypoints[15] = { 44, 98, 0.95 };
        pose.keypoints[16] = { 56, 98, 0.95 };
        return pose;
    }

    TrackedPose fallenPose(std::int64_t track_id, long long timestamp_ms) {
        TrackedPose pose = standingPose(track_id, timestamp_ms, false);
        pose.bbox = { 10, 40, 100, 70 };
        pose.keypoints[5] = { 25, 52, 0.95 };
        pose.keypoints[6] = { 35, 52, 0.95 };
        pose.keypoints[11] = { 70, 55, 0.95 };
        pose.keypoints[12] = { 80, 55, 0.95 };
        pose.keypoints[13] = { 86, 56, 0.95 };
        pose.keypoints[14] = { 88, 57, 0.95 };
        pose.keypoints[15] = { 96, 58, 0.95 };
        pose.keypoints[16] = { 98, 59, 0.95 };
        return pose;
    }

    ModelOutput poseOutput(double center_x, bool hands_up) {
        ModelOutput output;
        output.model_type = "pose";
        Detection detection{};
        detection.bbox[0] = static_cast<float>(center_x - 30);
        detection.bbox[1] = 350.0f;
        detection.bbox[2] = static_cast<float>(center_x + 30);
        detection.bbox[3] = 500.0f;
        detection.conf = 0.95f;
        detection.class_id = 0.0f;
        auto set = [&](int index, double x, double y) {
            detection.keypoints[index * 3] = static_cast<float>(x);
            detection.keypoints[index * 3 + 1] = static_cast<float>(y);
            detection.keypoints[index * 3 + 2] = 0.95f;
        };
        set(5, center_x - 12, 400);
        set(6, center_x + 12, 400);
        set(9, center_x - 15, hands_up ? 360 : 430);
        set(10, center_x + 15, hands_up ? 360 : 430);
        set(11, center_x - 10, 445);
        set(12, center_x + 10, 445);
        set(13, center_x - 10, 470);
        set(14, center_x + 10, 470);
        set(15, center_x - 10, 495);
        set(16, center_x + 10, 495);
        output.detections.push_back(detection);
        return output;
    }

    void testPhase1ElectronicFence() {
        SecurityZoneConfig zone;
        zone.zone_id = "restricted_a";
        zone.polygon_norm = { { 0.25, 0.25 }, { 0.75, 0.25 }, { 0.75, 0.75 }, { 0.25, 0.75 } };
        zone.enter_confirm_frames = 2;
        zone.exit_confirm_frames = 2;
        zone.dwell_alarm_ms = 500;
        zone.cooldown_ms = 0;
        ElectronicFenceEngine engine({ zone }, "session", "camera");

        assert(!ElectronicFenceEngine::pointInPolygonNormalized({ 10, 50 }, 100, 100, zone.polygon_norm));
        assert(ElectronicFenceEngine::pointInPolygonNormalized({ 50, 50 }, 100, 100, zone.polygon_norm));
        assert(ElectronicFenceEngine::pointInPolygonNormalized({ 25, 50 }, 100, 100, zone.polygon_norm));

        assert(engine.update({ track(1, 10, 50) }, 100, 100, 0).empty());
        assert(engine.update({ track(1, 50, 50) }, 100, 100, 100).empty());
        auto events = engine.update({ track(1, 50, 50) }, 100, 100, 200);
        assert(events.size() == 1 && events.front().event_type == "ZONE_ENTER");
        events = engine.update({ track(1, 50, 50) }, 100, 100, 800);
        assert(events.size() == 1 && events.front().event_type == "ZONE_DWELL");
        assert(engine.update({ track(1, 90, 50) }, 100, 100, 900).empty());
        events = engine.update({ track(1, 90, 50) }, 100, 100, 1000);
        assert(events.size() == 1 && events.front().event_type == "ZONE_EXIT");
        const auto statuses = engine.statuses(1000);
        assert(statuses.size() == 1 && !statuses.front().inside);
    }

    void testPhase2TrackingEnhancementAndAnalytics() {
        PeopleFlowTrackerSection tracker_config;
        tracker_config.min_hits = 1;
        tracker_config.max_age_frames = 3;
        tracker_config.match_iou_threshold = 0.0;
        tracker_config.center_distance_gate_norm = 0.5;
        tracker_config.use_alpha_beta_filter = true;
        tracker_config.motion_alpha = 0.85;
        tracker_config.motion_beta = 0.20;
        tracker_config.max_prediction_ms = 1000;
        PersonTracker tracker_engine(tracker_config);

        auto tracked = tracker_engine.update({ detection(10, 50, 0, true) }, 200, 100, 0);
        assert(tracked.size() == 1);
        const std::int64_t stable_id = tracked.front().track_id;
        tracked = tracker_engine.update({ detection(20, 50, 100, true) }, 200, 100, 100);
        assert(tracked.size() == 1 && tracked.front().track_id == stable_id);
        // A low-confidence detection is recovered by the existing second
        // association pass and updates the same time-aware motion state.
        tracked = tracker_engine.update({ detection(45, 50, 350, false) }, 200, 100, 350);
        assert(tracked.size() == 1 && tracked.front().track_id == stable_id);
        assert(tracked.front().matched_this_frame);
        const double matched_center_x = tracked.front().bbox.center().x;
        tracked = tracker_engine.update({}, 200, 100, 550);
        assert(tracked.size() == 1 && tracked.front().track_id == stable_id);
        assert(tracked.front().missed == 1 && !tracked.front().matched_this_frame);
        assert(tracked.front().bbox.center().x > matched_center_x);
        assert(tracked.front().motion_velocity_x_px_s > 0.0);

        TrackAnalyticsConfig config;
        config.history_window_ms = 10000;
        config.loiter_window_ms = 3000;
        config.loiter_min_path_px = 60;
        config.loiter_max_displacement_px = 10;
        config.stationary_speed_px_s = 5;
        TrackAnalyticsEngine engine(config);

        const std::vector<PfPoint> loop{
            { 10, 10 }, { 30, 10 }, { 30, 30 }, { 10, 30 }, { 10, 10 }
        };
        std::vector<TrackAnalyticsSnapshot> snapshots;
        for (std::size_t index = 0; index < loop.size(); ++index) {
            snapshots = engine.update(
                { track(2, loop[index].x, loop[index].y) },
                static_cast<long long>(index) * 1000);
        }
        assert(snapshots.size() == 1);
        assert(std::abs(snapshots.front().cumulative_distance_px - 80.0) < 1e-6);
        assert(snapshots.front().displacement_px < 1e-6);
        assert(snapshots.front().loitering);

        engine.update({ track(2, 10, 10) }, 5000);
        snapshots = engine.update({ track(2, 10, 10) }, 6000);
        assert(snapshots.front().stationary);
        assert(snapshots.front().stationary_ms >= 1000);
    }

    void testPhase3PoseRules() {
        PoseActionConfig config;
        config.confirm_frames = 2;
        config.release_frames = 2;
        config.cooldown_ms = 0;
        config.running_speed_px_s = 100;
        PoseActionEngine engine(config, "session", "camera");

        TrackAnalyticsSnapshot still;
        still.track_id = 3;
        std::vector<SecurityEvent> all;
        auto append = [&](std::vector<SecurityEvent> events) {
            all.insert(all.end(), events.begin(), events.end());
        };
        append(engine.update({ standingPose(3, 0, true) }, { still }, 0));
        append(engine.update({ standingPose(3, 100, true) }, { still }, 100));
        assert(hasType(all, "HANDS_UP_START"));
        append(engine.update({ standingPose(3, 200, false) }, { still }, 200));
        append(engine.update({ standingPose(3, 300, false) }, { still }, 300));
        assert(hasType(all, "HANDS_UP_END"));

        append(engine.update({ fallenPose(4, 400) }, {}, 400));
        append(engine.update({ fallenPose(4, 500) }, {}, 500));
        assert(hasType(all, "FALL_START"));

        TrackAnalyticsSnapshot fast;
        fast.track_id = 5;
        fast.instantaneous_speed_px_s = 250;
        append(engine.update({ standingPose(5, 600, false) }, { fast }, 600));
        append(engine.update({ standingPose(5, 700, false) }, { fast }, 700));
        assert(hasType(all, "RUNNING_START"));
    }

    void testPhase4TemporalClassifierContract() {
        TemporalActionConfig config;
        config.window_size = 4;
        config.min_samples = 2;
        config.confirm_windows = 2;
        config.release_windows = 2;
        config.cooldown_ms = 0;
        config.start_threshold = 0.70;
        config.end_threshold = 0.30;
        auto classifier = std::make_unique<FeatureThresholdTemporalClassifier>(
            "RAPID_INTERACTION_DEMO", 0, 1.0);
        TemporalActionEngine engine(config, std::move(classifier), "session", "camera");
        assert(engine.ready());
        assert(engine.classifierName() == "feature-threshold-demo");

        std::vector<SecurityEvent> all;
        auto update = [&](long long timestamp, double score) {
            auto events = engine.update(8, { timestamp, { score } });
            all.insert(all.end(), events.begin(), events.end());
        };
        update(0, 0.9);
        update(100, 0.9);
        update(200, 0.9);
        assert(hasType(all, "RAPID_INTERACTION_DEMO_START"));
        for (int index = 0; index < 7; ++index) update(300 + index * 100, 0.0);
        assert(hasType(all, "RAPID_INTERACTION_DEMO_END"));
        for (const SecurityEvent& event : all) assert(event.demo_classifier);
    }

    void testSingleFrameFourStageLivePipeline() {
        PeopleFlowSecuritySection config;
        config.enabled = true;
        config.zones.front().polygon_norm = {
            { 0.20, 0.20 }, { 0.80, 0.20 }, { 0.80, 0.90 }, { 0.20, 0.90 }
        };
        config.zones.front().enter_confirm_frames = 1;
        config.zones.front().cooldown_ms = 0;
        config.pose.confirm_frames = 1;
        config.pose.release_frames = 1;
        config.pose.cooldown_ms = 0;
        config.temporal.window_size = 4;
        config.temporal.min_samples = 2;
        config.temporal.confirm_windows = 1;
        config.temporal.release_windows = 1;
        config.temporal.cooldown_ms = 0;
        config.temporal.start_threshold = 0.5;
        config.temporal.end_threshold = 0.2;
        config.temporal_motion_speed_px_s = 100.0;
        SecurityLivePipeline pipeline(config, "live_session", "live_camera");

        PersonTrack first = track(11, 200, 500);
        auto frame = pipeline.update({ first }, poseOutput(200, true), { 640, 640 }, 0);
        assert(hasType(frame.new_events, "ZONE_ENTER"));
        assert(hasType(frame.new_events, "HANDS_UP_START"));
        assert(frame.poses.size() == 1 && frame.poses.front().track_id == 11);

        PersonTrack second = track(11, 240, 500);
        frame = pipeline.update({ second }, poseOutput(240, true), { 640, 640 }, 100);
        assert(hasType(frame.new_events, "RAPID_MOTION_DEMO_START"));
        assert(!frame.active_pose_actions.empty());
        assert(!frame.active_temporal_actions.empty());

        SecurityOverlayRenderer renderer(config);
        const cv::Mat blank(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
        const cv::Mat rendered = renderer.render(blank, frame);
        assert(!rendered.empty());
        assert(cv::norm(rendered, blank, cv::NORM_L1) > 0.0);
    }

}  // namespace

int main() {
    testPhase1ElectronicFence();
    testPhase2TrackingEnhancementAndAnalytics();
    testPhase3PoseRules();
    testPhase4TemporalClassifierContract();
    testSingleFrameFourStageLivePipeline();
    std::cout << "Phase 24 security analytics stages 1-4 passed\n";
    return 0;
}
