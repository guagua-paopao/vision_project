#include "server/camera_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>

#include <yaml-cpp/yaml.h>

#include "server/yaml_file_loader.h"

namespace yolo11_server {

    namespace {

        template <typename T>
        T readOrDefault(const YAML::Node& node, const std::string& key, const T& fallback) {
            if (!node || !node[key]) {
                return fallback;
            }
            return node[key].as<T>();
        }

        std::string toLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

    }  // namespace

    std::map<std::string, CameraProfile> loadCameraProfilesFromYaml(
        const std::string& yaml_path,
        std::string& error
    ) {
        std::map<std::string, CameraProfile> profiles;
        error.clear();
        if (yaml_path.empty()) {
            return profiles;
        }

        try {
            const YAML::Node root = loadYamlFileAbiSafe(yaml_path);
            const YAML::Node cameras = root["cameras"];
            if (!cameras || !cameras.IsMap()) {
                error = "camera profile file must contain a cameras map";
                return profiles;
            }

            for (const auto& entry : cameras) {
                CameraProfile profile;
                profile.id = entry.first.as<std::string>();
                const YAML::Node node = entry.second;
                profile.source_type = toLower(readOrDefault<std::string>(node, "source_type", profile.source_type));
                profile.url_env = readOrDefault<std::string>(node, "url_env", profile.url_env);
                profile.display_name = readOrDefault<std::string>(node, "display_name", profile.id);
                profile.transport = toLower(readOrDefault<std::string>(node, "transport", profile.transport));
                profile.enabled = readOrDefault<bool>(node, "enabled", profile.enabled);
                profile.version = std::max(1, readOrDefault<int>(node, "version", profile.version));
                profile.created_at_ms = readOrDefault<long long>(node, "created_at_ms", 0);
                profile.updated_at_ms = readOrDefault<long long>(node, "updated_at_ms", 0);
                profile.deleted_at_ms = readOrDefault<long long>(node, "deleted_at_ms", 0);

                if (profile.deleted_at_ms != 0) {
                    continue;
                }

                if (profile.id.empty()) {
                    continue;
                }
                if (profile.source_type != "rtsp") {
                    error = "camera profile '" + profile.id + "' has unsupported source_type";
                    continue;
                }
                if (profile.url_env.empty()) {
                    error = "camera profile '" + profile.id + "' is missing url_env";
                    continue;
                }
                if (profile.transport != "tcp" && profile.transport != "udp") {
                    profile.transport = "tcp";
                }
                profiles[profile.id] = profile;
            }
        }
        catch (const std::exception& e) {
            error = std::string("failed to load camera profiles: ") + e.what();
        }
        return profiles;
    }

    bool resolveCameraProfileUri(
        const CameraProfile& profile,
        std::string& uri,
        std::string& error
    ) {
        uri.clear();
        error.clear();
        if (!profile.enabled) {
            error = "camera profile is disabled";
            return false;
        }
        if (profile.url_env.empty()) {
            error = "camera profile secret reference is missing";
            return false;
        }

        const char* value = std::getenv(profile.url_env.c_str());
        if (value == nullptr || *value == '\0') {
            error = "camera profile secret is missing from the process environment";
            return false;
        }
        uri.assign(value);
        return true;
    }

}  // namespace yolo11_server
