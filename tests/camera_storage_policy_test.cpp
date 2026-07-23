#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "business/camera_frame_retention.h"
#include "business/frame_artifact_writer.h"
#include "business/postgres_client.h"
#include "postgres_test_guard.h"

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

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;
    const auto root = std::filesystem::temp_directory_path() /
        ("camera_storage_policy_" + std::to_string(nowMs()));
    CameraTasksSection config;
    config.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.output_dir = pathUtf8(root / "output");
    config.retention_batch_size = 100;
    config.storage.max_archive_bytes = 200;
    config.storage.high_watermark_percent = 50;
    config.storage.critical_watermark_percent = 90;
    config.storage.pressure_cleanup_batch_size = 100;
    std::string error;
    std::string code;
    PostgresConnection database;
    require(database.openFromEnvironment(config.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS camera_frames,camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
        error), "test PostgreSQL schema reset must succeed: " + error);
    auto repository = std::make_shared<CameraTaskRepository>(config);
    require(repository->initialize(error), "repository must initialize: " + error);

    const long long now = nowMs();
    CameraTaskDefinition task;
    task.task_id = "ct_policy";
    task.name = "storage policy";
    task.camera_profile = "entry_camera_01";
    task.output_mode = "both";
    task.retention_days = 3650;
    task.max_saved_frames = 100;
    task.created_at_ms = now;
    task.updated_at_ms = now;
    require(repository->createTask(task, code, error), "task must persist: " + error);
    CameraTaskRunRecord run;
    run.run_id = "cr_policy";
    run.task_id = task.task_id;
    run.definition_version = 1;
    run.definition_json = "{}";
    run.status = "stopped";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = now;
    run.stop_time_ms = now;
    run.last_update_ms = now;
    require(repository->createRun(run, code, error), "run must persist: " + error);

    const auto archive = root / "output" / "ct_policy" / "archive" / "cr_policy" /
        "2026" / "07" / "21";
    std::filesystem::create_directories(archive);
    for (int index = 0; index < 3; ++index) {
        const auto name = "frame_" + std::to_string(index) + ".jpg";
        std::ofstream file(archive / name, std::ios::binary);
        file << std::string(80, static_cast<char>('a' + index));
        file.close();
        CameraFrameArtifact frame;
        frame.frame_id = "cf_policy_" + std::to_string(index);
        frame.task_id = task.task_id;
        frame.run_id = run.run_id;
        frame.source_sequence = static_cast<unsigned long long>(index + 1);
        frame.capture_time_ms = now + index;
        frame.save_time_ms = now + index;
        frame.relative_path = "ct_policy/archive/cr_policy/2026/07/21/" + name;
        frame.width = 10;
        frame.height = 10;
        frame.size_bytes = 80;
        require(repository->insertFrame(frame, error), "frame metadata must persist: " + error);
    }
    const auto latest = root / "output" / "ct_policy" / "latest.jpg";
    {
        std::ofstream file(latest, std::ios::binary);
        file << "latest-must-survive";
    }

    CameraFrameRetentionSweeper retention(config, repository);
    require(retention.start(error), "retention must start: " + error);
    int deleted = 0;
    require(retention.sweepOnce(now, deleted, error), "pressure sweep must succeed: " + error);
    retention.stop();
    CameraTaskRepositoryStats stats;
    require(repository->stats(stats, error), "storage stats must remain queryable: " + error);
    require(deleted == 2 && stats.frames_total == 1 && stats.archive_bytes == 80,
        "high-watermark cleanup must delete oldest archives down to its target");
    require(std::filesystem::exists(latest), "pressure cleanup must never delete latest.jpg");

    CameraTasksSection critical = config;
    critical.storage.max_archive_bytes = 80;
    critical.storage.high_watermark_percent = 50;
    critical.storage.critical_watermark_percent = 90;
    FrameArtifactWriter writer(critical, repository);
    require(writer.start(error), "writer must start: " + error);
    auto frame = std::make_shared<FrameEnvelope>();
    frame->sequence = 99;
    frame->capture_time_ms = nowMs();
    frame->publish_time = std::chrono::steady_clock::now();
    frame->image = cv::Mat(64, 64, CV_8UC3, cv::Scalar(20, 100, 200)).clone();
    FrameArtifactJob job;
    job.task_id = task.task_id;
    job.run_id = run.run_id;
    job.output_mode = "archive";
    job.frame = frame;
    require(writer.enqueue(job), "critical-pressure job must enter the bounded writer queue");
    require(writer.waitForRunIdle(run.run_id, 5000), "critical-pressure job must finish promptly");
    writer.stop();
    const auto metrics = writer.metrics();
    require(metrics.failed_jobs == 1 && metrics.storage_pressure_rejections == 1,
        "writer must reject archive publication at the critical watermark");
    require(repository->stats(stats, error) && stats.frames_total == 1 && stats.archive_bytes == 80,
        "rejected publication must not add archive metadata");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::cout << "Camera storage quota, pressure cleanup, and admission tests passed\n";
    return 0;
}
