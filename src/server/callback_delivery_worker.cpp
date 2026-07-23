#include "server/callback_delivery_worker.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace yolo11_server {
namespace {

using json = nlohmann::json;

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string hexBytes(const unsigned char* bytes, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<int>(bytes[index]);
    }
    return output.str();
}

std::string sha256Hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    return hexBytes(digest, sizeof(digest));
}

std::string hmacSha256Hex(const std::string& secret, const std::string& value) {
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
    return hexBytes(digest, size);
}

std::string callbackBody(const SecurityAlertEventRecord& alert) {
    auto payload = json::parse(alert.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) payload = json::object();
    json body{
        { "schema_version", "1.0" },
        { "event_kind", "algorithm_alert" },
        { "event_id", alert.event_id },
        { "task_id", alert.task_id },
        { "camera_id", alert.task_id },
        { "run_id", alert.run_id },
        { "camera_profile", alert.camera_profile },
        { "event_type", alert.event_type },
        { "category", alert.category },
        { "severity", alert.severity },
        { "confidence", alert.confidence ? json(*alert.confidence) : json(nullptr) },
        { "track_id", alert.track_id ? json(*alert.track_id) : json(nullptr) },
        { "occurred_at_ms", alert.occurred_at_ms },
        { "algorithm", {
            { "profile", alert.algorithm_profile },
            { "model", alert.model_name },
            { "config_version", alert.config_version },
            { "demo_classifier", alert.demo_classifier }
        } },
        { "payload", std::move(payload) },
        { "created_at_ms", alert.created_at_ms }
    };
    if (!alert.evidence_frame_id.empty()) {
        body["evidence"] = { { "frame_id", alert.evidence_frame_id } };
    }
    return body.dump();
}

long long retryDelayMs(const CallbackDeliverySection& config, int attempt) {
    long long delay = config.initial_backoff_ms;
    for (int current = 1;
         current < attempt && delay < config.max_backoff_ms;
         ++current) {
        delay = std::min<long long>(
            config.max_backoff_ms,
            delay > std::numeric_limits<long long>::max() / 2
                ? config.max_backoff_ms
                : delay * 2);
    }
    return std::min<long long>(delay, config.max_backoff_ms);
}

bool retryableStatus(int status) {
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

bool validErrorCode(const std::string& value) {
    return !value.empty() && value.size() <= 160 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
        });
}

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) != required) {
        return {};
    }
    return result;
}

class InternetHandle final {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_ = nullptr;
};

