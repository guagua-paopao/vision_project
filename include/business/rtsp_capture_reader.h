#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

#include "server/app_config.h"

namespace yolo11_server {

    struct CapturedFrame {
        cv::Mat image;
        std::uint64_t sequence = 0;
        long long capture_time_ms = 0;
        bool resolution_changed = false;
    };

    struct RtspCaptureMetrics {
        std::string state = "stopped";
        std::string backend_name;
        bool backend_fallback = false;
        double capture_fps = 0.0;
        double source_fps = 0.0;
        long long latest_frame_age_ms = -1;
        long long dropped_frames = 0;
        int reconnect_count = 0;
        long long last_frame_time_ms = 0;
        int width = 0;
        int height = 0;
        bool resolution_changed = false;
        long long resolution_change_count = 0;
        std::string last_error;
    };

    // Phase 19.3 production RTSP reader.
    //
    // Exactly one background thread owns VideoCapture. The inference thread can
    // only clone the newest decoded frame. Replacing an unread frame is counted
    // as a deliberate drop, so slow inference never creates an old-frame queue.
    class RtspCaptureReader {
    public:
        explicit RtspCaptureReader(const CaptureSection& config);
        ~RtspCaptureReader() noexcept;

        RtspCaptureReader(const RtspCaptureReader&) = delete;
        RtspCaptureReader& operator=(const RtspCaptureReader&) = delete;

        bool start(
            const std::string& uri,
            const std::string& masked_uri,
            const std::string& camera_profile,
            std::string& error
        );
        void stop() noexcept;

        bool getLatestFrame(std::uint64_t after_sequence, CapturedFrame& frame);
        RtspCaptureMetrics metrics() const;
        bool active() const;

    private:
        void captureLoop() noexcept;
        bool waitForRetry(int delay_ms) const;
        void setState(const std::string& state, const std::string& error = std::string());
        void clearSecretNoexcept() noexcept;
        static long long nowMs();

    private:
        CaptureSection config_;
        std::string uri_;
        std::string masked_uri_;
        std::string camera_profile_;

        mutable std::mutex mutex_;
        CapturedFrame latest_;
        std::uint64_t delivered_sequence_ = 0;
        RtspCaptureMetrics metrics_;

        std::thread capture_thread_;
        std::atomic<bool> stop_requested_{ false };
        std::atomic<bool> active_{ false };
    };

}  // namespace yolo11_server
