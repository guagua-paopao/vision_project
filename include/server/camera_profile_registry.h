#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "server/camera_profile.h"

namespace yolo11_server {

struct CameraProfilePatch {
    std::optional<std::string> display_name;
    std::optional<std::string> url_env;
    std::optional<std::string> transport;
    std::optional<bool> enabled;
};

class CameraProfileRegistry final {
public:
    explicit CameraProfileRegistry(std::string yaml_path);

    bool initialize(std::string& error);
    bool list(bool include_deleted, std::vector<CameraProfile>& profiles, std::string& error) const;
    bool get(
        const std::string& profile_id,
        bool include_deleted,
        CameraProfile& profile,
        bool& found,
        std::string& error) const;
    bool create(CameraProfile profile, std::string& code, std::string& error);
    bool update(
        const std::string& profile_id,
        int expected_version,
        const CameraProfilePatch& patch,
        CameraProfile& profile,
        std::string& code,
        std::string& error);
    bool softDelete(
        const std::string& profile_id,
        int expected_version,
        std::string& code,
        std::string& error);

    static bool validIdentifier(const std::string& value);
    static bool validEnvironmentName(const std::string& value);
    static bool validProfile(const CameraProfile& profile);

private:
    bool loadLocked(std::map<std::string, CameraProfile>& profiles, std::string& error) const;
    bool saveLocked(const std::map<std::string, CameraProfile>& profiles, std::string& error) const;

    std::string yaml_path_;
    mutable std::mutex mutex_;
};

}  // namespace yolo11_server
