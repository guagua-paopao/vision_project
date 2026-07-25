#include "server/camera_algorithm_processor.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <opencv2/imgcodecs.hpp>

#include "business/line_crossing_counter.h"
#include "business/person_detector_adapter.h"
#include "business/person_tracker.h"
#include "business/people_flow_renderer.h"
#include "business/security_live_pipeline.h"
#include "business/security_overlay_renderer.h"
#include "server/camera_task_api_control.h"

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

std::string effectiveConfigVersion(
    const AppConfig& config,
    const CameraFrameJob& job
) {
    if (!job.analysis_config_version.empty()) {
        return job.analysis_config_version;
    }
    return config.people_flow.config_version.empty()
        ? job.algorithm_profile
        : config.people_flow.config_version;
}

json securityEventJson(const SecurityEvent& event) {
    return {
        { "event_id", event.event_id }, { "session_id", event.session_id },
        { "camera_id", event.camera_id }, { "track_id", event.track_id },
        { "category", securityEventCategoryName(event.category) },
        { "event_type", event.event_type }, { "zone_id", event.zone_id },
        { "start_time_ms", event.start_time_ms }, { "end_time_ms", event.end_time_ms },
        { "event_time_ms", event.event_time_ms }, { "confidence", event.confidence },
        { "severity", event.severity }, { "demo_classifier", event.demo_classifier }
    };
}

json activeActionsJson(
    const std::map<std::int64_t, std::vector<std::string>>& actions
) {
    json items = json::array();
    for (const auto& [track_id, labels] : actions) {
        items.push_back({ { "track_id", track_id }, { "labels", labels } });
    }
    return items;
}

json securityFrameJson(
    const SecurityFrameResult& frame,
    const PeopleFlowSecuritySection& config,
    const std::string& run_id,
    const std::string& task_id
) {
    int inside_count = 0;
    json zone_statuses = json::array();
    for (const TrackZoneStatus& status : frame.zone_statuses) {
        if (status.inside) ++inside_count;
        zone_statuses.push_back({
            { "zone_id", status.zone_id }, { "track_id", status.track_id },
            { "inside", status.inside }, { "entered_at_ms", status.entered_at_ms },
            { "dwell_ms", status.dwell_ms },
            { "dwell_alarm_emitted", status.dwell_alarm_emitted }
        });
    }
    json tracks = json::array();
    for (const TrackAnalyticsSnapshot& analytics : frame.track_analytics) {
        tracks.push_back({
            { "track_id", analytics.track_id },
            { "speed_px_s", analytics.instantaneous_speed_px_s },
            { "average_speed_px_s", analytics.average_speed_px_s },
            { "distance_px", analytics.cumulative_distance_px },
            { "stationary_ms", analytics.stationary_ms },
            { "stationary", analytics.stationary },
            { "loitering", analytics.loitering }
        });
    }
    json events = json::array();
    for (auto it = frame.recent_events.rbegin();
         it != frame.recent_events.rend(); ++it) {
        events.push_back(securityEventJson(*it));
    }
    return {
        { "success", true }, { "mode", "unified_camera_pipeline" },
        { "production_action_model", false }, { "run_id", run_id },
        { "camera_id", task_id }, { "timestamp_ms", frame.timestamp_ms },
        { "stages", {
            { "phase1", { { "name", "electronic_fence" }, { "ready", true },
                { "zone_count", config.zones.size() }, { "inside_count", inside_count },
                { "statuses", zone_statuses } } },
            { "phase2", { { "name", "tracking_and_analytics" }, { "ready", true },
                { "alpha_beta_filter", true }, { "track_count", tracks.size() },
                { "tracks", tracks } } },
            { "phase3", { { "name", "pose_rule_actions" }, { "ready", true },
                { "model", "yolo11-pose-tensorrt" },
                { "pose_count", frame.poses.size() },
                { "active_actions", activeActionsJson(frame.active_pose_actions) } } },
            { "phase4", { { "name", "temporal_action_demo" }, { "ready", true },
                { "classifier", "feature-threshold-demo" },
                { "demo_classifier", true },
                { "label", config.temporal_demo_label },
                { "active_actions", activeActionsJson(frame.active_temporal_actions) } } }
        } },
        { "events", events }
    };
}

