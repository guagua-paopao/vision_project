#include "server/camera_algorithm_processor.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "business/line_crossing_counter.h"
#include "business/person_detector_adapter.h"
#include "business/person_tracker.h"
#include "business/security_live_pipeline.h"

namespace yolo11_server {

namespace {

using json = nlohmann::json;

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string sha256Hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

bool contains(const std::set<std::string>& values, const std::string& value) {
    return values.find(value) != values.end();
}

bool wantsSecurityCategory(
    const std::set<std::string>& algorithms,
    SecurityEventCategory category
) {
    if (contains(algorithms, "security")) return true;
    switch (category) {
    case SecurityEventCategory::Zone:
        return contains(algorithms, "electronic_fence");
    case SecurityEventCategory::PoseAction:
        return contains(algorithms, "pose_action");
    case SecurityEventCategory::TemporalAction:
        return contains(algorithms, "temporal_action");
    }
    return false;
}

SecurityAlertEventRecord lineAlert(
    const AppConfig& config,
    const CameraInferenceResult& inference,
    const CrossingEvent& event
) {
    const std::string identity = inference.job.task_id + "|" + inference.job.run_id +
        "|people_flow|" + event.event_id;
    SecurityAlertEventRecord alert;
    alert.event_id = "ae_" + sha256Hex(identity);
    alert.task_id = inference.job.task_id;
    alert.run_id = inference.job.run_id;
    alert.camera_profile = inference.job.camera_profile;
    alert.event_type = event.direction == "IN" ? "PEOPLE_FLOW_IN" : "PEOPLE_FLOW_OUT";
    alert.category = "people_flow";
    alert.severity = 1;
    alert.confidence = std::clamp(event.confidence, 0.0, 1.0);
    alert.track_id = event.track_id;
    alert.occurred_at_ms = event.event_time_ms;
    alert.algorithm_profile = inference.job.algorithm_profile;
    alert.model_name = inference.output.model_type.empty()
        ? config.model.type : inference.output.model_type;
    alert.config_version = config.people_flow.config_version.empty()
        ? inference.job.algorithm_profile : config.people_flow.config_version;
    alert.payload_json = json{
        { "schema_version", "1.0" },
        { "direction", event.direction },
        { "line_id", event.line_id },
        { "point_x_norm", event.point_x_norm },
        { "point_y_norm", event.point_y_norm },
        { "source_sequence", inference.job.source_sequence }
    }.dump();
    alert.fingerprint = sha256Hex(identity);
    alert.delivery_status = inference.job.callback_profile.empty()
        ? "not_scheduled" : "pending";
    alert.created_at_ms = wallNowMs();
    return alert;
}

SecurityAlertEventRecord securityAlert(
    const AppConfig& config,
    const CameraInferenceResult& inference,
    const SecurityEvent& event
) {
    const std::string category = securityEventCategoryName(event.category);
    const std::string identity = inference.job.task_id + "|" + inference.job.run_id +
        "|" + category + "|" + event.event_id;
    SecurityAlertEventRecord alert;
    alert.event_id = "ae_" + sha256Hex(identity);
    alert.task_id = inference.job.task_id;
    alert.run_id = inference.job.run_id;
    alert.camera_profile = inference.job.camera_profile;
    alert.event_type = event.event_type;
    alert.category = category;
    alert.severity = std::clamp(event.severity, 1, 5);
    alert.confidence = std::clamp(event.confidence, 0.0, 1.0);
    alert.track_id = event.track_id;
    alert.occurred_at_ms = event.event_time_ms;
    alert.algorithm_profile = inference.job.algorithm_profile;
    alert.model_name = inference.output.model_type.empty()
        ? config.model.type : inference.output.model_type;
    alert.config_version = config.people_flow.config_version.empty()
        ? inference.job.algorithm_profile : config.people_flow.config_version;
    alert.demo_classifier = event.demo_classifier;
    alert.payload_json = json{
        { "schema_version", "1.0" },
        { "zone_id", event.zone_id },
        { "start_time_ms", event.start_time_ms },
        { "end_time_ms", event.end_time_ms },
        { "source_sequence", inference.job.source_sequence }
    }.dump();
    alert.fingerprint = sha256Hex(identity);
    alert.delivery_status = inference.job.callback_profile.empty()
        ? "not_scheduled" : "pending";
    alert.created_at_ms = wallNowMs();
    return alert;
}

}  // namespace

struct CameraAlgorithmProcessor::Session {
    Session(const AppConfig& config, const CameraFrameJob& job)
        : run_id(job.run_id),
        algorithm_profile(job.algorithm_profile),
        callback_profile(job.callback_profile),
        algorithms(job.algorithms.begin(), job.algorithms.end()),
        adapter(config.people_flow),
        tracker(config.people_flow.tracker),
        counter(
            config.people_flow.counting,
            job.run_id,
            job.task_id,
            config.people_flow.config_version.empty()
                ? job.algorithm_profile : config.people_flow.config_version,
            config.people_flow.initial_occupancy),
        warmup_remaining(config.people_flow.warmup_frames_after_reconnect) {
        const bool needs_security = contains(algorithms, "security") ||
            contains(algorithms, "electronic_fence") ||
            contains(algorithms, "pose_action") ||
            contains(algorithms, "temporal_action");
        if (needs_security && config.people_flow.security.enabled) {
            security = std::make_unique<SecurityLivePipeline>(
                config.people_flow.security, job.run_id, job.task_id);
        }
    }

