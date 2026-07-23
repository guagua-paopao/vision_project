#include <chrono>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

#include "business/camera_task_repository.h"
#include "business/postgres_client.h"
#include "postgres_test_guard.h"
#include "server/callback_delivery_worker.h"

namespace {

using namespace yolo11_server;
using json = nlohmann::json;

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

void setEnvironment(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

void unsetEnvironment(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

std::string hmacHex(const std::string& secret, const std::string& value) {
    unsigned int size = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size(),
        digest,
        &size);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<int>(digest[index]);
    }
    return output.str();
}

class FakeTransport final : public ICallbackHttpTransport {
public:
    CallbackHttpResponse post(const CallbackHttpRequest& request) noexcept override {
        requests.push_back(request);
        if (responses.empty()) {
            CallbackHttpResponse result;
            result.error_code = "FAKE_RESPONSE_MISSING";
            return result;
        }
        auto result = responses.front();
        responses.pop_front();
        return result;
    }

    std::deque<CallbackHttpResponse> responses;
    std::vector<CallbackHttpRequest> requests;
};

struct OutboxState {
    std::string status;
    int attempt = 0;
    long long next_attempt_at_ms = 0;
    long long delivered_at_ms = 0;
    int http_status = 0;
    std::string error_code;
    std::string response_hash;
};

OutboxState readOutbox(PostgresConnection& database, const std::string& event_id) {
    std::string error;
    auto statement = database.prepare(
        "SELECT status,attempt,next_attempt_at_ms,delivered_at_ms,last_http_status,"
        "last_error_code,response_body_hash FROM callback_outbox WHERE event_id=?;",
        error);
    require(statement != nullptr, "outbox query must prepare: " + error);
    statement->bindText(1, event_id);
    require(statement->step() == PG_STEP_ROW, "outbox row must exist");
    OutboxState result;
    result.status = statement->columnText(0);
    result.attempt = statement->columnInt(1);
    result.next_attempt_at_ms = statement->columnInt64(2);
    result.delivered_at_ms = statement->columnInt64(3);
    result.http_status = statement->columnInt(4);
    result.error_code = statement->columnText(5);
    result.response_hash = statement->columnText(6);
    return result;
}

SecurityAlertEventRecord alert(
    const std::string& event_id,
    const std::string& fingerprint,
    long long created_at_ms
) {
    SecurityAlertEventRecord result;
    result.event_id = event_id;
    result.task_id = "callback_camera";
    result.run_id = "callback_run";
    result.camera_profile = "entry_camera_01";
    result.event_type = "PEOPLE_FLOW_IN";
    result.category = "people_flow";
    result.severity = 2;
    result.confidence = 0.91;
    result.track_id = 42;
    result.occurred_at_ms = created_at_ms;
    result.algorithm_profile = "security_default";
    result.model_name = "pose";
    result.config_version = "callback-test-v1";
    result.payload_json = R"({"direction":"IN","line_id":"entrance"})";
    result.fingerprint = fingerprint;
    result.created_at_ms = created_at_ms;
    return result;
}

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;

    constexpr const char* kUrlEnv = "YOLO11_TEST_CALLBACK_URL";
    constexpr const char* kSecretEnv = "YOLO11_TEST_CALLBACK_SECRET";
    const std::string secret = "callback-test-secret-32-bytes-long";
    setEnvironment(kUrlEnv, "http://127.0.0.1:1/algorithm-alerts");
    setEnvironment(kSecretEnv, secret);

    AppConfig config;
    config.camera_tasks.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.callbacks.enabled = true;
    config.callbacks.poll_interval_ms = 10000;
    config.callbacks.request_timeout_ms = 1000;
    config.callbacks.lease_timeout_ms = 2000;
    config.callbacks.max_attempts = 3;
    config.callbacks.initial_backoff_ms = 100;
    config.callbacks.max_backoff_ms = 400;
    config.callbacks.request_body_limit_bytes = 1024;
    config.callbacks.response_body_limit_bytes = 128;
    CallbackProfileSection profile;
    profile.url_env = kUrlEnv;
    profile.hmac_secret_env = kSecretEnv;
    profile.allow_insecure_http = true;
    config.callbacks.profiles["backend_primary"] = profile;

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
    const long long stamp = nowMs();
    CameraTaskDefinition task;
    task.task_id = "callback_camera";
    task.name = "Callback delivery test";
    task.camera_profile = "entry_camera_01";
    task.enabled = true;
    task.desired_state = "running";
    task.analysis_enabled = true;
    task.target_infer_fps = 5.0;
    task.algorithm_profile = "security_default";
    task.algorithms = { "people_flow" };
    task.callback_profile = "backend_primary";
    task.version = 1;
    task.created_at_ms = stamp;
    task.updated_at_ms = stamp;
    std::string code;
    require(repository->createTask(task, code, error),
        "callback test task must persist: " + error);
    CameraTaskRunRecord run;
    run.run_id = "callback_run";
    run.task_id = task.task_id;
    run.definition_version = 1;
    run.definition_json = "{}";
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = stamp;
    run.last_update_ms = stamp;
    require(repository->createRun(run, code, error),
        "callback test run must persist: " + error);

