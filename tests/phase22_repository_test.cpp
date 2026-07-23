#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "business/postgres_client.h"
#include "business/people_flow_repository.h"
#include "postgres_test_guard.h"

namespace {

    std::string pathToUtf8(const std::filesystem::path& path) {
        const auto encoded = path.generic_u8string();
        return std::string(encoded.begin(), encoded.end());
    }

    void require(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    yolo11_server::CrossingEvent makeEvent(int index, long long base_time_ms) {
        yolo11_server::CrossingEvent event;
        event.event_id = "pf_repo_test_line_" + std::to_string(index);
        event.session_id = "pf_repo_test_session";
        event.camera_id = "entry_camera_01";
        event.line_id = "entrance_line_01";
        event.track_id = index + 1;
        event.direction = index % 2 == 0 ? "IN" : "OUT";
        event.event_time_ms = base_time_ms + index * 1000;
        event.confidence = 0.9;
        event.point_x_norm = 0.5;
        event.point_y_norm = 0.5;
        event.config_version = "test-v1";
        return event;
    }

}  // namespace

int main() {
    using namespace yolo11_server;
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;

    PeopleFlowSection config;
    config.storage.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.storage.writer_queue_capacity = 100;
    config.storage.writer_batch_size = 20;
    config.storage.writer_flush_interval_ms = 25;
    std::string reset_error;
    PostgresConnection database;
    require(database.openFromEnvironment(config.storage.postgres_dsn_env, reset_error),
        "test PostgreSQL connection must open: " + reset_error);
    require(database.exec(
        "DROP TABLE IF EXISTS pf_calibration_audit,pf_aggregates_minute,pf_crossing_events,pf_sessions,schema_version CASCADE;",
        reset_error), "test PostgreSQL schema reset must succeed: " + reset_error);

    const long long base_time_ms = 1710000000000LL;
    {
        PeopleFlowRepository repository(config);
        std::string error;
    const bool writer_started = repository.start(true, error);
    require(writer_started, "writer repository must start: " + error);

        PeopleFlowSessionRecord start;
        start.session_id = "pf_repo_test_session";
        start.camera_id = "entry_camera_01";
        start.status = "running";
        start.start_time_ms = base_time_ms;
        start.initial_occupancy = 3;
        start.final_occupancy = 3;
        start.config_version = "test-v1";
        require(repository.enqueueSessionStart(start), "session start must enter the writer queue");

        for (int index = 0; index < 10; ++index) {
            require(repository.enqueueEvent(makeEvent(index, base_time_ms)), "event must enter the writer queue");
        }
        require(repository.enqueueEvent(makeEvent(0, base_time_ms)), "duplicate event must enter the writer queue");

        PeopleFlowSessionRecord finish = start;
        finish.status = "stopped";
        finish.stop_time_ms = base_time_ms + 20000;
        finish.in_count = 5;
        finish.out_count = 5;
        finish.final_occupancy = 3;
        finish.stop_reason = "test";
        require(repository.enqueueSessionFinish(finish), "session finish must enter the writer queue");

        bool persisted = false;
        // A freshly started disposable PostgreSQL container may need several
        // seconds for its first schema/connection cycle on Windows CI.
        for (int attempt = 0; attempt < 200; ++attempt) {
            PeopleFlowSessionRecord stored;
            bool found = false;
            error.clear();
            if (repository.getSession(finish.session_id, stored, found, error) &&
                found && stored.status == "stopped") {
                persisted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        const auto persistence_health = repository.health();
        require(persisted, "asynchronous session/event batch must be committed: " +
            persistence_health.last_error);
        repository.stop();

        const PeopleFlowRepositoryHealth health = repository.health();
        require(health.written_events == 10, "exactly ten unique events must be written");
        require(health.duplicate_events == 1, "duplicate event_id must be ignored once");
        require(health.failed_batches == 0, "normal repository test must not fail a batch");
    }

    {
        PeopleFlowRepository query_repository(config);
        std::string error;
        require(query_repository.start(false, error), "query repository must reopen after restart: " + error);

        std::vector<CrossingEvent> events;
        require(query_repository.queryEvents(
            "entry_camera_01", "", base_time_ms, base_time_ms + 60000,
            100, 0, events, error), "event query must succeed: " + error);
        require(events.size() == 10, "restart query must return ten unique events");

        std::vector<CrossingEvent> incoming;
        require(query_repository.queryEvents(
            "entry_camera_01", "IN", base_time_ms, base_time_ms + 60000,
            100, 0, incoming, error), "direction query must succeed: " + error);
        require(incoming.size() == 5, "IN filter must return five events");

        std::vector<PeopleFlowSummaryBucket> summary;
        require(query_repository.querySummary(
            "entry_camera_01", base_time_ms, base_time_ms + 60000,
            "minute", summary, error), "summary query must succeed: " + error);
        require(summary.size() == 1, "ten events in one minute must produce one aggregate");
        require(summary[0].in_count == 5 && summary[0].out_count == 5,
            "minute aggregate must preserve IN and OUT counts");

        PeopleFlowSessionRecord session;
        bool found = false;
        require(query_repository.getSession("pf_repo_test_session", session, found, error),
            "historical session query must succeed: " + error);
        require(found && session.consistency_ok, "session must exist and pass event/count consistency");
        require(session.in_count == 5 && session.out_count == 5 && session.final_occupancy == 3,
            "final session counters must be restored after restart");

        PeopleFlowSessionRecord recovered;
        recovered.session_id = "pf_repo_recovered_session";
        recovered.camera_id = "entry_camera_01";
        recovered.status = "failed";
        recovered.start_time_ms = base_time_ms;
        recovered.stop_time_ms = base_time_ms + 40000;
        recovered.initial_occupancy = 2;
        recovered.final_occupancy = 2;
        recovered.config_version = "test-v1";
        recovered.stop_reason = "worker_heartbeat_stale";
        recovered.error = "WORKER_HEARTBEAT_STALE";
        require(query_repository.finalizeSessionSync(recovered, error),
            "stale-session synchronous finalization must succeed: " + error);
        PeopleFlowSessionRecord recovered_result;
        found = false;
        require(query_repository.getSession(recovered.session_id, recovered_result, found, error) && found,
            "synchronously recovered session must be queryable");
        require(recovered_result.status == "failed" && recovered_result.consistency_ok,
            "recovered session must be failed and internally consistent");

        PeopleFlowCalibrationRecord audit;
        audit.camera_id = "entry_camera_01";
        audit.session_id = "pf_repo_test_session";
        audit.before_occupancy = 3;
        audit.after_occupancy = 7;
        audit.reason = "phase22 test";
        audit.operator_name = "tester";
        audit.timestamp_ms = base_time_ms + 30000;
        require(query_repository.writeCalibration(audit, error), "calibration audit insert must succeed: " + error);
        query_repository.stop();
    }

    {
        PeopleFlowSection invalid = config;
        invalid.storage.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN_MISSING";
        PeopleFlowRepository degraded(invalid);
        std::string error;
        require(!degraded.start(false, error), "unwritable storage must fail its initial schema check");
        const PeopleFlowRepositoryHealth health = degraded.health();
        require(health.degraded && !health.last_error.empty(), "storage failure must be observable as degraded");
        degraded.stop();
    }

    std::cout << "PostgreSQL People Flow repository tests passed\n";
    return 0;
}
