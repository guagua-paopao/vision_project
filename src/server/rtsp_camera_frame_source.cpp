#include "server/rtsp_camera_frame_source.h"

#include <algorithm>

#include "server/uri_masker.h"

namespace yolo11_server {

RtspCameraFrameSource::RtspCameraFrameSource(
    CaptureSection capture_config,
    CameraProfile profile,
    std::shared_ptr<FfmpegOpenCoordinator> coordinator,
    bool require_ffmpeg_backend
) : capture_config_(std::move(capture_config)),
    profile_(std::move(profile)),
    coordinator_(std::move(coordinator)),
    require_ffmpeg_backend_(require_ffmpeg_backend) {
    capture_config_.transport = profile_.transport;
    if (require_ffmpeg_backend_) capture_config_.allow_backend_fallback = false;
}

RtspCameraFrameSource::~RtspCameraFrameSource() noexcept {
    stop();
}

bool RtspCameraFrameSource::start(std::string& error) {
    error.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reader_ && reader_->active()) return true;
    }

    std::string uri;
    if (!resolveCameraProfileUri(profile_, uri, error)) return false;
    if (!isRtspUri(uri)) {
        std::fill(uri.begin(), uri.end(), '\0');
        uri.clear();
        error = "camera profile environment value is not an RTSP URI";
        return false;
    }
    const std::string masked_uri = maskRtspUri(uri);
#ifdef _WIN32
    auto reader = std::make_shared<FfmpegProcessCaptureReader>(capture_config_);
#else
    auto reader = std::make_shared<RtspCaptureReader>(
        capture_config_, coordinator_, require_ffmpeg_backend_);
#endif
    const bool started = reader->start(uri, masked_uri, profile_.id, error);
    std::fill(uri.begin(), uri.end(), '\0');
    uri.clear();
    uri.shrink_to_fit();
    if (!started) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    reader_ = std::move(reader);
    return true;
}

void RtspCameraFrameSource::stop() noexcept {
    std::shared_ptr<ProductionRtspCaptureReader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reader = std::move(reader_);
    }
    if (reader) reader->stop();
}

SharedCameraFrame RtspCameraFrameSource::latest(std::uint64_t after_sequence) const {
    std::shared_ptr<ProductionRtspCaptureReader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reader = reader_;
    }
    return reader ? reader->getLatestFrameShared(after_sequence) : SharedCameraFrame{};
}

RtspCaptureMetrics RtspCameraFrameSource::metrics() const {
    std::shared_ptr<ProductionRtspCaptureReader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reader = reader_;
    }
    return reader ? reader->metrics() : RtspCaptureMetrics{};
}

bool RtspCameraFrameSource::active() const {
    std::shared_ptr<ProductionRtspCaptureReader> reader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reader = reader_;
    }
    return reader && reader->active();
}

std::shared_ptr<SharedCameraFrameHubRegistry> createSharedCameraFrameHubRegistry(
    const AppConfig& config
) {
    const CaptureSection capture_config = config.capture;
    const std::map<std::string, CameraProfile> profiles = config.camera_profiles;
    const bool require_ffmpeg_backend = config.camera_hub.require_ffmpeg_backend;
    const std::shared_ptr<FfmpegOpenCoordinator> coordinator = FfmpegOpenCoordinator::shared();
    return std::make_shared<SharedCameraFrameHubRegistry>(
        config.camera_hub,
        [capture_config, profiles, require_ffmpeg_backend, coordinator](
            const std::string& camera_profile
        ) -> std::unique_ptr<ICameraFrameSource> {
            const auto found = profiles.find(camera_profile);
            if (found == profiles.end()) return {};
            return std::make_unique<RtspCameraFrameSource>(
                capture_config, found->second, coordinator, require_ffmpeg_backend);
        });
}

}  // namespace yolo11_server
