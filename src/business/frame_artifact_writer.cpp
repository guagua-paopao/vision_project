#include "business/frame_artifact_writer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace yolo11_server {

namespace {

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool safeIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 160) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
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

bool ensureManagedDirectory(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::filesystem::path& result,
    std::string& error
) {
    result = root;
    for (const auto& component : relative) {
        if (component == "." || component == ".." || component.empty()) {
            error = "unsafe managed output path";
            return false;
        }
        result /= component;
        std::error_code fs_error;
        if (std::filesystem::exists(result, fs_error)) {
            if (fs_error || isReparsePoint(result) || !std::filesystem::is_directory(result, fs_error)) {
                error = "managed output directory is not a regular directory";
                return false;
            }
        }
        else if (!std::filesystem::create_directory(result, fs_error) || fs_error) {
            error = fs_error ? fs_error.message() : "failed to create managed output directory";
            return false;
        }
    }
    return true;
}

bool atomicPublish(
    const std::filesystem::path& final_path,
    const std::filesystem::path& temp_path,
    const std::vector<unsigned char>& bytes,
    std::string& error
) {
    if (isReparsePoint(final_path) || isReparsePoint(temp_path)) {
        error = "managed output file is a reparse point";
        return false;
    }
    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to open temporary JPEG";
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            error = "failed to write temporary JPEG";
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temp_path, ignored);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "failed to atomically publish JPEG, win32_error=" + std::to_string(GetLastError());
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return false;
    }
#else
    std::error_code fs_error;
    std::filesystem::rename(temp_path, final_path, fs_error);
    if (fs_error) {
        error = fs_error.message();
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return false;
    }
#endif
    return true;
}

struct UtcPathParts {
    std::string year;
    std::string month;
    std::string day;
    std::string timestamp;
};

UtcPathParts utcParts(long long time_ms) {
    const std::time_t seconds = static_cast<std::time_t>(time_ms / 1000);
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &seconds);
#else
    gmtime_r(&seconds, &value);
#endif
    std::ostringstream date;
    date << std::setfill('0') << std::setw(4) << value.tm_year + 1900
         << std::setw(2) << value.tm_mon + 1 << std::setw(2) << value.tm_mday
         << 'T' << std::setw(2) << value.tm_hour << std::setw(2) << value.tm_min
         << std::setw(2) << value.tm_sec << '.' << std::setw(3) << (time_ms % 1000) << 'Z';
    UtcPathParts result;
    result.year = date.str().substr(0, 4);
    result.month = date.str().substr(4, 2);
    result.day = date.str().substr(6, 2);
    result.timestamp = date.str();
    return result;
}

}  // namespace

FrameArtifactWriter::FrameArtifactWriter(
    const CameraTasksSection& config,
    std::shared_ptr<CameraTaskRepository> repository
) : config_(config), repository_(std::move(repository)) {
}

FrameArtifactWriter::~FrameArtifactWriter() noexcept {
    stop();
}

bool FrameArtifactWriter::start(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (accepting_) return true;
    if (!repository_) {
        error = "camera artifact repository is unavailable";
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
    accepting_ = true;
    stopping_ = false;
    try {
        const int count = std::max(1, config_.writer_threads);
        for (int index = 0; index < count; ++index) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }
    catch (const std::exception& exception) {
        accepting_ = false;
        stopping_ = true;
        work_cv_.notify_all();
        error = exception.what();
        return false;
    }
    return true;
}

void FrameArtifactWriter::stop() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ && workers_.empty()) return;
            accepting_ = false;
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        in_flight_per_run_.clear();
        stopping_ = false;
        idle_cv_.notify_all();
    }
    catch (...) {
    }
}

bool FrameArtifactWriter::enqueue(FrameArtifactJob job, FrameArtifactCompletion completion) {
    if (!job.frame || !safeIdentifier(job.task_id) || !safeIdentifier(job.run_id) ||
        (job.output_mode != "latest" && job.output_mode != "archive" && job.output_mode != "both")) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto run_depth = in_flight_per_run_[job.run_id];
    std::size_t total_depth = 0;
    for (const auto& entry : in_flight_per_run_) total_depth += entry.second;
    if (!accepting_ || total_depth >= static_cast<std::size_t>(std::max(1, config_.writer_queue_capacity)) ||
        run_depth >= static_cast<std::size_t>(std::max(1, config_.writer_queue_capacity_per_run))) {
        if (run_depth == 0) in_flight_per_run_.erase(job.run_id);
        return false;
    }
    ++in_flight_per_run_[job.run_id];
    queue_.push_back({ std::move(job), std::move(completion) });
    work_cv_.notify_one();
    return true;
}

