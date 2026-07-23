#include "business/ffmpeg_process_capture_reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace yolo11_server {
namespace {

int nextBackoffMs(int current, int maximum) {
    const long long doubled = static_cast<long long>(std::max(1, current)) * 2LL;
    return static_cast<int>(std::min<long long>(maximum, doubled));
}

std::string concatQuote(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 16);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

bool parsePositiveInt(std::string_view text, int& value) {
    if (text.empty()) return false;
    long long parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        parsed = parsed * 10 + (ch - '0');
        if (parsed > 16384) return false;
    }
    value = static_cast<int>(parsed);
    return value > 0;
}

bool parseY4mHeader(const std::string& line, int& width, int& height, double& fps) {
    if (line.rfind("YUV4MPEG2 ", 0) != 0) return false;
    std::istringstream input(line);
    std::string token;
    input >> token;
    int fps_num = 0;
    int fps_den = 1;
    while (input >> token) {
        if (token.size() > 1 && token.front() == 'W') {
            if (!parsePositiveInt(std::string_view(token).substr(1), width)) return false;
        }
        else if (token.size() > 1 && token.front() == 'H') {
            if (!parsePositiveInt(std::string_view(token).substr(1), height)) return false;
        }
        else if (token.size() > 1 && token.front() == 'F') {
            const auto colon = token.find(':');
            if (colon == std::string::npos ||
                !parsePositiveInt(std::string_view(token).substr(1, colon - 1), fps_num) ||
                !parsePositiveInt(std::string_view(token).substr(colon + 1), fps_den)) return false;
        }
    }
    if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) return false;
    fps = fps_num > 0 && fps_den > 0
        ? static_cast<double>(fps_num) / static_cast<double>(fps_den) : 0.0;
    return std::isfinite(fps) && fps >= 0.0 && fps <= 240.0;
}

#ifdef _WIN32

void closeHandle(HANDLE& handle) noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE stdout_read = nullptr;
};

bool writeAll(HANDLE pipe, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (!WriteFile(pipe, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool processAlive(HANDLE process) {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

bool readExact(
    HANDLE pipe,
    HANDLE process,
    void* destination,
    std::size_t size,
    int idle_timeout_ms,
    const std::atomic<bool>& stop_requested,
    std::size_t* received_total = nullptr
) {
    (void)process;
    (void)idle_timeout_ms;
    auto* output = static_cast<unsigned char*>(destination);
    std::size_t offset = 0;
    while (offset < size && !stop_requested.load()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, 64U * 1024U));
        DWORD received = 0;
        if (!ReadFile(pipe, output + offset, request, &received, nullptr) || received == 0) return false;
        offset += received;
        if (received_total) *received_total = offset;
    }
    return offset == size;
}

bool readLine(
    HANDLE pipe,
    HANDLE process,
    std::string& line,
    int idle_timeout_ms,
    const std::atomic<bool>& stop_requested
) {
    line.clear();
    while (line.size() < 4096 && !stop_requested.load()) {
        char ch = 0;
        if (!readExact(pipe, process, &ch, 1, idle_timeout_ms, stop_requested)) return false;
        if (ch == '\n') return true;
        if (ch != '\r') line.push_back(ch);
    }
    return false;
}

bool launchFfmpegProcess(
    const std::string& uri,
    const std::string& transport,
    int socket_timeout_ms,
    ChildProcess& child,
    std::string& error
) {
    error.clear();
    child = ChildProcess{};
    if (uri.find_first_of("\r\n") != std::string::npos ||
        uri.find('\0') != std::string::npos) {
        error = "camera URI contains unsupported control characters";
        return false;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE child_stdin_read = nullptr;
    HANDLE parent_stdin_write = nullptr;
    HANDLE parent_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE null_output = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_info{};

    auto cleanup = [&]() noexcept {
        closeHandle(child_stdin_read);
        closeHandle(parent_stdin_write);
        closeHandle(parent_stdout_read);
        closeHandle(child_stdout_write);
        closeHandle(null_output);
        closeHandle(process_info.hThread);
        closeHandle(process_info.hProcess);
    };

    if (!CreatePipe(&child_stdin_read, &parent_stdin_write, &attributes, 0) ||
        !SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&parent_stdout_read, &child_stdout_write, &attributes, 0) ||
        !SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        cleanup();
        error = "failed to create FFmpeg anonymous pipes";
        return false;
    }
    null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_output == INVALID_HANDLE_VALUE) {
        cleanup();
        error = "failed to open FFmpeg null error sink";
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = null_output;
    std::wstring command =
        L"ffmpeg.exe -hide_banner -loglevel quiet -nostats "
        L"-f concat -safe 0 -protocol_whitelist file,pipe,tcp,udp,rtp,rtsp,crypto "
        L"-i pipe:0 -map 0:v:0 -an -sn -dn -pix_fmt yuv420p -f yuv4mpegpipe pipe:1";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_info)) {
        cleanup();
        error = "failed to start FFmpeg executable";
        return false;
    }

    closeHandle(child_stdin_read);
    closeHandle(child_stdout_write);
    closeHandle(null_output);
    closeHandle(process_info.hThread);

    std::string concat = "ffconcat version 1.0\nfile " + concatQuote(uri) +
        "\noption rtsp_transport " + (transport == "udp" ? "udp" : "tcp") +
        "\noption timeout " + std::to_string(
            static_cast<long long>(std::max(1000, socket_timeout_ms)) * 1000LL) + "\n";
    const bool input_written = writeAll(parent_stdin_write, concat);
    std::fill(concat.begin(), concat.end(), '\0');
    concat.clear();
    closeHandle(parent_stdin_write);
    if (!input_written) {
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 1000);
        cleanup();
        error = "failed to provide camera reference to FFmpeg stdin";
        return false;
    }

    child.process = process_info.hProcess;
    child.stdout_read = parent_stdout_read;
    process_info.hProcess = nullptr;
    parent_stdout_read = nullptr;
    return true;
}