json inactiveSecurityState(
    long long timestamp_ms,
    const std::string& run_id,
    const std::string& task_id
) {
    return {
        { "success", true }, { "mode", "unified_camera_pipeline" },
        { "production_action_model", false }, { "run_id", run_id },
        { "camera_id", task_id }, { "timestamp_ms", timestamp_ms },
        { "stages", {
            { "phase1", { { "name", "electronic_fence" }, { "ready", false } } },
            { "phase2", { { "name", "tracking_and_analytics" }, { "ready", true } } },
            { "phase3", { { "name", "pose_rule_actions" }, { "ready", false } } },
            { "phase4", { { "name", "temporal_action_demo" }, { "ready", false },
                { "demo_classifier", true } } }
        } },
        { "events", json::array() }
    };
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
    alert.config_version = effectiveConfigVersion(config, inference.job);
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
    alert.config_version = effectiveConfigVersion(config, inference.job);
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
        config_version(effectiveConfigVersion(config, job)),
        initial_occupancy(std::max(0LL, job.initial_occupancy)),
        snapshot_fps(job.snapshot_fps > 0
            ? job.snapshot_fps : config.people_flow.snapshot_fps),
        algorithm_parameters_json(job.algorithm_parameters_json.empty()
            ? "{}" : job.algorithm_parameters_json),
        algorithms(job.algorithms.begin(), job.algorithms.end()),
        adapter(config.people_flow),
        tracker(config.people_flow.tracker),
        counter(
            config.people_flow.counting,
            job.run_id,
            job.task_id,
            config_version,
            initial_occupancy),
        renderer(config.people_flow),
        reconnect_count(job.reconnect_count),
        warmup_remaining(config.people_flow.warmup_frames_after_reconnect) {
        const bool needs_security = contains(algorithms, "security") ||
            contains(algorithms, "electronic_fence") ||
            contains(algorithms, "pose_action") ||
            contains(algorithms, "temporal_action");
        if (needs_security && config.people_flow.security.enabled) {
            security = std::make_unique<SecurityLivePipeline>(
                config.people_flow.security, job.run_id, job.task_id);
            security_renderer = std::make_unique<SecurityOverlayRenderer>(
                config.people_flow.security);
        }
        snapshot_interval_frames = std::max(1, static_cast<int>(
            std::max(0.1, job.target_infer_fps) /
            std::max(1, snapshot_fps)));
        const auto relative = std::filesystem::path(job.task_id) /
            job.run_id / "analysis" / "latest.jpg";
        snapshot_relative_path = relative.generic_string();
        snapshot_path =
            std::filesystem::u8path(config.camera_tasks.output_dir) / relative;
        std::error_code directory_error;
        std::filesystem::create_directories(
            snapshot_path.parent_path(), directory_error);
        snapshot_degraded = static_cast<bool>(directory_error);
    }

    std::string run_id;
    std::string algorithm_profile;
    std::string callback_profile;
    std::string config_version;
    long long initial_occupancy = 0;
    int snapshot_fps = 0;
    std::string algorithm_parameters_json = "{}";
    std::set<std::string> algorithms;
    PersonDetectorAdapter adapter;
    PersonTracker tracker;
    LineCrossingCounter counter;
    PeopleFlowRenderer renderer;
    std::unique_ptr<SecurityLivePipeline> security;
    std::unique_ptr<SecurityOverlayRenderer> security_renderer;
    SecurityFrameResult security_frame;
    std::filesystem::path snapshot_path;
    std::string snapshot_relative_path;
    int snapshot_interval_frames = 1;
    long long last_snapshot_frame = 0;
    bool snapshot_degraded = false;
    bool storage_degraded = false;
    int reconnect_count = 0;
    int warmup_remaining = 0;
    long long frame_count = 0;
    long long infer_window_start_ms = 0;
    long long infer_window_frames = 0;
    double infer_fps = 0.0;
    double last_inference_ms = 0.0;
    long long last_persist_ms = 0;
    CameraRunAnalysisResultRecord latest_result;
    CameraTaskRunHotStatus latest_hot;
    bool has_snapshot = false;
    bool active = true;
    std::mutex mutex;
};

