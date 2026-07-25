#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

#include "business/line_crossing_counter.h"
#include "business/person_detector_adapter.h"
#include "business/person_tracker.h"
#include "business/camera_task_repository.h"
#include "business/postgres_client.h"
#include "config.h"
#include "postgres_test_guard.h"
#include "server/camera_algorithm_processor.h"
#include "server/camera_task_api_control.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

Detection modelDetection(
    float x,
    float y,
    float width,
    float height,
    int frame_width = 100,
    int frame_height = 100
) {
    Detection result{};
    result.class_id = 0;
    result.conf = 0.95f;
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

CameraInferenceResult inference(
    unsigned long long sequence,
    long long timestamp_ms,
    Detection detection
) {
    auto frame = std::make_shared<FrameEnvelope>();
    frame->image = cv::Mat(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    frame->sequence = sequence;
    frame->capture_time_ms = timestamp_ms;
    CameraInferenceResult result;
    result.worker_id = 0;
    result.job.task_id = "camera_algorithm_test";
    result.job.run_id = "run_algorithm_test";
    result.job.camera_profile = "entry_camera_01";
    result.job.source_sequence = sequence;
    result.job.capture_time_ms = timestamp_ms;
    result.job.algorithm_profile = "security_default";
    result.job.algorithms = { "people_flow" };
    result.job.callback_profile = "backend_primary";
    result.job.analysis_config_version = "runspec-test-v3";
    result.job.target_infer_fps = 10.0;
    result.job.initial_occupancy = 4;
    result.job.snapshot_fps = 10;
    result.job.algorithm_parameters_json = R"({"line_id":"main"})";
    result.job.capture_fps = 25.0;
    result.job.source_fps = 25.0;
    result.job.latest_frame_age_ms = 10;
    result.job.frame = std::move(frame);
    result.output.model_type = "pose";
    result.output.detections = { detection };
    return result;
}

class CapturingAnalysisStatusSink final : public ICameraAnalysisStatusSink {
public:
    bool updateAnalysisStatus(
        const CameraTaskRunHotStatus& status,
        std::string& error) override {
        error.clear();
        latest = status;
        ++updates;
        return true;
    }
    CameraTaskRunHotStatus latest;
    int updates = 0;
};

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;
    const long long stamp = nowMs();

    AppConfig config;
    config.camera_tasks.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    const auto output_root = std::filesystem::temp_directory_path() /
        ("camera_r3_analysis_" + std::to_string(stamp));
    config.camera_tasks.output_dir = pathUtf8(output_root);
    config.people_flow.config_version = "algorithm-test-v1";
    config.people_flow.warmup_frames_after_reconnect = 1;
    config.people_flow.roi.enabled = false;
    config.people_flow.person.min_width_px = 1;
    config.people_flow.person.min_height_px = 1;
    config.people_flow.person.max_aspect_ratio = 10.0;
    config.people_flow.tracker.min_hits = 1;
    config.people_flow.tracker.max_age_frames = 5;
    config.people_flow.tracker.match_iou_threshold = 0.0;
    config.people_flow.tracker.center_distance_gate_norm = 1.0;
    config.people_flow.counting.line_a_norm = { 0.1, 0.5 };
    config.people_flow.counting.line_b_norm = { 0.9, 0.5 };
    config.people_flow.counting.hysteresis_px = 2.0;
    config.people_flow.counting.min_hits_for_count = 1;
    config.people_flow.counting.min_crossing_interval_ms = 0;
    config.people_flow.counting.max_crossing_gap_ms = 1000;
    config.people_flow.security.enabled = false;

    PostgresConnection database;
    std::string error;
    require(database.openFromEnvironment(config.camera_tasks.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS callback_outbox,security_alert_events,camera_idempotency_keys,"
        "camera_frames,camera_run_analysis_results,camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
        error), "test PostgreSQL schema reset must succeed: " + error);

    auto repository = std::make_shared<CameraTaskRepository>(config.camera_tasks);
    require(repository->initialize(error), "camera repository must initialize: " + error);
    CameraTaskDefinition task;
    task.task_id = "camera_algorithm_test";
    task.name = "Algorithm processor test";
    task.camera_profile = "entry_camera_01";
    task.enabled = true;
    task.desired_state = "running";
    task.analysis_enabled = true;
    task.target_infer_fps = 10.0;
    task.algorithm_profile = "security_default";
    task.algorithms = { "people_flow" };
    task.callback_profile = "backend_primary";
    task.version = 1;
    task.created_at_ms = stamp;
    task.updated_at_ms = stamp;
    std::string code;
    require(repository->createTask(task, code, error), "algorithm task must persist: " + error);
    CameraTaskRunRecord run;
    run.run_id = "run_algorithm_test";
    run.task_id = task.task_id;
    run.definition_version = 1;
    run.definition_json = "{}";
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = stamp;
    run.last_update_ms = stamp;
    require(repository->createRun(run, code, error), "algorithm run must persist: " + error);

    auto status_sink = std::make_shared<CapturingAnalysisStatusSink>();
    CameraAlgorithmProcessor processor(config, repository, status_sink);
    require(processor.start(error), "algorithm processor must start: " + error);
    auto first = inference(1, stamp + 100, modelDetection(40, 40, 20, 30));
    auto second = inference(2, stamp + 200, modelDetection(40, 40, 20, 30));
    auto third = inference(3, stamp + 300, modelDetection(40, 0, 20, 30));

    PersonDetectorAdapter baseline_adapter(config.people_flow);
    PersonTracker baseline_tracker(config.people_flow.tracker);
    LineCrossingCounter baseline_counter(
        config.people_flow.counting,
        first.job.run_id,
        first.job.task_id,
        first.job.analysis_config_version,
        first.job.initial_occupancy);
    auto advance_baseline = [&](const CameraInferenceResult& value, bool warmup) {
        const auto detections = baseline_adapter.filter(
            value.output, value.job.frame->image.size(), value.job.capture_time_ms);
        baseline_tracker.update(
            detections,
            value.job.frame->image.cols,
            value.job.frame->image.rows,
            value.job.capture_time_ms);
        if (!warmup) {
            baseline_counter.update(
                baseline_tracker.confirmedTracks(),
                value.job.frame->image.cols,
                value.job.frame->image.rows,
                value.job.capture_time_ms);
        }
    };
    advance_baseline(first, true);
    advance_baseline(second, false);
    advance_baseline(third, false);

    require(processor.handle(first, error), "warmup analysis frame must process: " + error);
    require(processor.handle(second, error), "baseline analysis frame must process: " + error);
    require(processor.handle(third, error), "crossing analysis frame must process: " + error);

    std::vector<SecurityAlertEventRecord> alerts;
    require(repository->listAlerts(task.task_id, "PEOPLE_FLOW_IN", 1, 20, 0, alerts, error) &&
            alerts.size() == 1 &&
            alerts.front().run_id == run.run_id &&
            alerts.front().delivery_status == "pending",
        "line crossing must create one durable alert and pending callback outbox");
    CameraRunAnalysisResultRecord analysis;
    bool found = false;
    require(repository->getRunAnalysisResult(
            run.run_id, analysis, found, error) && found,
        "R3 analysis snapshot must be durable");
    const auto baseline_counts = baseline_counter.counts();
    const auto security = nlohmann::json::parse(analysis.security_state_json);
    require(analysis.initial_occupancy == first.job.initial_occupancy &&
            analysis.in_count == baseline_counts.in_count &&
            analysis.out_count == baseline_counts.out_count &&
            analysis.final_occupancy == baseline_counts.occupancy &&
            analysis.last_live_persons == baseline_counts.live_persons &&
            security["stages"].contains("phase1") &&
            security["stages"].contains("phase2") &&
            security["stages"].contains("phase3") &&
            security["stages"].contains("phase4"),
        "unified Camera output must match the deterministic legacy People Flow core");
    const auto annotated_path =
        output_root / std::filesystem::u8path(analysis.snapshot_relative_path);
    require(!analysis.snapshot_relative_path.empty() &&
            !cv::imread(annotated_path.string()).empty() &&
            !analysis.snapshot_degraded,
        "R3 must produce a readable annotated latest JPEG");
    require(status_sink->updates >= 3 &&
            status_sink->latest.analysis_config_version ==
                first.job.analysis_config_version &&
            status_sink->latest.in_count == baseline_counts.in_count &&
            status_sink->latest.occupancy == baseline_counts.occupancy,
        "R3 must publish a complete hot analysis snapshot");

    auto reconnect = inference(4, stamp + 400, modelDetection(40, 0, 20, 30));
    reconnect.job.reconnect_count = 1;
    require(processor.handle(reconnect, error),
        "first frame after reconnect must be accepted as warmup");
    alerts.clear();
    require(repository->listAlerts(task.task_id, "PEOPLE_FLOW_IN", 1, 20, 0, alerts, error) &&
            alerts.size() == 1 &&
            status_sink->latest.analysis_reconnect_count == 1 &&
            status_sink->latest.in_count == baseline_counts.in_count,
        "reconnect warmup must not create a false crossing or reset accumulated counts");
    const auto metrics = processor.snapshot();
    require(metrics.active_sessions == 1 && metrics.processed_frames == 4 &&
            metrics.persisted_alerts == 1 && metrics.failed_frames == 0,
        "algorithm processor metrics must expose session, frame, and alert counts");

    auto unsupported = inference(5, stamp + 500, modelDetection(40, 0, 20, 30));
    unsupported.job.task_id = "unsupported_camera";
    unsupported.job.run_id = "unsupported_run";
    unsupported.job.algorithms = { "ppe_detection" };
    require(!processor.handle(unsupported, error) &&
            error == "UNSUPPORTED_ALGORITHM:ppe_detection",
        "unsupported algorithms must fail explicitly instead of silently producing no result");

    processor.detachCamera(task.task_id, run.run_id);
    require(processor.snapshot().active_sessions == 0,
        "camera detach must release stateful analytics session");
    processor.stop();
    std::error_code cleanup_error;
    std::filesystem::remove_all(output_root, cleanup_error);
    std::cout << "Camera algorithm processor tests passed\n";
    return 0;
}
