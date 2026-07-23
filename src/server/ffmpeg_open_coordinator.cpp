#include "server/ffmpeg_open_coordinator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace yolo11_server {

namespace {

void setFfmpegTransport(const std::string& transport) {
    const std::string value = "rtsp_transport;" +
        (transport == "udp" ? std::string("udp") : std::string("tcp"));
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS", value.c_str());
#else
    ::setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", value.c_str(), 1);
#endif
}

std::string safeBackendName(cv::VideoCapture& capture) {
    try {
        return capture.getBackendName();
    }
    catch (...) {
        return {};
    }
}

bool isFfmpegBackend(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return name.find("FFMPEG") != std::string::npos;
}

}  // namespace

bool FfmpegOpenCoordinator::open(
    cv::VideoCapture& capture,
    const std::string& uri,
    const CaptureSection& config,
    const std::string& transport,
    bool require_ffmpeg_backend,
    std::string& backend_name,
    bool& fallback_used,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    backend_name.clear();
    error.clear();
    fallback_used = false;
    capture.release();
    setFfmpegTransport(transport);

    const std::vector<int> open_params = {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, config.open_timeout_ms,
        cv::CAP_PROP_READ_TIMEOUT_MSEC, config.read_timeout_ms
    };
    bool opened = capture.open(uri, cv::CAP_FFMPEG, open_params);
    if (!opened && config.allow_backend_fallback && !require_ffmpeg_backend) {
        capture.release();
        fallback_used = true;
        opened = capture.open(uri, cv::CAP_ANY, open_params);
    }
    if (!opened || !capture.isOpened()) {
        capture.release();
        error = "RTSP capture open failed";
        return false;
    }

    backend_name = safeBackendName(capture);
    if (require_ffmpeg_backend && !isFfmpegBackend(backend_name)) {
        capture.release();
        backend_name.clear();
        error = "FFMPEG_BACKEND_REQUIRED";
        return false;
    }
    return true;
}

std::shared_ptr<FfmpegOpenCoordinator> FfmpegOpenCoordinator::shared() {
    static const auto coordinator = std::make_shared<FfmpegOpenCoordinator>();
    return coordinator;
}

}  // namespace yolo11_server