    std::string run_id;
    std::string algorithm_profile;
    std::string callback_profile;
    std::set<std::string> algorithms;
    PersonDetectorAdapter adapter;
    PersonTracker tracker;
    LineCrossingCounter counter;
    std::unique_ptr<SecurityLivePipeline> security;
    int warmup_remaining = 0;
    bool active = true;
    std::mutex mutex;
};

CameraAlgorithmProcessor::CameraAlgorithmProcessor(
    AppConfig config,
    std::shared_ptr<CameraTaskRepository> repository
) : config_(std::move(config)),
    repository_(std::move(repository)) {
}

CameraAlgorithmProcessor::~CameraAlgorithmProcessor() noexcept {
    stop();
}

bool CameraAlgorithmProcessor::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (!repository_) {
        error = "camera algorithm repository is unavailable";
        return false;
    }
    if (!repository_->initialize(error)) return false;
    running_.store(true);
    return true;
}

void CameraAlgorithmProcessor::stop() noexcept {
    running_.store(false);
    try {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& entry : sessions_) {
            std::lock_guard<std::mutex> session_lock(entry.second->mutex);
            entry.second->active = false;
        }
        sessions_.clear();
    }
    catch (...) {
    }
}

std::shared_ptr<CameraAlgorithmProcessor::Session> CameraAlgorithmProcessor::sessionFor(
    const CameraFrameJob& job,
    std::string& error
) {
    const std::set<std::string> supported(
        config_.analysis.supported_algorithms.begin(),
        config_.analysis.supported_algorithms.end());
    for (const auto& algorithm : job.algorithms) {
        if (!contains(supported, algorithm)) {
            error = "UNSUPPORTED_ALGORITHM:" + algorithm;
            return {};
        }
    }
    const bool needs_security =
        std::find_if(job.algorithms.begin(), job.algorithms.end(), [](const std::string& value) {
            return value == "security" || value == "electronic_fence" ||
                value == "pose_action" || value == "temporal_action";
        }) != job.algorithms.end();
    if (needs_security && !config_.people_flow.security.enabled) {
        error = "SECURITY_ANALYTICS_DISABLED";
        return {};
    }

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto existing = sessions_.find(job.task_id);
    if (existing != sessions_.end() && existing->second->run_id == job.run_id) {
        if (existing->second->algorithm_profile != job.algorithm_profile ||
            existing->second->callback_profile != job.callback_profile ||
            existing->second->algorithms !=
                std::set<std::string>(job.algorithms.begin(), job.algorithms.end())) {
            error = "ANALYSIS_DEFINITION_CHANGED_WITHIN_RUN";
            return {};
        }
        return existing->second;
    }
    if (existing != sessions_.end()) {
        std::lock_guard<std::mutex> session_lock(existing->second->mutex);
        existing->second->active = false;
        sessions_.erase(existing);
    }
    auto session = std::make_shared<Session>(config_, job);
    sessions_[job.task_id] = session;
    return session;
}

bool CameraAlgorithmProcessor::handle(
    const CameraInferenceResult& inference,
    std::string& error
) {
    error.clear();
    if (!running_.load()) {
        error = "ALGORITHM_PROCESSOR_NOT_RUNNING";
        return false;
    }
    auto session = sessionFor(inference.job, error);
    if (!session) {
        ++failed_frames_;
        return false;
    }

    std::vector<SecurityAlertEventRecord> alerts;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->active || session->run_id != inference.job.run_id) {
            error = "STALE_ANALYSIS_SESSION";
            ++failed_frames_;
            return false;
        }
        const cv::Size frame_size = inference.job.frame->image.size();
        const auto detections = session->adapter.filter(
            inference.output, frame_size, inference.job.capture_time_ms);
        session->tracker.update(
            detections,
            frame_size.width,
            frame_size.height,
            inference.job.capture_time_ms);
        const auto tracks = session->tracker.confirmedTracks();

        if (contains(session->algorithms, "people_flow")) {
            if (session->warmup_remaining > 0) {
                --session->warmup_remaining;
            }
            else {
                const auto events = session->counter.update(
                    tracks,
                    frame_size.width,
                    frame_size.height,
                    inference.job.capture_time_ms);
                for (const auto& event : events) {
                    alerts.push_back(lineAlert(config_, inference, event));
                }
            }
        }
        if (session->security) {
            const auto security = session->security->update(
                tracks,
                inference.output,
                frame_size,
                inference.job.capture_time_ms);
            for (const auto& event : security.new_events) {
                if (wantsSecurityCategory(session->algorithms, event.category)) {
                    alerts.push_back(securityAlert(config_, inference, event));
                }
            }
        }
    }

    for (const auto& alert : alerts) {
        std::string code;
        std::string repository_error;
        if (repository_->insertAlert(
                alert,
                inference.job.callback_profile,
                code,
                repository_error)) {
            ++persisted_alerts_;
            continue;
        }
        if (code == "ALERT_ALREADY_EXISTS") {
            ++duplicate_alerts_;
            continue;
        }
        error = code.empty() ? repository_error : code + ":" + repository_error;
        ++failed_frames_;
        return false;
    }
    ++processed_frames_;
    return true;
}

void CameraAlgorithmProcessor::detachCamera(
    const std::string& task_id,
    const std::string& run_id
) noexcept {
    try {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            const auto found = sessions_.find(task_id);
            if (found == sessions_.end() || found->second->run_id != run_id) return;
            session = found->second;
            sessions_.erase(found);
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->active = false;
    }
    catch (...) {
    }
}

CameraAlgorithmProcessorSnapshot CameraAlgorithmProcessor::snapshot() const {
    CameraAlgorithmProcessorSnapshot result;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        result.active_sessions = sessions_.size();
    }
    result.processed_frames = processed_frames_.load();
    result.persisted_alerts = persisted_alerts_.load();
    result.duplicate_alerts = duplicate_alerts_.load();
    result.failed_frames = failed_frames_.load();
    return result;
}

}  // namespace yolo11_server