    auto transport = std::make_shared<FakeTransport>();
    transport->responses = {
        { true, 500, "retry-body", false, {} },
        { true, 204, {}, false, {} },
        { true, 400, "terminal-body", false, {} },
        { false, 0, {}, false, "WINHTTP_SEND_FAILED" },
        { false, 0, {}, false, "WINHTTP_SEND_FAILED" },
        { false, 0, {}, false, "WINHTTP_SEND_FAILED" },
        { true, 204, {}, false, {} }
    };
    CallbackDeliveryWorker worker(config, repository, transport);

    const auto retry_alert = alert("ae_callback_retry", "fp_callback_retry", stamp + 1);
    require(repository->insertAlert(retry_alert, "backend_primary", code, error),
        "retry alert/outbox must persist: " + error);
    bool found = false;
    const long long retry_first = stamp + 1000;
    require(worker.processOneAt(retry_first, found, error) && found,
        "retryable HTTP attempt must be processed: " + error);
    auto state = readOutbox(database, retry_alert.event_id);
    require(state.status == "retry" && state.attempt == 1 &&
            state.next_attempt_at_ms == retry_first + 100 &&
            state.http_status == 500 &&
            state.error_code == "CALLBACK_HTTP_RETRYABLE",
        "HTTP 500 must schedule the first exponential retry");
    require(worker.processOneAt(retry_first + 99, found, error) && !found,
        "outbox must not be reclaimed before retry due time");
    require(worker.processOneAt(retry_first + 100, found, error) && found,
        "due retry must be claimed: " + error);
    state = readOutbox(database, retry_alert.event_id);
    require(state.status == "delivered" && state.attempt == 2 &&
            state.http_status == 204 &&
            state.delivered_at_ms == retry_first + 100,
        "any 2xx must terminally mark delivery successful");

    require(transport->requests.size() == 2,
        "retry flow must make exactly two HTTP attempts");
    const auto& first_request = transport->requests.front();
    const auto body = json::parse(first_request.body);
    require(body["schema_version"] == "1.0" &&
            body["event_kind"] == "algorithm_alert" &&
            body["event_id"] == retry_alert.event_id &&
            body["camera_id"] == task.task_id &&
            body["algorithm"]["profile"] == task.algorithm_profile &&
            !body.contains("delivery"),
        "callback body must match the frozen alert schema without delivery internals");
    require(first_request.headers.at("Idempotency-Key") == retry_alert.event_id &&
            first_request.headers.at("X-Event-Id") == retry_alert.event_id &&
            first_request.headers.at("X-Signature-Version") == "1" &&
            first_request.headers.at("X-Timestamp") ==
                std::to_string(retry_first) &&
            first_request.headers.at("X-Signature") ==
                hmacHex(
                    secret,
                    std::to_string(retry_first) + "\n" + first_request.body),
        "callback headers and HMAC-SHA256 input must match the P0 contract");

    const auto terminal_alert =
        alert("ae_callback_terminal", "fp_callback_terminal", stamp + 2);
    require(repository->insertAlert(terminal_alert, "backend_primary", code, error),
        "terminal alert/outbox must persist: " + error);
    const long long terminal_time = stamp + 2000;
    require(worker.processOneAt(terminal_time, found, error) && found,
        "terminal HTTP attempt must process: " + error);
    state = readOutbox(database, terminal_alert.event_id);
    require(state.status == "dead_letter" && state.attempt == 1 &&
            state.http_status == 400 &&
            state.error_code == "CALLBACK_HTTP_TERMINAL_4XX" &&
            state.response_hash.size() == 64,
        "non-retryable 4xx must enter dead letter with body hash only");