class WinHttpCallbackTransport final : public ICallbackHttpTransport {
public:
    CallbackHttpResponse post(const CallbackHttpRequest& request) noexcept override {
        CallbackHttpResponse response;
        try {
            const std::wstring url = utf8ToWide(request.url);
            if (url.empty() || request.body.size() >
                    static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
                response.error_code = "CALLBACK_URL_OR_BODY_INVALID";
                return response;
            }

            URL_COMPONENTS components{};
            components.dwStructSize = sizeof(components);
            components.dwSchemeLength = static_cast<DWORD>(-1);
            components.dwHostNameLength = static_cast<DWORD>(-1);
            components.dwUrlPathLength = static_cast<DWORD>(-1);
            components.dwExtraInfoLength = static_cast<DWORD>(-1);
            components.dwUserNameLength = static_cast<DWORD>(-1);
            components.dwPasswordLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
                components.dwHostNameLength == 0 ||
                components.dwUserNameLength != 0 ||
                components.dwPasswordLength != 0) {
                response.error_code = "CALLBACK_URL_INVALID";
                return response;
            }
            const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
            const bool insecure = components.nScheme == INTERNET_SCHEME_HTTP;
            if ((!secure && !insecure) || (insecure && !request.allow_insecure_http)) {
                response.error_code = insecure
                    ? "CALLBACK_INSECURE_HTTP_FORBIDDEN"
                    : "CALLBACK_SCHEME_UNSUPPORTED";
                return response;
            }

            const std::wstring host(
                components.lpszHostName, components.dwHostNameLength);
            std::wstring target = components.dwUrlPathLength == 0
                ? std::wstring(L"/")
                : std::wstring(components.lpszUrlPath, components.dwUrlPathLength);
            if (components.dwExtraInfoLength > 0) {
                const std::wstring extra(
                    components.lpszExtraInfo, components.dwExtraInfoLength);
                if (extra.find(L'#') != std::wstring::npos) {
                    response.error_code = "CALLBACK_URL_FRAGMENT_FORBIDDEN";
                    return response;
                }
                target += extra;
            }

            InternetHandle session(WinHttpOpen(
                L"vision-project-callback/1.0",
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0));
            if (!session) {
                response.error_code = "WINHTTP_SESSION_FAILED";
                return response;
            }
            WinHttpSetTimeouts(
                session.get(),
                request.timeout_ms,
                request.timeout_ms,
                request.timeout_ms,
                request.timeout_ms);
            InternetHandle connection(WinHttpConnect(
                session.get(), host.c_str(), components.nPort, 0));
            if (!connection) {
                response.error_code = "WINHTTP_CONNECT_FAILED";
                return response;
            }
            InternetHandle http_request(WinHttpOpenRequest(
                connection.get(),
                L"POST",
                target.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                secure ? WINHTTP_FLAG_SECURE : 0));
            if (!http_request) {
                response.error_code = "WINHTTP_REQUEST_FAILED";
                return response;
            }
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (!WinHttpSetOption(
                    http_request.get(),
                    WINHTTP_OPTION_REDIRECT_POLICY,
                    &redirect_policy,
                    sizeof(redirect_policy))) {
                response.error_code = "WINHTTP_REDIRECT_POLICY_FAILED";
                return response;
            }

            std::wstring headers = L"Content-Type: application/json\r\n";
            for (const auto& header : request.headers) {
                const std::wstring name = utf8ToWide(header.first);
                const std::wstring value = utf8ToWide(header.second);
                if (name.empty() || value.empty() ||
                    header.first.find_first_of("\r\n") != std::string::npos ||
                    header.second.find_first_of("\r\n") != std::string::npos) {
                    response.error_code = "CALLBACK_HEADER_INVALID";
                    return response;
                }
                headers += name + L": " + value + L"\r\n";
            }
            if (!WinHttpSendRequest(
                    http_request.get(),
                    headers.c_str(),
                    static_cast<DWORD>(headers.size()),
                    request.body.empty()
                        ? WINHTTP_NO_REQUEST_DATA
                        : const_cast<char*>(request.body.data()),
                    static_cast<DWORD>(request.body.size()),
                    static_cast<DWORD>(request.body.size()),
                    0)) {
                response.error_code = "WINHTTP_SEND_FAILED";
                return response;
            }
            if (!WinHttpReceiveResponse(http_request.get(), nullptr)) {
                response.error_code = "WINHTTP_RECEIVE_FAILED";
                return response;
            }
            DWORD status = 0;
            DWORD status_size = sizeof(status);
            if (!WinHttpQueryHeaders(
                    http_request.get(),
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX)) {
                response.error_code = "WINHTTP_STATUS_FAILED";
                return response;
            }
            response.status_code = static_cast<int>(status);

            const std::size_t body_limit =
                static_cast<std::size_t>(std::max(0, request.response_body_limit_bytes));
            SHA256_CTX response_digest{};
            SHA256_Init(&response_digest);
            while (true) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(http_request.get(), &available)) {
                    response.error_code = "WINHTTP_BODY_QUERY_FAILED";
                    return response;
                }
                if (available == 0) break;
                std::vector<char> buffer(std::min<DWORD>(available, 8192));
                DWORD read = 0;
                if (!WinHttpReadData(
                        http_request.get(),
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &read)) {
                    response.error_code = "WINHTTP_BODY_READ_FAILED";
                    return response;
                }
                SHA256_Update(&response_digest, buffer.data(), read);
                const std::size_t remaining =
                    response.body.size() < body_limit
                        ? body_limit - response.body.size()
                        : 0;
                const std::size_t copy = std::min<std::size_t>(remaining, read);
                response.body.append(buffer.data(), copy);
                if (copy < read) response.body_truncated = true;
            }
            unsigned char digest[SHA256_DIGEST_LENGTH]{};
            SHA256_Final(digest, &response_digest);
            response.body_hash = hexBytes(digest, sizeof(digest));
            response.transport_ok = true;
            return response;
        }
        catch (...) {
            response.error_code = "WINHTTP_UNEXPECTED_FAILURE";
            return response;
        }
    }
};

#else

class UnsupportedCallbackTransport final : public ICallbackHttpTransport {
public:
    CallbackHttpResponse post(const CallbackHttpRequest&) noexcept override {
        CallbackHttpResponse result;
        result.error_code = "CALLBACK_TRANSPORT_UNSUPPORTED";
        return result;
    }
};

#endif

}  // namespace

std::shared_ptr<ICallbackHttpTransport> createProductionCallbackHttpTransport() {
#ifdef _WIN32
    return std::make_shared<WinHttpCallbackTransport>();
#else
    return std::make_shared<UnsupportedCallbackTransport>();
#endif
}

CallbackDeliveryWorker::CallbackDeliveryWorker(
    AppConfig config,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICallbackHttpTransport> transport
) : config_(std::move(config)),
    repository_(std::move(repository)),
    transport_(transport ? std::move(transport) : createProductionCallbackHttpTransport()) {
}

