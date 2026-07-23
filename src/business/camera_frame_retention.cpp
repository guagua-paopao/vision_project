#include "business/camera_frame_retention.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <spdlog/spdlog.h>

namespace yolo11_server {

namespace {

bool safeIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 160) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool isReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

bool pathIsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto relative = child.lexically_relative(parent);
    if (relative.empty() && child != parent) return false;
    for (const auto& component : relative) if (component == "..") return false;
    return !relative.is_absolute();
}

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

CameraFrameRetentionSweeper::CameraFrameRetentionSweeper(
    const CameraTasksSection& config,
    std::shared_ptr<CameraTaskRepository> repository
) : config_(config), repository_(std::move(repository)) {
}

CameraFrameRetentionSweeper::~CameraFrameRetentionSweeper() noexcept {
    stop();
}

bool CameraFrameRetentionSweeper::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (!repository_) {
        error = "camera retention repository is unavailable";
        return false;
    }
    std::error_code fs_error;
    const auto configured = std::filesystem::absolute(std::filesystem::u8path(config_.output_dir), fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    std::filesystem::create_directories(configured, fs_error);
    if (fs_error || isReparsePoint(configured)) {
        error = fs_error ? fs_error.message() : "camera output root must not be a reparse point";
        return false;
    }
    output_root_ = std::filesystem::weakly_canonical(configured, fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    running_.store(true);
    try {
        thread_ = std::thread([this]() { loop(); });
    }
    catch (const std::exception& exception) {
        running_.store(false);
        error = exception.what();
        return false;
    }
    return true;
}

void CameraFrameRetentionSweeper::stop() noexcept {
    if (!running_.exchange(false)) return;
    try {
        if (thread_.joinable()) thread_.join();
    }
    catch (...) {
    }
}

bool CameraFrameRetentionSweeper::sweepOnce(
    long long now_ms,
    int& deleted_count,
    std::string& error
) {
    deleted_count = 0;
    std::vector<CameraFrameArtifact> candidates;
    if (!repository_->listRetentionCandidates(
        now_ms, std::max(1, config_.retention_batch_size), candidates, error)) return false;
    for (const auto& frame : candidates) {
        std::string remove_error;
        if (!removeManagedArchive(frame, remove_error)) {
            if (error.empty()) error = remove_error;
            continue;
        }
        std::string repository_error;
        if (!repository_->deleteFrameMetadata(frame.frame_id, repository_error)) {
            if (error.empty()) error = repository_error;
            continue;
        }
        ++deleted_count;
    }
    CameraTaskRepositoryStats stats;
    std::string stats_error;
    if (!repository_->stats(stats, stats_error)) {
        if (error.empty()) error = stats_error;
        return false;
    }
    std::error_code fs_error;
    auto space = std::filesystem::space(output_root_, fs_error);
    if (fs_error) {
        if (error.empty()) error = fs_error.message();
        return false;
    }
    const long long quota_target = config_.storage.max_archive_bytes > 0
        ? config_.storage.max_archive_bytes * config_.storage.high_watermark_percent / 100 : 0;
    auto underPressure = [&]() {
        const bool quota_pressure = quota_target > 0 && stats.archive_bytes > quota_target;
        const bool free_pressure = config_.storage.min_free_bytes > 0 &&
            static_cast<long long>(space.available) < config_.storage.min_free_bytes;
        return quota_pressure || free_pressure;
    };
    if (underPressure()) {
        std::vector<CameraFrameArtifact> pressure_candidates;
        std::string pressure_error;
        if (!repository_->listOldestFrames(
            config_.storage.pressure_cleanup_batch_size, pressure_candidates, pressure_error)) {
            if (error.empty()) error = pressure_error;
            return false;
        }
        for (const auto& frame : pressure_candidates) {
            if (!underPressure()) break;
            std::string remove_error;
            if (!removeManagedArchive(frame, remove_error)) {
                if (error.empty()) error = remove_error;
                continue;
            }
            std::string repository_error;
            if (!repository_->deleteFrameMetadata(frame.frame_id, repository_error)) {
                if (error.empty()) error = repository_error;
                continue;
            }
            ++deleted_count;
            stats.archive_bytes = std::max(0LL, stats.archive_bytes - frame.size_bytes);
            space.available += static_cast<std::uintmax_t>(std::max(0LL, frame.size_bytes));
        }
    }
    return error.empty();
}

void CameraFrameRetentionSweeper::loop() noexcept {
    const int interval_seconds = std::max(1, config_.retention_sweep_interval_seconds);
    int elapsed = 0;
    while (running_.load()) {
        if (elapsed >= interval_seconds) {
            int deleted = 0;
            std::string error;
            if (!sweepOnce(wallNowMs(), deleted, error) && !error.empty()) {
                spdlog::warn("Camera frame retention sweep incomplete: {}", error);
            }
            elapsed = 0;
        }
        for (int index = 0; index < 10 && running_.load(); ++index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ++elapsed;
    }
}

bool CameraFrameRetentionSweeper::removeManagedArchive(
    const CameraFrameArtifact& frame,
    std::string& error
) const {
    if (!safeIdentifier(frame.task_id) || !safeIdentifier(frame.run_id)) {
        error = "retention candidate has an unsafe identifier";
        return false;
    }
    const auto relative = std::filesystem::u8path(frame.relative_path).lexically_normal();
    if (relative.empty() || relative.is_absolute()) {
        error = "retention candidate has an unsafe relative path";
        return false;
    }
    std::vector<std::filesystem::path> components;
    for (const auto& component : relative) {
        if (component == "." || component == ".." || component.empty()) {
            error = "retention candidate escapes the managed path";
            return false;
        }
        components.push_back(component);
    }
    if (components.size() < 5 || components[0] != std::filesystem::u8path(frame.task_id) ||
        components[1] != "archive" || components[2] != std::filesystem::u8path(frame.run_id)) {
        error = "retention candidate is outside the task archive root";
        return false;
    }
    const auto target = output_root_ / relative;
    const auto archive_root = output_root_ / std::filesystem::u8path(frame.task_id) / "archive";
    std::error_code fs_error;
    const auto canonical_archive = std::filesystem::weakly_canonical(archive_root, fs_error);
    if (fs_error || isReparsePoint(archive_root)) {
        error = fs_error ? fs_error.message() : "task archive root is a reparse point";
        return false;
    }
    auto current = output_root_;
    for (auto iterator = relative.begin(); iterator != relative.end(); ++iterator) {
        auto next = iterator;
        ++next;
        if (next == relative.end()) break;
        current /= *iterator;
        if (isReparsePoint(current)) {
            error = "retention refuses to follow a reparse point";
            return false;
        }
    }
    const auto canonical_parent = std::filesystem::weakly_canonical(target.parent_path(), fs_error);
    if (fs_error || !pathIsWithin(canonical_parent, canonical_archive) || isReparsePoint(target)) {
        error = fs_error ? fs_error.message() : "retention target failed canonical path validation";
        return false;
    }
    const bool removed = std::filesystem::remove(target, fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    if (!removed && std::filesystem::exists(target, fs_error)) {
        error = "retention target could not be removed";
        return false;
    }
    return true;
}

}  // namespace yolo11_server