#endif

}  // namespace

FfmpegProcessCaptureReader::FfmpegProcessCaptureReader(const CaptureSection& config)
    : config_(config) {
}

FfmpegProcessCaptureReader::~FfmpegProcessCaptureReader() noexcept {
    stop();
}

bool FfmpegProcessCaptureReader::start(
    const std::string& uri,
    const std::string& masked_uri,
    const std::string& camera_profile,
    std::string& error
) {
    error.clear();
    if (active_.load()) {
        error = "FFmpeg process capture reader is already active";
        return false;
    }
    if (uri.empty()) {
        error = "RTSP capture URI is empty";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uri_ = uri;
        masked_uri_ = masked_uri;
        camera_profile_ = camera_profile;
        metrics_ = RtspCaptureMetrics{};
        metrics_.state = "starting";
    }
    std::atomic_store(&latest_, SharedCameraFrame{});
    stop_requested_.store(false);
    active_.store(true);
    try {
        capture_thread_ = std::thread([this]() { captureLoop(); });
    }
    catch (const std::exception& e) {
        active_.store(false);
        clearSecretNoexcept();
        error = std::string("failed to create FFmpeg capture thread: ") + e.what();
        return false;
    }
    return true;
}

void FfmpegProcessCaptureReader::stop() noexcept {
    stop_requested_.store(true);
    terminateActiveProcessNoexcept();
    try {
        if (capture_thread_.joinable() && capture_thread_.get_id() != std::this_thread::get_id()) {
            capture_thread_.join();
        }
    }
    catch (...) {
    }
    active_.store(false);
    setState("stopped");
    clearSecretNoexcept();
}

SharedCameraFrame FfmpegProcessCaptureReader::getLatestFrameShared(
    std::uint64_t after_sequence
) const {
    const auto latest = std::atomic_load(&latest_);
    if (!latest || latest->sequence == 0 || latest->sequence <= after_sequence || latest->image.empty()) {
        return {};
    }
    return latest;
}

RtspCaptureMetrics FfmpegProcessCaptureReader::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = metrics_;
    if (result.last_frame_time_ms > 0) {
        result.latest_frame_age_ms = std::max(0LL, nowMs() - result.last_frame_time_ms);
    }
    return result;
}

bool FfmpegProcessCaptureReader::active() const {
    return active_.load();
}

