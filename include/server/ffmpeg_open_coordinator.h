#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <opencv2/videoio.hpp>

#include "server/app_config.h"

namespace yolo11_server {

class FfmpegOpenCoordinator {
public:
    bool open(
        cv::VideoCapture& capture,
        const std::string& uri,
        const CaptureSection& config,
        const std::string& transport,
        bool require_ffmpeg_backend,
        std::string& backend_name,
        bool& fallback_used,
        std::string& error
    );

    static std::shared_ptr<FfmpegOpenCoordinator> shared();

private:
    std::mutex mutex_;
};

}  // namespace yolo11_server