    const auto exhausted_alert =
        alert("ae_callback_exhausted", "fp_callback_exhausted", stamp + 3);
    require(repository->insertAlert(exhausted_alert, "backend_primary", code, error),
        "exhaustion alert/outbox must persist: " + error);
    const long long exhausted_first = stamp + 3000;
    require(worker.processOneAt(exhausted_first, found, error) && found,
        "first transport failure must process");
    state = readOutbox(database, exhausted_alert.event_id);
    require(state.status == "retry" && state.attempt == 1 &&
            state.next_attempt_at_ms == exhausted_first + 100 &&
            state.error_code == "WINHTTP_SEND_FAILED",
        "transport failure must preserve a stable nonsecret error code");
    require(worker.processOneAt(exhausted_first + 100, found, error) && found,
        "second transport failure must process");
    state = readOutbox(database, exhausted_alert.event_id);
    require(state.status == "retry" && state.attempt == 2 &&
            state.next_attempt_at_ms == exhausted_first + 300,
        "second retry must double the delay");
    require(worker.processOneAt(exhausted_first + 300, found, error) && found,
        "final transport failure must process");
    state = readOutbox(database, exhausted_alert.event_id);
    require(state.status == "dead_letter" && state.attempt == 3 &&
            state.error_code == "CALLBACK_MAX_ATTEMPTS_EXHAUSTED",
        "retryable failures must dead-letter at max_attempts");

    const auto stale_alert =
        alert("ae_callback_stale_lease", "fp_callback_stale_lease", stamp + 4);
    require(repository->insertAlert(stale_alert, "backend_primary", code, error),
        "stale-lease alert/outbox must persist: " + error);
    CallbackOutboxRecord claimed;
    const long long stale_first = stamp + 4000;
    require(repository->claimDueCallback(
            stale_first, config.callbacks.lease_timeout_ms, claimed, found, error) &&
            found && claimed.attempt == 1,
        "first consumer must claim a lease: " + error);
    require(worker.processOneAt(stale_first + 1999, found, error) && !found,
        "another consumer must not steal a live delivery lease");
    require(worker.processOneAt(stale_first + 2000, found, error) && found,
        "expired delivering lease must be reclaimed after a simulated crash");
    state = readOutbox(database, stale_alert.event_id);
    require(state.status == "delivered" && state.attempt == 2,
        "stale lease recovery must fence attempt 1 and deliver attempt 2");
    require(!repository->markCallbackDelivered(
                claimed.outbox_id,
                1,
                stale_first + 2001,
                204,
                {},
                error) &&
            error == "callback delivery lease was lost",
        "late attempt 1 completion must be rejected by the attempt fencing token");

    const auto retired_alert =
        alert("ae_callback_retired", "fp_callback_retired", stamp + 5);
    require(repository->insertAlert(retired_alert, "retired_profile", code, error),
        "retired-profile alert/outbox must persist: " + error);
    require(worker.processOneAt(stamp + 5000, found, error) && found,
        "retired profile record must be consumed");
    state = readOutbox(database, retired_alert.event_id);
    require(state.status == "dead_letter" &&
            state.error_code == "CALLBACK_PROFILE_NOT_CONFIGURED",
        "removed callback profile must fail closed without an HTTP request");

    auto oversized_alert =
        alert("ae_callback_oversized", "fp_callback_oversized", stamp + 6);
    oversized_alert.payload_json =
        json({ { "blob", std::string(1500, 'x') } }).dump();
    require(repository->insertAlert(
            oversized_alert, "backend_primary", code, error),
        "oversized callback alert/outbox must persist for policy testing: " + error);
    require(worker.processOneAt(stamp + 5500, found, error) && found,
        "oversized callback record must be consumed");
    state = readOutbox(database, oversized_alert.event_id);
    require(state.status == "dead_letter" &&
            state.error_code == "CALLBACK_PAYLOAD_TOO_LARGE" &&
            transport->requests.size() == 7,
        "oversized callback payload must dead-letter without reaching the network");

    SecurityAlertEventRecord loaded;
    bool alert_found = false;
    require(repository->getAlert(
            retry_alert.event_id, loaded, alert_found, error) &&
            alert_found && loaded.delivery_status == "delivered",
        "alert query must expose the terminal outbox delivery status");
    const auto metrics = worker.snapshot();
    require(metrics.claimed == 9 && metrics.delivered == 2 &&
            metrics.retries == 3 && metrics.dead_letters == 4 &&
            metrics.transport_failures == 3,
        "callback metrics must expose delivery, retry, dead-letter, and transport counts");

    unsetEnvironment(kSecretEnv);
    auto missing_secret_transport = std::make_shared<FakeTransport>();
    CallbackDeliveryWorker missing_secret_worker(
        config, repository, missing_secret_transport);
    require(!missing_secret_worker.processOneAt(
                stamp + 6000, found, error) &&
            error.find("secret") != std::string::npos &&
            error.find("callback-test-secret") == std::string::npos,
        "callback startup must fail closed without echoing a missing secret value");
    unsetEnvironment(kUrlEnv);

    std::cout << "Durable callback delivery worker tests passed\n";
    return 0;
}
