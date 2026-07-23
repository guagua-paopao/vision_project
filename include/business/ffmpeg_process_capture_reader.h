#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "business/rtsp_capture_reader.h"

namespace yolo11_server {

// Production Windows RTSP reader backed by the installed FFmpeg executable.
// The URI is written to an inherited anonymous stdin pipe as an ffconcat
// document; it is never placed in the child command line or on disk.
class FfmpegProcessCaptureReader {
public:
    explicit FfmpegProcessCaptureReader(const CaptureSection& config);
    ~FfmpegProcessCaptureReader() noexcept;

    FfmpegProcessCaptureReader(const FfmpegProcessCaptureReader&) = delete;
    FfmpegProcessCaptureReader& operator=(const FfmpegProcessCaptureReader&) = delete;

    bool start(
        const std::string& uri,
        const std::string& masked_uri,
        const std::string& camera_profile,
        std::string& error);
    void stop() noexcept;

    SharedCameraFrame getLatestFrameShared(std::uint64_t after_sequence = 0) const;
    RtspCaptureMetrics metrics() const;
    bool active() const;

private:
    void captureLoop() noexcept;
    bool waitForRetry(int delay_ms) const;
    void setState(const std::string& state, const std::string& error = std::string());
    void clearSecretNoexcept() noexcept;
    void terminateActiveProcessNoexcept() noexcept;
    static long long nowMs();

private:
    CaptureSection config_;
    std::string uri_;
    std::string masked_uri_;
    std::string camera_profile_;

    mutable std::mutex mutex_;
    mutable std::mutex process_mutex_;
    SharedCameraFrame latest_;
    RtspCaptureMetrics metrics_;
    void* process_handle_ = nullptr;

    std::thread capture_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> active_{false};
};

}  // namespace yolo11_server