bool FrameArtifactWriter::waitForRunIdle(const std::string& run_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [&]() {
        const auto found = in_flight_per_run_.find(run_id);
        return found == in_flight_per_run_.end() || found->second == 0;
    });
}

std::size_t FrameArtifactWriter::queueDepth(const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!run_id.empty()) {
        const auto found = in_flight_per_run_.find(run_id);
        return found == in_flight_per_run_.end() ? 0 : found->second;
    }
    std::size_t result = 0;
    for (const auto& entry : in_flight_per_run_) result += entry.second;
    return result;
}

FrameArtifactWriterMetrics FrameArtifactWriter::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FrameArtifactWriterMetrics result;
    for (const auto& entry : in_flight_per_run_) result.queue_depth += entry.second;
    result.encoded_jobs = encoded_jobs_;
    result.completed_jobs = completed_jobs_;
    result.failed_jobs = failed_jobs_;
    result.orphaned_artifacts = orphaned_artifacts_;
    result.storage_pressure_rejections = storage_pressure_rejections_;
    return result;
}

std::filesystem::path FrameArtifactWriter::outputRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_root_;
}

void FrameArtifactWriter::workerLoop() noexcept {
    for (;;) {
        PendingJob pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [&]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) return;
            pending = std::move(queue_.front());
            queue_.pop_front();
        }
        FrameArtifactWriteResult result = write(pending.job);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = in_flight_per_run_.find(pending.job.run_id);
            if (found != in_flight_per_run_.end()) {
                if (found->second > 0) --found->second;
                if (found->second == 0) in_flight_per_run_.erase(found);
            }
            if (result.success) ++completed_jobs_;
            else ++failed_jobs_;
            idle_cv_.notify_all();
        }
        try {
            if (pending.completion) pending.completion(result);
        }
        catch (...) {
        }
    }
}