CallbackDeliveryWorker::~CallbackDeliveryWorker() noexcept {
    stop();
}

bool CallbackDeliveryWorker::ensureInitialized(std::string& error) {
    std::lock_guard<std::mutex> lock(init_mutex_);
    error.clear();
    if (initialized_) return true;
    if (!config_.callbacks.enabled) {
        error = "CALLBACK_DELIVERY_DISABLED";
        return false;
    }
    if (!config_.callbacks.config_error.empty()) {
        error = config_.callbacks.config_error;
        return false;
    }
    if (!repository_ || !transport_) {
        error = "callback delivery dependencies are unavailable";
        return false;
    }
    if (!repository_->initialize(error)) return false;

    std::map<std::string, ResolvedProfile> resolved;
    for (const auto& entry : config_.callbacks.profiles) {
        if (!entry.second.enabled) continue;
        const char* url = std::getenv(entry.second.url_env.c_str());
        const char* secret = std::getenv(entry.second.hmac_secret_env.c_str());
        if (!url || !*url) {
            error = "callback endpoint environment variable is not configured";
            return false;
        }
        if (!secret || std::char_traits<char>::length(secret) < 16) {
            error = "callback HMAC secret is missing or shorter than 16 bytes";
            return false;
        }
        if (std::char_traits<char>::length(url) > 2048 ||
            std::char_traits<char>::length(secret) > 4096) {
            error = "callback endpoint or secret is outside allowed bounds";
            return false;
        }
        const std::string endpoint(url);
        const bool https = endpoint.rfind("https://", 0) == 0;
        const bool http = endpoint.rfind("http://", 0) == 0;
        if (!https && !(http && entry.second.allow_insecure_http)) {
            error = http
                ? "callback insecure HTTP is forbidden by profile"
                : "callback endpoint must use HTTP or HTTPS";
            return false;
        }
        resolved.emplace(
            entry.first,
            ResolvedProfile{
                endpoint,
                std::string(secret),
                entry.second.allow_insecure_http
            });
    }
    if (resolved.empty()) {
        error = "no enabled callback profiles are ready";
        return false;
    }
    profiles_ = std::move(resolved);
    initialized_ = true;
    return true;
}

bool CallbackDeliveryWorker::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (!ensureInitialized(error)) return false;
    stop_requested_.store(false);
    try {
        thread_ = std::thread([this]() { run(); });
    }
    catch (const std::exception& exception) {
        error = std::string("failed to start callback delivery thread: ") + exception.what();
        return false;
    }
    running_.store(true);
    return true;
}

void CallbackDeliveryWorker::stop() noexcept {
    stop_requested_.store(true);
    wait_cv_.notify_all();
    try {
        if (thread_.joinable()) thread_.join();
    }
    catch (...) {
    }
    running_.store(false);
}

bool CallbackDeliveryWorker::running() const {
    return running_.load();
}

void CallbackDeliveryWorker::setLastError(const std::string& error_code) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    last_error_code_ = error_code;
}

