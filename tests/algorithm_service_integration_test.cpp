#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <openssl/hmac.h>

#include "business/camera_task_repository.h"
#include "business/postgres_client.h"
#include "config.h"
#include "postgres_test_guard.h"
#include "server/callback_delivery_worker.h"
#include "server/camera_algorithm_processor.h"
#include "server/camera_task_http_controller.h"

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

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::map<std::string, std::string> parseHeaders(const std::string& request) {
    std::map<std::string, std::string> result;
    const auto header_end = request.find("\r\n\r\n");
    std::istringstream input(request.substr(0, header_end));
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        std::string value = line.substr(separator + 1);
        const auto start = value.find_first_not_of(" \t");
        value = start == std::string::npos ? "" : value.substr(start);
        result[lower(line.substr(0, separator))] = value;
    }
    return result;
}

std::size_t contentLength(const std::string& request) {
    const auto headers = parseHeaders(request);
    const auto found = headers.find("content-length");
    return found == headers.end()
        ? 0 : static_cast<std::size_t>(std::stoull(found->second));
}

std::string hmacSha256Hex(
    const std::string& secret,
    const std::string& value
) {
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

Detection modelDetection(float x, float y, float width, float height) {
    Detection result{};
    result.class_id = 0;
    result.conf = 0.95f;
    constexpr int frame_width = 100;
    constexpr int frame_height = 100;
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
    const CameraTaskCommand& command,
    unsigned long long sequence,
    long long timestamp_ms,
    Detection detection
) {
    auto frame = std::make_shared<FrameEnvelope>();
    frame->image = cv::Mat(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    frame->sequence = sequence;
    frame->capture_time_ms = timestamp_ms;
    CameraInferenceResult result;
    result.job.task_id = command.task_id;
    result.job.run_id = command.run_id;
    result.job.camera_profile = command.camera_profile;
    result.job.source_sequence = sequence;
    result.job.capture_time_ms = timestamp_ms;
    result.job.algorithm_profile = command.algorithm_profile;
    result.job.algorithms = command.algorithms;
    result.job.callback_profile = command.callback_profile;
    result.job.frame = std::move(frame);
    result.output.model_type = "pose";
    result.output.detections = { detection };
    return result;
}

crow::request apiRequest(const std::string& body = {}) {
    crow::request request;
    request.body = body;
    request.add_header("Authorization", "Bearer p5-integration-token");
    request.add_header("Idempotency-Key", "p5-integration-create");
    return request;
}

class FakeApiControl final : public ICameraTaskApiControl {
public:
    bool submitStart(
        const CameraTaskCommand& command,
        std::string& error) override {
        submitted.push_back(command);
        error.clear();
        return true;
    }
    bool requestStop(
        const std::string&,
        std::string& error) override {
        error.clear();
        return true;
    }
    bool getRunStatus(
        const std::string&,
        CameraTaskRunHotStatus& status,
        std::string& error) override {
        status = {};
        error.clear();
        return true;
    }
    bool getHubStatus(
        const std::string&,
        CameraHubHotStatus& status,
        std::string& error) override {
        status = {};
        error.clear();
        return true;
    }
    std::vector<CameraTaskCommand> submitted;
};

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;

    WSADATA winsock{};
    require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0,
        "Winsock must initialize");
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "callback listener must open");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(
            listener,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0,
        "callback listener must bind");
    require(listen(listener, 1) == 0, "callback listener must listen");
    int address_size = sizeof(address);
    require(getsockname(
            listener,
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == 0,
        "callback listener port must resolve");
    const int port = ntohs(address.sin_port);

    const std::string callback_secret =
        "p5-integration-hmac-secret-32bytes";
    const std::string callback_url =
        "http://127.0.0.1:" + std::to_string(port) +
        "/api/v1/algorithm-alerts";
    _putenv_s("P5_INTEGRATION_CALLBACK_URL", callback_url.c_str());
    _putenv_s("P5_INTEGRATION_CALLBACK_SECRET", callback_secret.c_str());

    const auto root = std::filesystem::temp_directory_path() /
        ("algorithm_service_p5_" + std::to_string(nowMs()));
    std::filesystem::create_directories(root);
    AppConfig config;
    config.camera_tasks.enabled = true;
    config.camera_tasks.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.camera_tasks.output_dir = pathUtf8(root / "output");
    config.stream.camera_profiles_path.clear();
    config.worker.worker_num = 1;
    config.analysis.enabled = true;
    config.callbacks.enabled = true;
    config.callbacks.max_attempts = 3;
    CallbackProfileSection callback_profile;
    callback_profile.enabled = true;
    callback_profile.url_env = "P5_INTEGRATION_CALLBACK_URL";
    callback_profile.hmac_secret_env = "P5_INTEGRATION_CALLBACK_SECRET";
    callback_profile.allow_insecure_http = true;
    config.callbacks.profiles["backend_primary"] = callback_profile;
    CameraProfile camera_profile;
    camera_profile.id = "entry_camera_01";
    camera_profile.url_env = "P5_INTEGRATION_RTSP_URI";
    camera_profile.enabled = true;
    config.camera_profiles[camera_profile.id] = camera_profile;

    config.people_flow.config_version = "p5-integration-v1";
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
    require(database.openFromEnvironment(
            config.camera_tasks.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS callback_outbox,security_alert_events,"
        "camera_idempotency_keys,camera_frames,camera_task_runs,camera_tasks,"
        "camera_schema_version CASCADE;",
        error),
        "P5 integration schema reset must succeed: " + error);

    auto repository =
        std::make_shared<CameraTaskRepository>(config.camera_tasks);
    auto control = std::make_shared<FakeApiControl>();
    CameraTaskHttpController controller(
        config,
        repository,
        control,
        "p5-integration-token");
    require(controller.initialize(error),
        "P5 HTTP controller must initialize: " + error);
    const std::string camera_id = "p5_integration_camera";
    auto response = controller.createTask(apiRequest(
        R"({"camera_id":"p5_integration_camera","name":"P5 integration","camera_profile":"entry_camera_01","desired_state":"running","frame_interval_ms":1000,"output_mode":"latest","analysis":{"enabled":true,"target_infer_fps":5.0,"algorithm_profile":"security_default","algorithms":["people_flow"]},"callback_profile":"backend_primary"})"));
    require(response.code == 202 && control->submitted.size() == 1,
        "HTTP create must persist one camera Run and submit one command");

    CameraAlgorithmProcessor processor(config, repository);
    require(processor.start(error), "algorithm processor must start: " + error);
    const long long stamp = nowMs();
    auto first = inference(
        control->submitted.front(),
        1,
        stamp + 100,
        modelDetection(40, 40, 20, 30));
    auto second = inference(
        control->submitted.front(),
        2,
        stamp + 200,
        modelDetection(40, 0, 20, 30));
    require(processor.handle(first, error) &&
            processor.handle(second, error),
        "synthetic line crossing must pass through the algorithm processor: " +
            error);
    std::vector<SecurityAlertEventRecord> alerts;
    require(repository->listAlerts(
            camera_id,
            "PEOPLE_FLOW_IN",
            1,
            20,
            0,
            alerts,
            error) &&
            alerts.size() == 1 &&
            alerts.front().delivery_status == "pending",
        "algorithm result must create one pending callback alert");
    const std::string event_id = alerts.front().event_id;

    std::string captured;
    std::thread receiver([&]() {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval wait{ 5, 0 };
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) return;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        char buffer[4096];
        std::size_t expected = 0;
        while (true) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            captured.append(buffer, static_cast<std::size_t>(count));
            const auto header_end = captured.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                expected = header_end + 4 + contentLength(captured);
                if (captured.size() >= expected) break;
            }
        }
        const std::string reply =
            "HTTP/1.1 204 No Content\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(client, reply.data(), static_cast<int>(reply.size()), 0);
        shutdown(client, SD_BOTH);
        closesocket(client);
    });

    CallbackDeliveryWorker callback(config, repository);
    bool found = false;
    require(callback.processOneAt(stamp + 1000, found, error) && found,
        "real callback delivery must process the pending outbox: " + error);
    receiver.join();
    closesocket(listener);

    const auto header_end = captured.find("\r\n\r\n");
    require(header_end != std::string::npos,
        "loopback receiver must capture a complete callback");
    const auto headers = parseHeaders(captured);
    const std::string body = captured.substr(header_end + 4);
    const auto payload = json::parse(body, nullptr, false);
    require(!payload.is_discarded() &&
            payload["event_id"] == event_id &&
            payload["camera_id"] == camera_id &&
            !payload.contains("delivery"),
        "real callback body must preserve alert_event.v1 semantics");
    require(headers.at("idempotency-key") == event_id &&
            headers.at("x-event-id") == event_id &&
            headers.at("x-signature-version") == "1" &&
            headers.at("x-signature") == hmacSha256Hex(
                callback_secret,
                headers.at("x-timestamp") + "\n" + body),
        "real callback must preserve idempotency and HMAC headers");

    response = controller.listAlerts(apiRequest(), camera_id);
    const auto alert_response = json::parse(response.body);
    require(response.code == 200 &&
            alert_response["items"].size() == 1 &&
            alert_response["items"][0]["delivery"]["status"] == "delivered",
        "HTTP alert audit must observe the delivered callback");

    processor.stop();
    _putenv_s("P5_INTEGRATION_CALLBACK_URL", "");
    _putenv_s("P5_INTEGRATION_CALLBACK_SECRET", "");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    WSACleanup();
    std::cout << "P5 integrated HTTP-to-callback algorithm service tests passed\n";
    return 0;
}