FrameArtifactWriteResult FrameArtifactWriter::write(const FrameArtifactJob& job) noexcept {
    FrameArtifactWriteResult result;
    result.save_time_ms = wallNowMs();
    try {
        cv::Mat output = job.frame->image;
        if (output.empty()) {
            result.error_code = "FRAME_EMPTY";
            result.error_message = "camera frame is empty";
            return result;
        }
        double scale = 1.0;
        if (job.max_width > 0 && output.cols > job.max_width) {
            scale = std::min(scale, static_cast<double>(job.max_width) / output.cols);
        }
        if (job.max_height > 0 && output.rows > job.max_height) {
            scale = std::min(scale, static_cast<double>(job.max_height) / output.rows);
        }
        cv::Mat resized;
        if (scale < 1.0) {
            cv::resize(output, resized, cv::Size(), scale, scale, cv::INTER_AREA);
            output = resized;
        }
        result.width = output.cols;
        result.height = output.rows;
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".jpg", output, encoded,
            { cv::IMWRITE_JPEG_QUALITY, std::clamp(job.jpeg_quality, 1, 100) })) {
            result.error_code = "JPEG_ENCODE_FAILED";
            result.error_message = "OpenCV JPEG encoder returned false";
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++encoded_jobs_;
        }

        const bool write_archive = job.output_mode == "archive" || job.output_mode == "both";
        const bool write_latest = job.output_mode == "latest" || job.output_mode == "both";
        std::unique_lock<std::mutex> storage_guard;
        if (write_archive) {
            storage_guard = std::unique_lock<std::mutex>(storage_guard_mutex_);
            if (!archiveAdmissionAllowed(encoded.size(), result.error_message)) {
                result.error_code = "STORAGE_PRESSURE";
                std::lock_guard<std::mutex> lock(mutex_);
                ++storage_pressure_rejections_;
                return result;
            }
            const UtcPathParts utc = utcParts(job.frame->capture_time_ms);
            const std::filesystem::path relative_directory = std::filesystem::u8path(job.task_id) /
                "archive" / std::filesystem::u8path(job.run_id) / utc.year / utc.month / utc.day;
            std::filesystem::path directory;
            if (!ensureManagedDirectory(output_root_, relative_directory, directory, result.error_message)) {
                result.error_code = "OUTPUT_PATH_UNSAFE";
                return result;
            }
            std::ostringstream name;
            name << utc.timestamp << '_' << std::setfill('0') << std::setw(10)
                 << job.frame->sequence << ".jpg";
            const auto final_path = directory / name.str();
            const auto temp_path = directory / (name.str() + "." + job.run_id + ".tmp");
            if (!atomicPublish(final_path, temp_path, encoded, result.error_message)) {
                result.error_code = "ARCHIVE_PUBLISH_FAILED";
                return result;
            }
            CameraFrameArtifact artifact;
            artifact.frame_id = "cf_" + job.run_id + "_" + std::to_string(job.frame->sequence);
            artifact.task_id = job.task_id;
            artifact.run_id = job.run_id;
            artifact.source_sequence = job.frame->sequence;
            artifact.capture_time_ms = job.frame->capture_time_ms;
            artifact.save_time_ms = result.save_time_ms;
            artifact.relative_path = pathUtf8(relative_directory / name.str());
            artifact.width = result.width;
            artifact.height = result.height;
            artifact.size_bytes = static_cast<long long>(encoded.size());
            std::string repository_error;
            if (!repository_->insertFrame(artifact, repository_error)) {
                recordOrphan(artifact, repository_error);
                result.error_code = "FRAME_METADATA_WRITE_FAILED";
                result.error_message = repository_error;
                return result;
            }
            result.archive_published = true;
            result.artifact = artifact;
            storage_guard.unlock();
        }
        if (write_latest) {
            std::filesystem::path directory;
            if (!ensureManagedDirectory(output_root_, std::filesystem::u8path(job.task_id),
                directory, result.error_message)) {
                result.error_code = "OUTPUT_PATH_UNSAFE";
                return result;
            }
            const auto final_path = directory / "latest.jpg";
            const auto temp_path = directory / ("latest." + job.run_id + ".tmp");
            if (!atomicPublish(final_path, temp_path, encoded, result.error_message)) {
                result.error_code = "LATEST_PUBLISH_FAILED";
                return result;
            }
            result.latest_published = true;
        }
        result.success = true;
        return result;
    }
    catch (const std::exception& exception) {
        result.error_code = "FRAME_ARTIFACT_WRITE_FAILED";
        result.error_message = exception.what();
        return result;
    }
    catch (...) {
        result.error_code = "FRAME_ARTIFACT_WRITE_FAILED";
        result.error_message = "unknown frame artifact writer error";
        return result;
    }
}

bool FrameArtifactWriter::archiveAdmissionAllowed(
    std::size_t encoded_bytes,
    std::string& error
) const {
    error.clear();
    CameraTaskRepositoryStats stats;
    if (!repository_->stats(stats, error)) {
        error = "camera archive accounting is unavailable";
        return false;
    }
    if (config_.storage.max_archive_bytes > 0) {
        const long long critical_limit = config_.storage.max_archive_bytes *
            config_.storage.critical_watermark_percent / 100;
        if (stats.archive_bytes + static_cast<long long>(encoded_bytes) > critical_limit) {
            error = "camera archive quota reached critical watermark";
            return false;
        }
    }
    if (config_.storage.min_free_bytes > 0) {
        std::error_code fs_error;
        const auto space = std::filesystem::space(output_root_, fs_error);
        if (fs_error) {
            error = "camera output filesystem space is unavailable";
            return false;
        }
        if (static_cast<long long>(space.available) < config_.storage.min_free_bytes +
            static_cast<long long>(encoded_bytes)) {
            error = "camera output filesystem reached minimum free-space reserve";
            return false;
        }
    }
    return true;
}

void FrameArtifactWriter::recordOrphan(
    const CameraFrameArtifact& artifact,
    const std::string& reason
) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++orphaned_artifacts_;
        }
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        std::ofstream output(output_root_ / "orphaned_artifacts.log", std::ios::app);
        if (!output) return;
        std::string clean_reason = reason;
        std::replace(clean_reason.begin(), clean_reason.end(), '\n', ' ');
        std::replace(clean_reason.begin(), clean_reason.end(), '\r', ' ');
        output << artifact.frame_id << '\t' << artifact.relative_path << '\t' << clean_reason << '\n';
    }
    catch (...) {
    }
}

}  // namespace yolo11_server
