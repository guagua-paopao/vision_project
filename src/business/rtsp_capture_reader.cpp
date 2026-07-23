#include "business/rtsp_capture_reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>

#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

namespace yolo11_server {

    namespace {

        int nextBackoffMs(int current, int maximum) {
            const long long doubled = static_cast<long long>(std::max(1, current)) * 2LL;
            return static_cast<int>(std::min<long long>(maximum, doubled));
        }

    }  // namespace

    RtspCaptureReader::RtspCaptureReader(
        const CaptureSection& config,
        std::shared_ptr<FfmpegOpenCoordinator> open_coordinator,
        bool require_ffmpeg_backend
    ) : config_(config),
        open_coordinator_(std::move(open_coordinator)),
        require_ffmpeg_backend_(require_ffmpeg_backend) {
        if (!open_coordinator_) open_coordinator_ = FfmpegOpenCoordinator::shared();
    }

    RtspCaptureReader::~RtspCaptureReader() noexcept {
        stop();
    }

    bool RtspCaptureReader::start(
        const std::string& uri,
        const std::string& masked_uri,
        const std::string& camera_profile,
        std::string& error
    ) {
        error.clear();
        if (active_.load()) {
            error = "RTSP capture reader is already active";
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
            error = std::string("failed to create RTSP capture thread: ") + e.what();
            return false;
        }
        return true;
    }

    void RtspCaptureReader::stop() noexcept {
        stop_requested_.store(true);
        try {
            if (capture_thread_.joinable()) {
                if (capture_thread_.get_id() == std::this_thread::get_id()) {
                    capture_thread_.detach();
                }
                else {
                    capture_thread_.join();
                }
            }
        }
        catch (...) {
            spdlog::error("RTSP capture reader stop exception ignored");
        }
        active_.store(false);
        setState("stopped");
        clearSecretNoexcept();
    }

    bool RtspCaptureReader::getLatestFrame(std::uint64_t after_sequence, CapturedFrame& frame) {
        const SharedCameraFrame latest = getLatestFrameShared(after_sequence);
        if (!latest) return false;
        frame.sequence = latest->sequence;
        frame.capture_time_ms = latest->capture_time_ms;
        frame.publish_time = latest->publish_time;
        frame.resolution_changed = latest->resolution_changed;
        frame.image = latest->image.clone();
        return !frame.image.empty();
    }

    SharedCameraFrame RtspCaptureReader::getLatestFrameShared(std::uint64_t after_sequence) const {
        const SharedCameraFrame latest = std::atomic_load(&latest_);
        if (!latest || latest->sequence == 0 || latest->sequence <= after_sequence || latest->image.empty()) {
            return {};
        }
        return latest;
    }