CameraAlgorithmProcessor::CameraAlgorithmProcessor(
    AppConfig config,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICameraAnalysisStatusSink> status_sink
) : config_(std::move(config)),
    repository_(std::move(repository)),
    status_sink_(std::move(status_sink)) {
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
            existing->second->config_version !=
                effectiveConfigVersion(config_, job) ||
            existing->second->initial_occupancy !=
                std::max(0LL, job.initial_occupancy) ||
            existing->second->snapshot_fps !=
                (job.snapshot_fps > 0
                    ? job.snapshot_fps
                    : config_.people_flow.snapshot_fps) ||
            existing->second->algorithm_parameters_json !=
                (job.algorithm_parameters_json.empty()
                    ? "{}"
                    : job.algorithm_parameters_json) ||
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
    CameraRunAnalysisResultRecord analysis_result;
    CameraTaskRunHotStatus hot;
    bool persist_analysis = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->active || session->run_id != inference.job.run_id) {
            error = "STALE_ANALYSIS_SESSION";
            ++failed_frames_;
            return false;
        }
        if (inference.job.reconnect_count != session->reconnect_count) {
            session->tracker.reset();
            session->counter.resetTrackState();
            session->renderer.resetEventMarkers();
            if (session->security) session->security->reset();
            if (session->security_renderer) session->security_renderer->reset();
            session->security_frame = {};
            session->warmup_remaining =
                config_.people_flow.warmup_frames_after_reconnect;
            session->reconnect_count = inference.job.reconnect_count;
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

        bool warmup_active = false;
        if (contains(session->algorithms, "people_flow")) {
            if (session->warmup_remaining > 0) {
                warmup_active = true;
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
            session->security_frame = session->security->update(
                tracks,
                inference.output,
                frame_size,
                inference.job.capture_time_ms);
            for (const auto& event : session->security_frame.new_events) {
                if (wantsSecurityCategory(session->algorithms, event.category)) {
                    alerts.push_back(securityAlert(config_, inference, event));
                }
            }
        }

        ++session->frame_count;
        ++session->infer_window_frames;
        if (session->infer_window_start_ms <= 0) {
            session->infer_window_start_ms = inference.job.capture_time_ms;
        }
        const long long infer_elapsed =
            inference.job.capture_time_ms - session->infer_window_start_ms;
        if (infer_elapsed >= 1000) {
            session->infer_fps =
                static_cast<double>(session->infer_window_frames) * 1000.0 /
                std::max(1LL, infer_elapsed);
            session->infer_window_start_ms = inference.job.capture_time_ms;
            session->infer_window_frames = 0;
        }
        session->last_inference_ms = inference.inference_ms;

        const PeopleFlowCounts counts = session->counter.counts();
        const bool snapshot_due = session->frame_count == 1 ||
            session->frame_count - session->last_snapshot_frame >=
                session->snapshot_interval_frames ||
            !alerts.empty();
        if (snapshot_due) {
            PeopleFlowRenderMetrics metrics;
            metrics.capture_fps = inference.job.capture_fps;
            metrics.infer_fps = session->infer_fps;
            metrics.latest_frame_age_ms = inference.job.latest_frame_age_ms;
            metrics.reconnect_count = session->reconnect_count;
            metrics.capture_state = "running";
            cv::Mat rendered = session->renderer.render(
                inference.job.frame->image, tracks, counts, metrics, nullptr);
            if (session->security_renderer) {
                rendered = session->security_renderer->render(
                    rendered.empty() ? inference.job.frame->image : rendered,
                    session->security_frame);
            }
            const cv::Mat& output =
                rendered.empty() ? inference.job.frame->image : rendered;
            const std::vector<int> parameters{
                cv::IMWRITE_JPEG_QUALITY,
                std::clamp(config_.people_flow.jpeg_quality, 1, 100)
            };
            try {
                if (cv::imwrite(
                        session->snapshot_path.string(), output, parameters)) {
                    session->last_snapshot_frame = session->frame_count;
                    session->snapshot_degraded = false;
                }
                else {
                    session->snapshot_degraded = true;
                }
            }
            catch (...) {
                session->snapshot_degraded = true;
            }
        }

        const json security_state = session->security
            ? securityFrameJson(
                session->security_frame,
                config_.people_flow.security,
                inference.job.run_id,
                inference.job.task_id)
            : inactiveSecurityState(
                inference.job.capture_time_ms,
                inference.job.run_id,
                inference.job.task_id);

        analysis_result.run_id = inference.job.run_id;
        analysis_result.task_id = inference.job.task_id;
        analysis_result.initial_occupancy = session->initial_occupancy;
        analysis_result.in_count = counts.in_count;
        analysis_result.out_count = counts.out_count;
        analysis_result.final_occupancy = counts.occupancy;
        analysis_result.last_live_persons = counts.live_persons;
        analysis_result.security_state_json = security_state.dump();
        analysis_result.snapshot_relative_path =
            session->last_snapshot_frame > 0
                ? session->snapshot_relative_path
                : std::string{};
        analysis_result.storage_degraded = session->storage_degraded;
        analysis_result.snapshot_degraded = session->snapshot_degraded;
        analysis_result.last_update_ms = inference.job.capture_time_ms;

        hot.found = true;
        hot.run_id = inference.job.run_id;
        hot.task_id = inference.job.task_id;
        hot.analysis_config_version = session->config_version;
        hot.infer_fps = session->infer_fps;
        hot.last_inference_ms = session->last_inference_ms;
        hot.analysis_frame_count = session->frame_count;
        hot.initial_occupancy = session->initial_occupancy;
        hot.in_count = counts.in_count;
        hot.out_count = counts.out_count;
        hot.occupancy = counts.occupancy;
        hot.live_persons = counts.live_persons;
        hot.analysis_reconnect_count = session->reconnect_count;
        hot.warmup_frames_remaining = session->warmup_remaining;
        hot.security_state_json = analysis_result.security_state_json;
        hot.analysis_snapshot_relative_path =
            analysis_result.snapshot_relative_path;
        hot.analysis_storage_degraded = session->storage_degraded;
        hot.analysis_snapshot_degraded = session->snapshot_degraded;
        hot.analysis_last_update_ms = inference.job.capture_time_ms;

        persist_analysis = session->last_persist_ms <= 0 ||
            inference.job.capture_time_ms - session->last_persist_ms >= 1000 ||
            !alerts.empty();
        session->latest_result = analysis_result;
        session->latest_hot = hot;
        session->has_snapshot = true;
        (void)warmup_active;
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
    if (persist_analysis) {
        std::string repository_error;
        if (repository_->upsertRunAnalysisResult(
                analysis_result, repository_error)) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->last_persist_ms = analysis_result.last_update_ms;
            session->storage_degraded = false;
            session->latest_result.storage_degraded = false;
            session->latest_hot.analysis_storage_degraded = false;
            hot.analysis_storage_degraded = false;
        }
        else {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->storage_degraded = true;
            session->latest_result.storage_degraded = true;
            session->latest_hot.analysis_storage_degraded = true;
            hot.analysis_storage_degraded = true;
        }
    }
    if (status_sink_) {
        std::string status_error;
        status_sink_->updateAnalysisStatus(hot, status_error);
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
        CameraRunAnalysisResultRecord final_result;
        CameraTaskRunHotStatus final_hot;
        bool finalize = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            const auto found = sessions_.find(task_id);
            if (found == sessions_.end() || found->second->run_id != run_id) return;
            session = found->second;
            sessions_.erase(found);
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->active = false;
            if (session->has_snapshot) {
                final_result = session->latest_result;
                final_result.finalized_at_ms = wallNowMs();
                final_result.last_update_ms =
                    std::max(final_result.last_update_ms,
                        final_result.finalized_at_ms);
                final_hot = session->latest_hot;
                final_hot.analysis_last_update_ms =
                    final_result.last_update_ms;
                finalize = true;
            }
        }
        if (finalize) {
            std::string ignored;
            repository_->upsertRunAnalysisResult(final_result, ignored);
            if (status_sink_) {
                status_sink_->updateAnalysisStatus(final_hot, ignored);
            }
        }
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
