#include "server/camera_profile_registry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <yaml-cpp/yaml.h>

#include "server/yaml_file_loader.h"

namespace yolo11_server {

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename T>
T readOrDefault(const YAML::Node& node, const std::string& key, const T& fallback) {
    return node && node[key] ? node[key].as<T>() : fallback;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "failed to atomically replace camera profile registry, win32_error=" +
            std::to_string(GetLastError());
        return false;
    }
#else
    std::error_code fs_error;
    std::filesystem::rename(source, destination, fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
#endif
    return true;
}

}  // namespace

CameraProfileRegistry::CameraProfileRegistry(std::string yaml_path)
    : yaml_path_(std::move(yaml_path)) {
}

bool CameraProfileRegistry::initialize(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, CameraProfile> profiles;
    return loadLocked(profiles, error);
}

bool CameraProfileRegistry::list(
    bool include_deleted,
    std::vector<CameraProfile>& profiles,
    std::string& error
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, CameraProfile> loaded;
    if (!loadLocked(loaded, error)) return false;
    profiles.clear();
    for (const auto& entry : loaded) {
        if (include_deleted || entry.second.deleted_at_ms == 0) profiles.push_back(entry.second);
    }
    return true;
}

bool CameraProfileRegistry::get(
    const std::string& profile_id,
    bool include_deleted,
    CameraProfile& profile,
    bool& found,
    std::string& error
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, CameraProfile> loaded;
    if (!loadLocked(loaded, error)) return false;
    const auto item = loaded.find(profile_id);
    found = item != loaded.end() && (include_deleted || item->second.deleted_at_ms == 0);
    if (found) profile = item->second;
    return true;
}

bool CameraProfileRegistry::create(
    CameraProfile profile,
    std::string& code,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    code.clear();
    std::map<std::string, CameraProfile> loaded;
    if (!loadLocked(loaded, error)) return false;
    if (!validProfile(profile)) {
        code = "INVALID_CAMERA_PROFILE";
        return false;
    }
    if (loaded.find(profile.id) != loaded.end()) {
        code = "CAMERA_PROFILE_ALREADY_EXISTS";
        return false;
    }
    profile.source_type = "rtsp";
    profile.transport = lower(profile.transport);
    profile.version = 1;
    profile.created_at_ms = nowMs();
    profile.updated_at_ms = profile.created_at_ms;
    profile.deleted_at_ms = 0;
    loaded.emplace(profile.id, profile);
    return saveLocked(loaded, error);
}

bool CameraProfileRegistry::update(
    const std::string& profile_id,
    int expected_version,
    const CameraProfilePatch& patch,
    CameraProfile& profile,
    std::string& code,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    code.clear();
    std::map<std::string, CameraProfile> loaded;
    if (!loadLocked(loaded, error)) return false;
    const auto found = loaded.find(profile_id);
    if (found == loaded.end() || found->second.deleted_at_ms != 0) {
        code = "CAMERA_PROFILE_NOT_FOUND";
        return false;
    }
    if (found->second.version != expected_version) {
        code = "CAMERA_PROFILE_VERSION_CONFLICT";
        return false;
    }
    profile = found->second;
    if (patch.display_name) profile.display_name = *patch.display_name;
    if (patch.url_env) profile.url_env = *patch.url_env;
    if (patch.transport) profile.transport = lower(*patch.transport);
    if (patch.enabled) profile.enabled = *patch.enabled;
    if (!validProfile(profile)) {
        code = "INVALID_CAMERA_PROFILE";
        return false;
    }
    ++profile.version;
    profile.updated_at_ms = nowMs();
    loaded[profile_id] = profile;
    return saveLocked(loaded, error);
}

bool CameraProfileRegistry::softDelete(
    const std::string& profile_id,
    int expected_version,
    std::string& code,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    code.clear();
    std::map<std::string, CameraProfile> loaded;
    if (!loadLocked(loaded, error)) return false;
    const auto found = loaded.find(profile_id);
    if (found == loaded.end() || found->second.deleted_at_ms != 0) {
        code = "CAMERA_PROFILE_NOT_FOUND";
        return false;
    }
    if (found->second.version != expected_version) {
        code = "CAMERA_PROFILE_VERSION_CONFLICT";
        return false;
    }
    found->second.enabled = false;
    ++found->second.version;
    found->second.updated_at_ms = nowMs();
    found->second.deleted_at_ms = found->second.updated_at_ms;
    return saveLocked(loaded, error);
}