bool FfmpegProcessCaptureReader::waitForRetry(int delay_ms) const {
    int waited = 0;
    const int bounded = std::max(0, delay_ms);
    while (!stop_requested_.load() && waited < bounded) {
        const int step = std::min(100, bounded - waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    return !stop_requested_.load();
}

void FfmpegProcessCaptureReader::setState(const std::string& state, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.state = state;
    metrics_.last_error = error;
}

void FfmpegProcessCaptureReader::clearSecretNoexcept() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::fill(uri_.begin(), uri_.end(), '\0');
        uri_.clear();
        uri_.shrink_to_fit();
    }
    catch (...) {
    }
}

void FfmpegProcessCaptureReader::terminateActiveProcessNoexcept() noexcept {
#ifdef _WIN32
    try {
        std::lock_guard<std::mutex> lock(process_mutex_);
        auto process = static_cast<HANDLE>(process_handle_);
        if (process && processAlive(process)) TerminateProcess(process, 1);
    }
    catch (...) {
    }
#endif
}

long long FfmpegProcessCaptureReader::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void FfmpegProcessCaptureReader::captureLoop() noexcept {
#ifndef _WIN32
    setState("failed", "external FFmpeg process capture is unsupported on this platform");
    active_.store(false);
    clearSecretNoexcept();
#else
    int consecutive_failures = 0;
    int backoff_ms = std::max(100, config_.reconnect_initial_delay_ms);
    int previous_width = 0;
    int previous_height = 0;
    std::uint64_t sequence = 0;
    try {
        while (!stop_requested_.load()) {
            std::string uri_copy;
            std::string masked_copy;
            std::string profile_copy;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uri_copy = uri_;
                masked_copy = masked_uri_;
                profile_copy = camera_profile_;
            }
            setState(consecutive_failures == 0 ? "opening" : "reconnecting",
                consecutive_failures == 0 ? std::string{} :
                    std::string("FFmpeg process exited or timed out; retry scheduled"));

            ChildProcess child;
            std::string launch_error;
            const bool launched = launchFfmpegProcess(
                uri_copy, config_.transport,
                std::max(config_.open_timeout_ms, config_.read_timeout_ms),
                child, launch_error);
            std::fill(uri_copy.begin(), uri_copy.end(), '\0');
            uri_copy.clear();
            if (!launched) {
                ++consecutive_failures;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++metrics_.reconnect_count;
                    metrics_.state = "reconnecting";
                    metrics_.last_error = launch_error;
                }
                if (config_.reconnect_max_attempts > 0 &&
                    consecutive_failures >= config_.reconnect_max_attempts) {
                    setState("failed", "FFmpeg process reconnect attempts exhausted");
                    break;
                }
                if (!waitForRetry(backoff_ms)) break;
                backoff_ms = nextBackoffMs(backoff_ms, config_.reconnect_max_delay_ms);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(process_mutex_);
                process_handle_ = child.process;
            }

            std::string header;
            int width = 0;
            int height = 0;
            double source_fps = 0.0;
            bool stream_ok = readLine(child.stdout_read, child.process, header,
                std::max(15000, config_.open_timeout_ms), stop_requested_) &&
                parseY4mHeader(header, width, height, source_fps);
            if (stream_ok) {
                const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
                stream_ok = pixels <= (512ULL * 1024ULL * 1024ULL) && pixels % 2 == 0;
            }
            if (stream_ok) {
                std::lock_guard<std::mutex> lock(mutex_);
                metrics_.backend_name = "FFMPEG";
                metrics_.backend_fallback = false;
                ++metrics_.open_count;
                metrics_.source_fps = source_fps;
                metrics_.width = width;
                metrics_.height = height;
                metrics_.state = "reading_first_frame";
                metrics_.last_error.clear();
            }

            int warmup_remaining = std::max(0, config_.warmup_frames);
            long long fps_window_start_ms = nowMs();
            long long fps_window_frames = 0;
            const std::size_t frame_size = stream_ok
                ? static_cast<std::size_t>(width) * height * 3U / 2U : 0;
            std::vector<unsigned char> yuv(frame_size);
            bool first_frame = true;
            while (stream_ok && !stop_requested_.load()) {
                std::string frame_header;
                const int frame_timeout_ms = first_frame
                    ? std::max(15000, config_.open_timeout_ms)
                    : config_.read_timeout_ms;
                if (!readLine(child.stdout_read, child.process, frame_header,
                        frame_timeout_ms, stop_requested_)) {
                    if (!stop_requested_.load()) {
                        std::cerr << "[FFMPEG_PIPE] frame header read failed\n";
                    }
                    stream_ok = false;
                    break;
                }
                if (frame_header.rfind("FRAME", 0) != 0) {
                    std::cerr << "[FFMPEG_PIPE] invalid frame marker\n";
                    stream_ok = false;
                    break;
                }
                std::size_t payload_received = 0;
                if (!readExact(child.stdout_read, child.process, yuv.data(), yuv.size(),
                        frame_timeout_ms, stop_requested_, &payload_received)) {
                    if (!stop_requested_.load()) {
                        std::cerr << "[FFMPEG_PIPE] frame payload read failed: received="
                                  << payload_received << ", expected=" << yuv.size() << '\n';
                    }
                    stream_ok = false;
                    break;
                }
                first_frame = false;
                if (warmup_remaining > 0) {
                    --warmup_remaining;
                    continue;
                }

                cv::Mat yuv_frame(height * 3 / 2, width, CV_8UC1, yuv.data());
                cv::Mat decoded;
                cv::cvtColor(yuv_frame, decoded, cv::COLOR_YUV2BGR_I420);
                if (decoded.empty()) {
                    stream_ok = false;
                    break;
                }
                const long long capture_time_ms = nowMs();
                const bool resolution_changed = previous_width > 0 && previous_height > 0 &&
                    (previous_width != width || previous_height != height);
                previous_width = width;
                previous_height = height;

                ++fps_window_frames;
                const long long elapsed_ms = capture_time_ms - fps_window_start_ms;
                double capture_fps = 0.0;
                if (elapsed_ms >= 1000) {
                    capture_fps = static_cast<double>(fps_window_frames) * 1000.0 /
                        static_cast<double>(std::max(1LL, elapsed_ms));
                    fps_window_start_ms = capture_time_ms;
                    fps_window_frames = 0;
                }

                auto published = std::make_shared<FrameEnvelope>();
                published->image = std::move(decoded);
                published->sequence = ++sequence;
                published->capture_time_ms = capture_time_ms;
                published->publish_time = std::chrono::steady_clock::now();
                published->resolution_changed = resolution_changed;
                const bool overwritten = static_cast<bool>(std::atomic_load(&latest_));
                std::atomic_store(&latest_, std::static_pointer_cast<const FrameEnvelope>(published));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (overwritten) {
                        ++metrics_.overwritten_frames;
                        metrics_.dropped_frames = metrics_.overwritten_frames;
                    }
                    metrics_.state = "running";
                    metrics_.last_error.clear();
                    metrics_.last_frame_time_ms = capture_time_ms;
                    metrics_.latest_frame_age_ms = 0;
                    metrics_.width = width;
                    metrics_.height = height;
                    metrics_.resolution_changed = resolution_changed;
                    if (resolution_changed) ++metrics_.resolution_change_count;
                    if (capture_fps > 0.0) metrics_.capture_fps = capture_fps;
                }
            }

            closeHandle(child.stdout_read);
            if (processAlive(child.process)) TerminateProcess(child.process, 1);
            WaitForSingleObject(child.process, 2000);
            {
                std::lock_guard<std::mutex> lock(process_mutex_);
                if (process_handle_ == child.process) process_handle_ = nullptr;
                closeHandle(child.process);
            }
            if (stop_requested_.load()) break;

            ++consecutive_failures;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++metrics_.reconnect_count;
                metrics_.state = "reconnecting";
                metrics_.last_error = "FFmpeg process exited or timed out; retry scheduled";
            }
            spdlog::warn("FFmpeg RTSP process reconnect scheduled: camera_profile={}, masked_uri={}",
                profile_copy, masked_copy);
            if (config_.reconnect_max_attempts > 0 &&
                consecutive_failures >= config_.reconnect_max_attempts) {
                setState("failed", "FFmpeg process reconnect attempts exhausted");
                break;
            }
            if (!waitForRetry(backoff_ms)) break;
            backoff_ms = nextBackoffMs(backoff_ms, config_.reconnect_max_delay_ms);
        }
    }
    catch (...) {
        setState("failed", "FFmpeg process capture raised an exception");
        terminateActiveProcessNoexcept();
    }
    active_.store(false);
    if (stop_requested_.load()) setState("stopped");
    clearSecretNoexcept();
#endif
}

}  // namespace yolo11_server