bool CallbackDeliveryWorker::processOneAt(
    long long now_ms,
    bool& found,
    std::string& error
) {
    std::lock_guard<std::mutex> process_lock(process_mutex_);
    found = false;
    error.clear();
    if (now_ms <= 0) {
        error = "callback delivery time is invalid";
        return false;
    }
    if (!ensureInitialized(error)) return false;

    CallbackOutboxRecord outbox;
    if (!repository_->claimDueCallback(
            now_ms,
            config_.callbacks.lease_timeout_ms,
            outbox,
            found,
            error)) {
        setLastError("CALLBACK_STORAGE_CLAIM_FAILED");
        return false;
    }
    if (!found) return true;
    ++claimed_;

    SecurityAlertEventRecord alert;
    bool alert_found = false;
    if (!repository_->getAlert(outbox.event_id, alert, alert_found, error)) {
        setLastError("CALLBACK_ALERT_READ_FAILED");
        return false;
    }

    const auto profile = profiles_.find(outbox.callback_profile);
    if (!alert_found || profile == profiles_.end()) {
        const std::string code = alert_found
            ? "CALLBACK_PROFILE_NOT_CONFIGURED"
            : "CALLBACK_ALERT_NOT_FOUND";
        if (!repository_->finishCallbackAttempt(
                outbox.outbox_id,
                outbox.attempt,
                "dead_letter",
                now_ms,
                now_ms,
                0,
                code,
                {},
                error)) {
            ++lease_conflicts_;
            setLastError("CALLBACK_LEASE_COMPLETION_FAILED");
            return false;
        }
        ++dead_letters_;
        setLastError(code);
        return true;
    }

    const std::string timestamp = std::to_string(now_ms);
    const std::string body = callbackBody(alert);
    if (body.size() > static_cast<std::size_t>(
            config_.callbacks.request_body_limit_bytes)) {
        if (!repository_->finishCallbackAttempt(
                outbox.outbox_id,
                outbox.attempt,
                "dead_letter",
                now_ms,
                now_ms,
                0,
                "CALLBACK_PAYLOAD_TOO_LARGE",
                {},
                error)) {
            ++lease_conflicts_;
            setLastError("CALLBACK_LEASE_COMPLETION_FAILED");
            return false;
        }
        ++dead_letters_;
        setLastError("CALLBACK_PAYLOAD_TOO_LARGE");
        return true;
    }
    CallbackHttpRequest request;
    request.url = profile->second.url;
    request.body = body;
    request.timeout_ms = config_.callbacks.request_timeout_ms;
    request.response_body_limit_bytes =
        config_.callbacks.response_body_limit_bytes;
    request.allow_insecure_http = profile->second.allow_insecure_http;
    request.headers = {
        { "Idempotency-Key", alert.event_id },
        { "X-Event-Id", alert.event_id },
        { "X-Signature", hmacSha256Hex(
            profile->second.hmac_secret, timestamp + "\n" + body) },
        { "X-Signature-Version", "1" },
        { "X-Timestamp", timestamp }
    };

    CallbackHttpResponse response;
    try {
        response = transport_->post(request);
    }
    catch (...) {
        response.error_code = "CALLBACK_TRANSPORT_EXCEPTION";
    }
    const std::string response_hash =
        response.body_hash.size() == 64
            ? response.body_hash
            : (response.body.empty() ? std::string{} : sha256Hex(response.body));

    if (response.transport_ok &&
        response.status_code >= 200 &&
        response.status_code <= 299) {
        if (!repository_->markCallbackDelivered(
                outbox.outbox_id,
                outbox.attempt,
                now_ms,
                response.status_code,
                response_hash,
                error)) {
            ++lease_conflicts_;
            setLastError("CALLBACK_LEASE_COMPLETION_FAILED");
            return false;
        }
        ++delivered_;
        last_success_at_ms_.store(now_ms);
        setLastError({});
        return true;
    }

    if (!response.transport_ok) ++transport_failures_;
    const bool retryable = !response.transport_ok ||
        retryableStatus(response.status_code);
    std::string code;
    std::string next_status;
    long long next_attempt_at_ms = now_ms;
    if (retryable && outbox.attempt < config_.callbacks.max_attempts) {
        code = response.transport_ok
            ? "CALLBACK_HTTP_RETRYABLE"
            : (validErrorCode(response.error_code)
                ? response.error_code
                : "CALLBACK_TRANSPORT_FAILED");
        next_status = "retry";
        next_attempt_at_ms += retryDelayMs(config_.callbacks, outbox.attempt);
    }
    else {
        next_status = "dead_letter";
        if (retryable) code = "CALLBACK_MAX_ATTEMPTS_EXHAUSTED";
        else if (response.status_code >= 400 && response.status_code <= 499) {
            code = "CALLBACK_HTTP_TERMINAL_4XX";
        }
        else {
            code = "CALLBACK_HTTP_UNEXPECTED_STATUS";
        }
    }
    if (!repository_->finishCallbackAttempt(
            outbox.outbox_id,
            outbox.attempt,
            next_status,
            next_attempt_at_ms,
            now_ms,
            response.status_code,
            code,
            response_hash,
            error)) {
        ++lease_conflicts_;
        setLastError("CALLBACK_LEASE_COMPLETION_FAILED");
        return false;
    }
    if (next_status == "retry") ++retries_;
    else ++dead_letters_;
    setLastError(code);
    return true;
}

CallbackDeliverySnapshot CallbackDeliveryWorker::snapshot() const {
    CallbackDeliverySnapshot result;
    result.running = running_.load();
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        result.profiles_ready = static_cast<int>(profiles_.size());
    }
    result.claimed = claimed_.load();
    result.delivered = delivered_.load();
    result.retries = retries_.load();
    result.dead_letters = dead_letters_.load();
    result.transport_failures = transport_failures_.load();
    result.lease_conflicts = lease_conflicts_.load();
    result.last_success_at_ms = last_success_at_ms_.load();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        result.last_error_code = last_error_code_;
    }
    return result;
}

void CallbackDeliveryWorker::run() noexcept {
    while (!stop_requested_.load()) {
        bool found = false;
        std::string error;
        if (!processOneAt(wallNowMs(), found, error)) {
            spdlog::warn(
                "Callback delivery poll failed: error_code={}",
                snapshot().last_error_code);
        }
        if (stop_requested_.load()) break;
        if (found) continue;
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(
            lock,
            std::chrono::milliseconds(config_.callbacks.poll_interval_ms),
            [&]() { return stop_requested_.load(); });
    }
}

}  // namespace yolo11_server