bool CameraProfileRegistry::validIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool CameraProfileRegistry::validEnvironmentName(const std::string& value) {
    static const std::regex pattern("^[A-Z][A-Z0-9_]{1,127}$");
    return std::regex_match(value, pattern);
}

bool CameraProfileRegistry::validProfile(const CameraProfile& profile) {
    const auto transport = lower(profile.transport);
    return validIdentifier(profile.id) && profile.source_type == "rtsp" &&
        validEnvironmentName(profile.url_env) && !profile.display_name.empty() &&
        profile.display_name.size() <= 256 && (transport == "tcp" || transport == "udp");
}

bool CameraProfileRegistry::loadLocked(
    std::map<std::string, CameraProfile>& profiles,
    std::string& error
) const {
    profiles.clear();
    error.clear();
    if (yaml_path_.empty()) {
        error = "camera profile registry path is empty";
        return false;
    }
    try {
        const auto root = loadYamlFileAbiSafe(yaml_path_);
        const auto cameras = root["cameras"];
        if (!cameras || !cameras.IsMap()) {
            error = "camera profile file must contain a cameras map";
            return false;
        }
        for (const auto& entry : cameras) {
            CameraProfile profile;
            profile.id = entry.first.as<std::string>();
            const auto node = entry.second;
            profile.source_type = lower(readOrDefault<std::string>(node, "source_type", "rtsp"));
            profile.url_env = readOrDefault<std::string>(node, "url_env", "");
            profile.display_name = readOrDefault<std::string>(node, "display_name", profile.id);
            profile.transport = lower(readOrDefault<std::string>(node, "transport", "tcp"));
            profile.enabled = readOrDefault<bool>(node, "enabled", true);
            profile.version = std::max(1, readOrDefault<int>(node, "version", 1));
            profile.created_at_ms = readOrDefault<long long>(node, "created_at_ms", 0);
            profile.updated_at_ms = readOrDefault<long long>(node, "updated_at_ms", 0);
            profile.deleted_at_ms = readOrDefault<long long>(node, "deleted_at_ms", 0);
            if (!validProfile(profile)) {
                error = "invalid camera profile metadata: " + profile.id;
                return false;
            }
            profiles.emplace(profile.id, std::move(profile));
        }
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool CameraProfileRegistry::saveLocked(
    const std::map<std::string, CameraProfile>& profiles,
    std::string& error
) const {
    error.clear();
    try {
        YAML::Emitter output;
        output << YAML::BeginMap << YAML::Key << "cameras" << YAML::Value << YAML::BeginMap;
        for (const auto& entry : profiles) {
            const auto& profile = entry.second;
            output << YAML::Key << profile.id << YAML::Value << YAML::BeginMap
                   << YAML::Key << "source_type" << YAML::Value << "rtsp"
                   << YAML::Key << "url_env" << YAML::Value << profile.url_env
                   << YAML::Key << "display_name" << YAML::Value << profile.display_name
                   << YAML::Key << "transport" << YAML::Value << profile.transport
                   << YAML::Key << "enabled" << YAML::Value << profile.enabled
                   << YAML::Key << "version" << YAML::Value << profile.version
                   << YAML::Key << "created_at_ms" << YAML::Value << profile.created_at_ms
                   << YAML::Key << "updated_at_ms" << YAML::Value << profile.updated_at_ms
                   << YAML::Key << "deleted_at_ms" << YAML::Value << profile.deleted_at_ms
                   << YAML::EndMap;
        }
        output << YAML::EndMap << YAML::EndMap;
        if (!output.good()) {
            error = output.GetLastError();
            return false;
        }
        const auto destination = std::filesystem::absolute(std::filesystem::u8path(yaml_path_));
        std::error_code fs_error;
        std::filesystem::create_directories(destination.parent_path(), fs_error);
        if (fs_error) {
            error = fs_error.message();
            return false;
        }
        const auto temporary = destination.parent_path() /
            (destination.filename().string() + ".tmp." + std::to_string(nowMs()));
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << output.c_str() << '\n';
            stream.flush();
            if (!stream) {
                error = "failed to write camera profile registry temporary file";
                stream.close();
                std::filesystem::remove(temporary, fs_error);
                return false;
            }
        }
        if (!replaceFile(temporary, destination, error)) {
            std::filesystem::remove(temporary, fs_error);
            return false;
        }
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

}  // namespace yolo11_server