    RtspCaptureMetrics RtspCaptureReader::metrics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        RtspCaptureMetrics result = metrics_;
        if (result.last_frame_time_ms > 0) {
            result.latest_frame_age_ms = std::max(0LL, nowMs() - result.last_frame_time_ms);
        }
        return result;
    }

    bool RtspCaptureReader::active() const {
        return active_.load();
    }

    bool RtspCaptureReader::waitForRetry(int delay_ms) const {
        const int bounded_delay = std::max(0, delay_ms);
        int waited = 0;
        while (!stop_requested_.load() && waited < bounded_delay) {
            const int step = std::min(100, bounded_delay - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            waited += step;
        }
        return !stop_requested_.load();
    }

    void RtspCaptureReader::setState(const std::string& state, const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.state = state;
        metrics_.last_error = error;
    }

    void RtspCaptureReader::clearSecretNoexcept() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            std::fill(uri_.begin(), uri_.end(), '\0');
            uri_.clear();
            uri_.shrink_to_fit();
        }
        catch (...) {
        }
    }

    long long RtspCaptureReader::nowMs() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    void RtspCaptureReader::captureLoop() noexcept {
        cv::VideoCapture capture;
        int consecutive_failures = 0;
        int backoff_ms = std::max(100, config_.reconnect_initial_delay_ms);
        int warmup_remaining = std::max(0, config_.warmup_frames);
        long long fps_window_start_ms = nowMs();
        long long fps_window_frames = 0;
        std::uint64_t sequence = 0;
        int previous_width = 0;
        int previous_height = 0;

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
                    consecutive_failures == 0 ? std::string{} : std::string("RTSP open/read failed; retry scheduled"));

                bool fallback_used = false;
                std::string backend_name;
                std::string open_error;
                const bool opened = open_coordinator_->open(
                    capture, uri_copy, config_, config_.transport, require_ffmpeg_backend_,
                    backend_name, fallback_used, open_error);
                std::fill(uri_copy.begin(), uri_copy.end(), '\0');
                uri_copy.clear();

                if (!opened || !capture.isOpened()) {
                    capture.release();
                    ++consecutive_failures;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++metrics_.reconnect_count;
                        metrics_.backend_fallback = fallback_used;
                        metrics_.state = "reconnecting";
                        metrics_.last_error = open_error == "FFMPEG_BACKEND_REQUIRED"
                            ? open_error
                            : "RTSP open failed; retry scheduled";
                    }
                    spdlog::warn(
                        "RTSP open failed: camera_profile={}, masked_uri={}, reconnect_count={}",
                        profile_copy,
                        masked_copy,
                        metrics().reconnect_count
                    );
                    if (open_error == "FFMPEG_BACKEND_REQUIRED") {
                        setState("failed", open_error);
                        break;
                    }
                    if (config_.reconnect_max_attempts > 0 && consecutive_failures >= config_.reconnect_max_attempts) {
                        setState("failed", "RTSP reconnect attempts exhausted during open");
                        break;
                    }
                    if (!waitForRetry(backoff_ms)) break;
                    backoff_ms = nextBackoffMs(backoff_ms, config_.reconnect_max_delay_ms);
                    continue;
                }

                capture.set(cv::CAP_PROP_BUFFERSIZE, static_cast<double>(std::max(1, config_.buffer_size)));
                double source_fps = capture.get(cv::CAP_PROP_FPS);
                if (!std::isfinite(source_fps) || source_fps < 0.0 || source_fps > 240.0) source_fps = 0.0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    metrics_.backend_name = backend_name;
                    metrics_.backend_fallback = fallback_used;
                    ++metrics_.open_count;
                    metrics_.source_fps = source_fps;
                    metrics_.last_error.clear();
                }
                spdlog::info(
                    "RTSP capture opened: camera_profile={}, masked_uri={}, backend={}, fallback={}",
                    profile_copy,
                    masked_copy,
                    backend_name,
                    fallback_used
                );

                consecutive_failures = 0;
                backoff_ms = std::max(100, config_.reconnect_initial_delay_ms);
                warmup_remaining = std::max(0, config_.warmup_frames);
                fps_window_start_ms = nowMs();
                fps_window_frames = 0;
                bool reconnect_required = false;
                long long last_decoded_time_ms = 0;

                while (!stop_requested_.load() && capture.isOpened()) {
                    cv::Mat decoded;
                    const bool read_ok = capture.read(decoded);
                    const long long capture_time_ms = nowMs();
                    if (!read_ok || decoded.empty()) {
                        reconnect_required = true;
                        setState("reconnecting", "RTSP read timed out or returned an empty frame");
                        break;
                    }
                    if (last_decoded_time_ms > 0 &&
                        capture_time_ms - last_decoded_time_ms > config_.stale_frame_timeout_ms) {
                        reconnect_required = true;
                        setState("reconnecting", "RTSP frame became stale; reconnect scheduled");
                        break;
                    }
                    last_decoded_time_ms = capture_time_ms;

                    if (warmup_remaining > 0) {
                        --warmup_remaining;
                        continue;
                    }

                    const int width = decoded.cols;
                    const int height = decoded.rows;
                    const bool resolution_changed = previous_width > 0 && previous_height > 0 &&
                        (previous_width != width || previous_height != height);
                    previous_width = width;
                    previous_height = height;

                    ++fps_window_frames;
                    const long long fps_elapsed_ms = capture_time_ms - fps_window_start_ms;
                    double capture_fps = 0.0;
                    if (fps_elapsed_ms >= 1000) {
                        capture_fps = static_cast<double>(fps_window_frames) * 1000.0 /
                            static_cast<double>(std::max(1LL, fps_elapsed_ms));
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

                capture.release();
                if (stop_requested_.load()) break;
                if (reconnect_required) {
                    ++consecutive_failures;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++metrics_.reconnect_count;
                    }
                    if (config_.reconnect_max_attempts > 0 && consecutive_failures >= config_.reconnect_max_attempts) {
                        setState("failed", "RTSP reconnect attempts exhausted after read failure");
                        break;
                    }
                    if (!waitForRetry(backoff_ms)) break;
                    backoff_ms = nextBackoffMs(backoff_ms, config_.reconnect_max_delay_ms);
                }
            }
        }
        catch (const std::exception& e) {
            // Do not attach the URI to this message. Some OpenCV exceptions
            // include the filename supplied to open(), so expose a fixed error.
            spdlog::error("RTSP capture loop exception: camera_profile={}, error_type=standard", camera_profile_);
            setState("failed", "RTSP capture backend raised an exception");
        }
        catch (...) {
            spdlog::error("RTSP capture loop unknown exception: camera_profile={}", camera_profile_);
            setState("failed", "RTSP capture backend raised an unknown exception");
        }

        capture.release();
        active_.store(false);
        if (stop_requested_.load()) setState("stopped");
        clearSecretNoexcept();
    }

}  // namespace yolo11_server
