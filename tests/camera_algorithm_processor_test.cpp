#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "business/camera_task_repository.h"
#include "business/postgres_client.h"
#include "config.h"
#include "postgres_test_guard.h"
#include "server/camera_algorithm_processor.h"

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
    result.job.frame = std::move(frame);
    result.output.model_type = "pose";
    result.output.detections = { detection };
    return result;
}

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;
    const long long stamp = nowMs();

    AppConfig config;
    config.camera_tasks.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.people_flow.config_version = "algorithm-test-v1";
    config.people_flow.warmup_frames_after_reconnect = 0;
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
        "camera_frames,camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
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

    CameraAlgorithmProcessor processor(config, repository);
    require(processor.start(error), "algorithm processor must start: " + error);
    auto first = inference(1, stamp + 100, modelDetection(40, 40, 20, 30));
    auto second = inference(2, stamp + 200, modelDetection(40, 0, 20, 30));
    require(processor.handle(first, error), "first analysis frame must process: " + error);
    require(processor.handle(second, error), "crossing analysis frame must process: " + error);

    std::vector<SecurityAlertEventRecord> alerts;
    require(repository->listAlerts(task.task_id, "PEOPLE_FLOW_IN", 1, 20, 0, alerts, error) &&
            alerts.size() == 1 &&
            alerts.front().run_id == run.run_id &&
            alerts.front().delivery_status == "pending",
        "line crossing must create one durable alert and pending callback outbox");
    const auto metrics = processor.snapshot();
    require(metrics.active_sessions == 1 && metrics.processed_frames == 2 &&
            metrics.persisted_alerts == 1 && metrics.failed_frames == 0,
        "algorithm processor metrics must expose session, frame, and alert counts");

    auto unsupported = inference(3, stamp + 300, modelDetection(40, 0, 20, 30));
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
    std::cout << "Camera algorithm processor tests passed\n";
    return 0;
}
