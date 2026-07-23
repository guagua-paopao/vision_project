#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "business/rtsp_capture_reader.h"
#ifdef _WIN32
#include "business/ffmpeg_process_capture_reader.h"
#endif
#include "server/camera_profile.h"
#include "server/shared_camera_frame_hub.h"

namespace yolo11_server {

#ifdef _WIN32
using ProductionRtspCaptureReader = FfmpegProcessCaptureReader;
#else
using ProductionRtspCaptureReader = RtspCaptureReader;
#endif

class RtspCameraFrameSource final : public ICameraFrameSource {
public:
    RtspCameraFrameSource(
        CaptureSection capture_config,
        CameraProfile profile,
        std::shared_ptr<FfmpegOpenCoordinator> coordinator,
        bool require_ffmpeg_backend
    );
    ~RtspCameraFrameSource() noexcept override;

    bool start(std::string& error) override;
    void stop() noexcept override;
    SharedCameraFrame latest(std::uint64_t after_sequence) const override;
    RtspCaptureMetrics metrics() const override;
    bool active() const override;

private:
    CaptureSection capture_config_;
    CameraProfile profile_;
    std::shared_ptr<FfmpegOpenCoordinator> coordinator_;
    bool require_ffmpeg_backend_ = true;
    mutable std::mutex mutex_;
    std::shared_ptr<ProductionRtspCaptureReader> reader_;
};

// Builds the process-local production registry. The factory captures a copy of
// camera profile metadata, so RTSP secrets are still resolved only when a Hub
// source starts inside the worker process.
std::shared_ptr<SharedCameraFrameHubRegistry> createSharedCameraFrameHubRegistry(
    const AppConfig& config
);

}  // namespace yolo11_server
