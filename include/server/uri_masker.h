#pragma once

#include <string>

namespace yolo11_server {

    // Phase 19.2: returns true only for rtsp:// or rtsps:// values.
    bool isRtspUri(const std::string& value);

    // Redacts the complete user-info component. For malformed RTSP input the
    // function deliberately returns only a scheme-level marker and never the
    // original value.
    std::string maskRtspUri(const std::string& value);

    // RTSP values are redacted; local camera/file display values are returned
    // unchanged because they do not contain camera credentials.
    std::string maskStreamSource(const std::string& source_type, const std::string& value);

}  // namespace yolo11_server
