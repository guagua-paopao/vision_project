#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "business/camera_task_repository.h"
#include "server/app_config.h"

namespace yolo11_server {

struct CallbackHttpRequest {
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
    int timeout_ms = 5000;
    int response_body_limit_bytes = 4096;
    bool allow_insecure_http = false;
};

struct CallbackHttpResponse {
    bool transport_ok = false;
    int status_code = 0;
    std::string body;
    bool body_truncated = false;
    // Stable nonsecret code only; never include endpoint, body, or credentials.
    std::string error_code;
    // SHA-256 of the complete response stream, even when body capture is capped.
    std::string body_hash;
};

class ICallbackHttpTransport {
public:
    virtual ~ICallbackHttpTransport() = default;
    virtual CallbackHttpResponse post(const CallbackHttpRequest& request) noexcept = 0;
};

std::shared_ptr<ICallbackHttpTransport> createProductionCallbackHttpTransport();

struct CallbackDeliverySnapshot {
    bool running = false;
    int profiles_ready = 0;
    long long claimed = 0;
    long long delivered = 0;
    long long retries = 0;
    long long dead_letters = 0;
    long long transport_failures = 0;
    long long lease_conflicts = 0;
    long long last_success_at_ms = 0;
    std::string last_error_code;
};

// PostgreSQL transactional-outbox consumer. Claims use SKIP LOCKED and the
// monotonically increasing attempt as a fencing token. Delivery is at least
// once; receivers deduplicate by event_id / Idempotency-Key.
class CallbackDeliveryWorker final {
public:
    CallbackDeliveryWorker(
        AppConfig config,
        std::shared_ptr<CameraTaskRepository> repository,
        std::shared_ptr<ICallbackHttpTransport> transport = {}
    );
    ~CallbackDeliveryWorker() noexcept;

    CallbackDeliveryWorker(const CallbackDeliveryWorker&) = delete;
    CallbackDeliveryWorker& operator=(const CallbackDeliveryWorker&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool running() const;

    // Executes at most one due delivery. Exposed for deterministic acceptance
    // tests and operational drains; normal production use calls start().
    bool processOneAt(long long now_ms, bool& found, std::string& error);
    CallbackDeliverySnapshot snapshot() const;

private:
    struct ResolvedProfile {
        std::string url;
        std::string hmac_secret;
        bool allow_insecure_http = false;
    };

    bool ensureInitialized(std::string& error);
    void run() noexcept;
    void setLastError(const std::string& error_code);

    AppConfig config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::shared_ptr<ICallbackHttpTransport> transport_;
    std::map<std::string, ResolvedProfile> profiles_;
    mutable std::mutex init_mutex_;
    mutable std::mutex process_mutex_;
    mutable std::mutex status_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::thread thread_;
    bool initialized_ = false;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_requested_{ false };
    std::atomic<long long> claimed_{ 0 };
    std::atomic<long long> delivered_{ 0 };
    std::atomic<long long> retries_{ 0 };
    std::atomic<long long> dead_letters_{ 0 };
    std::atomic<long long> transport_failures_{ 0 };
    std::atomic<long long> lease_conflicts_{ 0 };
    std::atomic<long long> last_success_at_ms_{ 0 };
    std::string last_error_code_;
};

}  // namespace yolo11_server
