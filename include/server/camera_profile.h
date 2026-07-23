#pragma once

#include <map>
#include <string>

namespace yolo11_server {

    struct CameraProfile {
        std::string id;
        std::string source_type = "rtsp";
        std::string url_env;
        std::string display_name;
        std::string transport = "tcp";
        bool enabled = true;
        int version = 1;
        long long created_at_ms = 0;
        long long updated_at_ms = 0;
        long long deleted_at_ms = 0;
    };

    std::map<std::string, CameraProfile> loadCameraProfilesFromYaml(
        const std::string& yaml_path,
        std::string& error
    );

    bool resolveCameraProfileUri(
        const CameraProfile& profile,
        std::string& uri,
        std::string& error
    );

}  // namespace yolo11_server
