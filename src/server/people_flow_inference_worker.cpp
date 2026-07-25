#include "server/people_flow_inference_worker.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "business/line_crossing_counter.h"
#include "business/person_detector_adapter.h"
#include "business/person_tracker.h"
#include "business/people_flow_renderer.h"
#include "business/security_live_pipeline.h"
#include "business/security_overlay_renderer.h"
#include "server/camera_profile.h"
#include "server/people_flow_session_runner.h"
#include "server/rtsp_camera_frame_source.h"
#include "server/shared_camera_frame_hub.h"

namespace yolo11_server {

    namespace {

        RedisSection workerRedisConfig(const RedisSection& base, const std::string& consumer_name) {
            RedisSection result = base;
            result.consumer_name = consumer_name;
            return result;
        }

        std::string processIdString() {
#ifdef _WIN32
            return std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
#else
            return std::to_string(static_cast<long long>(::getpid()));
#endif
        }

        std::string hostNameString() {
            char buffer[256] = { 0 };
#ifdef _WIN32
            DWORD size = static_cast<DWORD>(sizeof(buffer));
            if (::GetComputerNameA(buffer, &size)) return std::string(buffer, size);
#else
            if (::gethostname(buffer, sizeof(buffer) - 1) == 0) return std::string(buffer);
#endif
            return "unknown";
        }

        nlohmann::json securityEventJson(const SecurityEvent& event) {
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

        nlohmann::json activeActionsJson(
            const std::map<std::int64_t, std::vector<std::string>>& actions
        ) {
            nlohmann::json items = nlohmann::json::array();
            for (const auto& [track_id, labels] : actions) {
                items.push_back({ { "track_id", track_id }, { "labels", labels } });
            }
            return items;
        }

        nlohmann::json securityFrameJson(
            const SecurityFrameResult& frame,
            const PeopleFlowSecuritySection& config,
            const std::string& session_id,
            const std::string& camera_id
        ) {
            int inside_count = 0;
            nlohmann::json zone_statuses = nlohmann::json::array();
            for (const TrackZoneStatus& status : frame.zone_statuses) {
                if (status.inside) ++inside_count;
                zone_statuses.push_back({
                    { "zone_id", status.zone_id }, { "track_id", status.track_id },
                    { "inside", status.inside }, { "entered_at_ms", status.entered_at_ms },
                    { "dwell_ms", status.dwell_ms },
                    { "dwell_alarm_emitted", status.dwell_alarm_emitted }
                });
            }
            nlohmann::json tracks = nlohmann::json::array();
            for (const TrackAnalyticsSnapshot& analytics : frame.track_analytics) {
                tracks.push_back({
                    { "track_id", analytics.track_id },
                    { "speed_px_s", analytics.instantaneous_speed_px_s },
                    { "average_speed_px_s", analytics.average_speed_px_s },
                    { "distance_px", analytics.cumulative_distance_px },
                    { "stationary_ms", analytics.stationary_ms },
                    { "stationary", analytics.stationary }, { "loitering", analytics.loitering }
                });
            }
            nlohmann::json events = nlohmann::json::array();
            for (auto it = frame.recent_events.rbegin(); it != frame.recent_events.rend(); ++it) {
                events.push_back(securityEventJson(*it));
            }
            return {
                { "success", true }, { "mode", "single_machine_four_stage_demo" },
                { "production_action_model", false }, { "session_id", session_id },
                { "camera_id", camera_id }, { "timestamp_ms", frame.timestamp_ms },
                { "stages", {
                    { "phase1", { { "name", "electronic_fence" }, { "ready", true },
                        { "zone_count", config.zones.size() }, { "inside_count", inside_count },
                        { "statuses", zone_statuses } } },
                    { "phase2", { { "name", "tracking_and_analytics" }, { "ready", true },
                        { "alpha_beta_filter", true }, { "track_count", tracks.size() },
                        { "tracks", tracks } } },
                    { "phase3", { { "name", "pose_rule_actions" }, { "ready", true },
                        { "model", "yolo11-pose-tensorrt" }, { "pose_count", frame.poses.size() },
                        { "active_actions", activeActionsJson(frame.active_pose_actions) } } },
                    { "phase4", { { "name", "temporal_action_demo" }, { "ready", true },
                        { "classifier", "feature-threshold-demo" }, { "demo_classifier", true },
                        { "label", config.temporal_demo_label },
                        { "active_actions", activeActionsJson(frame.active_temporal_actions) } } }
                } },
                { "events", events }
            };
        }

        bool writeJsonAtomically(const std::filesystem::path& path, const nlohmann::json& value) {
            const std::filesystem::path temporary = path.string() + ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) return false;
                output << value.dump(2) << '\n';
                if (!output.good()) return false;
            }
#ifdef _WIN32
            return ::MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            std::error_code error;
            std::filesystem::rename(temporary, path, error);
            return !error;
#endif
        }

    }  // namespace

    PeopleFlowInferenceWorker::PeopleFlowInferenceWorker(
        int worker_id,
        const AppConfig& config,
        const std::string& consumer_name,
        std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry
    ) : worker_id_(worker_id),
        config_(config),
        redis_queue_(workerRedisConfig(config.redis, consumer_name)),
        heartbeat_queue_(workerRedisConfig(config.redis, consumer_name)),
        hub_registry_(std::move(hub_registry)) {
        config_.redis.consumer_name = consumer_name;
        process_start_time_ms_ = nowMs();
    }

    PeopleFlowInferenceWorker::~PeopleFlowInferenceWorker() noexcept {
        stop();
        releaseRunnerNoexcept();
    }

    bool PeopleFlowInferenceWorker::start() {
        if (running_.load()) return true;
        if (!config_.redis.enabled || !config_.people_flow.enabled) {
            spdlog::error("PeopleFlowInferenceWorker requires redis.enabled=true and people_flow.enabled=true");
            return false;
        }
        const bool supported_security_pose = config_.people_flow.security.enabled &&
            config_.model.type == "pose";
        if (config_.model.type != "detect" && !supported_security_pose) {
            spdlog::error("People-flow worker requires model.type=detect, or pose when people_flow.security.enabled=true");
            return false;
        }
        if (!config_.people_flow.config_error.empty()) {
            spdlog::error("People-flow config invalid: {}", config_.people_flow.config_error);
            return false;
        }
        if (!config_.camera_hub.enabled) {
            spdlog::error("People-flow worker requires camera_hub.enabled=true");
            return false;
        }
        if (!hub_registry_) {
            hub_registry_ = createSharedCameraFrameHubRegistry(config_);
            owns_hub_registry_ = true;
        }
        if (!hub_registry_) {
            spdlog::error("People-flow camera Hub registry initialization failed");
            return false;
        }
        std::string error;
        std::cerr << "[BOOT] connecting People Flow Redis clients\n";
        if (!redis_queue_.connect(error) || !heartbeat_queue_.connect(error)) {
            spdlog::error("People-flow Redis connection failed: {}", error);
            return false;
        }
        std::cerr << "[BOOT] People Flow Redis clients connected\n";
        std::cerr << "[BOOT] initializing People Flow model\n";
        if (!initModelRunner()) return false;
        runner_initialized_ = true;
        std::cerr << "[BOOT] People Flow model initialized\n";
        std::filesystem::create_directories(config_.people_flow.output_dir);
        repository_ = std::make_unique<PeopleFlowRepository>(config_.people_flow);
        std::string storage_error;
        std::cerr << "[BOOT] starting People Flow repository\n";
        if (!repository_->start(true, storage_error)) {
            spdlog::warn("People-flow PostgreSQL starts degraded and will retry: {}", storage_error);
        }
        std::cerr << "[BOOT] People Flow repository started\n";
        running_.store(true);
        setWorkerState("idle", "", "");
        if (config_.worker.heartbeat_enabled) heartbeat_thread_ = std::thread([this]() { heartbeatLoop(); });
        thread_ = std::thread([this]() { loop(); });
        std::cerr << "[BOOT] People Flow command and heartbeat threads started\n";
        return true;
    }

    void PeopleFlowInferenceWorker::stop() noexcept {
        running_.store(false);
        try {
            if (thread_.joinable()) thread_.join();
            if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
            if (repository_) repository_->stop();
            if (owns_hub_registry_ && hub_registry_) {
                hub_registry_->stopAll();
                hub_registry_.reset();
                owns_hub_registry_ = false;
            }
        }
        catch (...) {
            spdlog::error("People-flow worker stop exception ignored");
        }
    }

    bool PeopleFlowInferenceWorker::running() const {
        return running_.load();
    }

    void PeopleFlowInferenceWorker::setAlgorithmRuntimeProvider(
        AlgorithmRuntimeSnapshotProvider provider
    ) {
        {
            std::lock_guard<std::mutex> lock(algorithm_provider_mutex_);
            algorithm_provider_ = std::move(provider);
        }
        if (running_.load()) writeHeartbeatNoexcept();
    }

    bool PeopleFlowInferenceWorker::initModelRunner() {
        runner_ = createModelRunner(config_.model.type);
        std::string error;
        if (!runner_ || !runner_->init(config_, error)) {
            spdlog::error("People-flow {} ModelRunner init failed: {}", config_.model.type, error);
            runner_.reset();
            return false;
        }
        return true;
    }

    void PeopleFlowInferenceWorker::releaseRunnerNoexcept() noexcept {
        if (!runner_initialized_) return;
        try {
            if (runner_) runner_->release();
        }
        catch (...) {
        }
        runner_.reset();
        runner_initialized_ = false;
    }

    void PeopleFlowInferenceWorker::loop() {
        writeHeartbeatNoexcept();
        while (running_.load()) {
            if (session_active_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (session_thread_.joinable()) session_thread_.join();
            RedisTask task;
            std::string error;
            if (!redis_queue_.popTask(task, error)) {
                if (!error.empty()) {
                    spdlog::error("People-flow popTask failed: {}", error);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                continue;
            }
            session_active_.store(true);
            try {
                session_thread_ = std::thread([this, task]() {
                    try {
                        PeopleFlowSessionRunner session(
                            worker_id_, config_, redis_queue_, repository_.get(), runner_.get(),
                            hub_registry_, running_, processed_count_, failed_count_,
                            [this](const std::string& status, const std::string& session_id,
                                   const std::string& state_error) {
                                setWorkerState(status, session_id, state_error);
                            },
                            [this]() { writeHeartbeatNoexcept(); });
                        session.run(task);
                    }
                    catch (const std::exception& e) {
                        failed_count_.fetch_add(1);
                        setWorkerState("idle", "", e.what());
                        spdlog::error("Unhandled People Flow Runner error: {}", e.what());
                    }
                    catch (...) {
                        failed_count_.fetch_add(1);
                        setWorkerState("idle", "", "unknown People Flow Runner error");
                        spdlog::error("Unhandled unknown People Flow Runner error");
                    }
                    session_active_.store(false);
                });
            }
            catch (const std::exception& e) {
                session_active_.store(false);
                failed_count_.fetch_add(1);
                setWorkerState("idle", "", e.what());
                spdlog::error("Failed to create People Flow session thread: {}", e.what());
            }
        }
        if (session_thread_.joinable()) session_thread_.join();
        setWorkerState("stopping", "", "");
        writeHeartbeatNoexcept();
    }

    void PeopleFlowSessionRunner::run(const RedisTask& task) {
        const std::string session_id = task.people_flow_session_id.empty() ? task.task_id : task.people_flow_session_id;
        const std::string camera_id = task.people_flow_camera_id.empty() ? config_.people_flow.camera_id : task.people_flow_camera_id;
        const long long start_time_ms = nowMs();
        bool acknowledged = false;
        std::shared_ptr<FrameSubscription> frame_subscription;
        PeopleFlowSessionStatus state;
        state.found = true;
        state.session_id = session_id;
        state.camera_id = camera_id;
        state.camera_profile = task.camera_profile;
        state.masked_uri = task.masked_uri;
        state.config_version = task.people_flow_config_version.empty()
            ? config_.people_flow.config_version
            : task.people_flow_config_version;
        state.snapshot_path = task.snapshot_path;
        state.status = "starting";
        state.capture_state = "starting";
        state.worker_id = worker_id_;
        state.consumer_name = config_.redis.consumer_name;
        state.create_time_ms = task.create_time_ms;
        state.start_time_ms = start_time_ms;
        state.last_update_ms = start_time_ms;
        state.occupancy = std::max(0LL, task.initial_occupancy);
        bool redis_event_degraded = false;
        if (repository_) {
            PeopleFlowSessionRecord session;
            session.session_id = session_id;
            session.camera_id = camera_id;
            session.status = "starting";
            session.start_time_ms = start_time_ms;
            session.initial_occupancy = state.occupancy;
            session.final_occupancy = state.occupancy;
            session.config_version = state.config_version;
            if (!repository_->enqueueSessionStart(session)) redis_event_degraded = true;
        }
        auto publish = [&]() {
            state.last_update_ms = nowMs();
            if (repository_) {
                const PeopleFlowRepositoryHealth storage_health = repository_->health();
                state.event_queue_depth = static_cast<long long>(storage_health.queue_depth);
                state.storage_degraded = redis_event_degraded || storage_health.degraded ||
                    storage_health.dropped_tasks > 0;
                if (storage_health.degraded && state.last_error.empty()) {
                    state.last_error = "PostgreSQL persistence degraded";
                }
            }
            else {
                state.storage_degraded = true;
            }
            std::string publish_error;
            if (!redis_queue_.updatePeopleFlowSession(
                    state,
                    config_.people_flow.session_ttl_seconds,
                    config_.people_flow.realtime_ttl_seconds,
                    publish_error)) {
                spdlog::warn("People-flow status update failed: session_id={}, error={}", session_id, publish_error);
            }
        };

        setWorkerState("starting", session_id, "");
        publish();
        writeHeartbeatNoexcept();

        try {
            if (task.task_kind != "people_flow") throw std::runtime_error("unexpected task_kind for people-flow worker");
            if (!runner_) throw std::runtime_error("people-flow model runner is not initialized");
            if (session_id.empty() || camera_id.empty()) throw std::runtime_error("people-flow task has empty session or camera id");

            const auto profile_it = config_.camera_profiles.find(task.camera_profile);
            if (profile_it == config_.camera_profiles.end()) throw std::runtime_error("camera profile is unknown");
            const CameraProfile& profile = profile_it->second;
            if (task.source_ref != "env:" + profile.url_env) throw std::runtime_error("camera profile secret reference mismatch");
            if (!hub_registry_) throw std::runtime_error("camera Hub registry is unavailable");
            std::string subscription_error;
            const SubscriberDescriptor subscriber{
                "people_flow:" + config_.redis.consumer_name + ":" + session_id,
                "people_flow"
            };
            if (!hub_registry_->subscribe(
                    task.camera_profile, subscriber, frame_subscription, subscription_error)) {
                throw std::runtime_error(subscription_error.empty()
                    ? "camera Hub subscription failed"
                    : subscription_error);
            }
            std::string ack_error;
            acknowledged = redis_queue_.ackTask(task.stream_id, ack_error);
            if (!acknowledged) spdlog::warn("People-flow XACK failed: session_id={}, error={}", session_id, ack_error);

            const std::filesystem::path snapshot_path = state.snapshot_path.empty()
                ? std::filesystem::path(config_.people_flow.output_dir) / camera_id / session_id / "latest.jpg"
                : std::filesystem::path(state.snapshot_path);
            state.snapshot_path = snapshot_path.string();
            std::filesystem::create_directories(snapshot_path.parent_path());
            const std::filesystem::path security_state_path =
                snapshot_path.parent_path() / config_.people_flow.security.state_file_name;
            if (config_.people_flow.security.enabled) {
                std::error_code remove_error;
                std::filesystem::remove(security_state_path, remove_error);
            }

            PersonDetectorAdapter adapter(config_.people_flow);
            PersonTracker tracker(config_.people_flow.tracker);
            LineCrossingCounter counter(
                config_.people_flow.counting,
                session_id,
                camera_id,
                state.config_version,
                task.initial_occupancy
            );
            PeopleFlowRenderer renderer(config_.people_flow);
            std::unique_ptr<SecurityLivePipeline> security_pipeline;
            std::unique_ptr<SecurityOverlayRenderer> security_renderer;
            if (config_.people_flow.security.enabled) {
                security_pipeline = std::make_unique<SecurityLivePipeline>(
                    config_.people_flow.security, session_id, camera_id);
                security_renderer = std::make_unique<SecurityOverlayRenderer>(
                    config_.people_flow.security);
            }
            const bool debug_enabled = config_.people_flow.visualization.enabled;
            const std::filesystem::path event_frames_dir = snapshot_path.parent_path() / "event_frames";
            if (debug_enabled && config_.people_flow.visualization.save_event_frames) {
                std::filesystem::create_directories(event_frames_dir);
            }

            int last_reconnect_count = 0;
            int warmup_remaining = config_.people_flow.warmup_frames_after_reconnect;
            long long last_status_update_ms = 0;
            long long last_stop_check_ms = 0;
            long long last_calibration_check_ms = 0;
            long long infer_window_start_ms = nowMs();
            long long infer_window_frames = 0;
            long long last_snapshot_frame = 0;
            long long event_evidence_sequence = 0;
            const int snapshot_interval = std::max(1,
                config_.people_flow.target_infer_fps / std::max(1, config_.people_flow.snapshot_fps));
            auto next_infer_time = std::chrono::steady_clock::now();
            std::vector<PersonTrack> tracks;
            std::vector<PersonTrack> all_tracks;
            PersonFilterResult filter_result;
            std::vector<TrackCounterDebugState> counter_debug_states;
            PeopleFlowFrameDebug frame_debug;
            SecurityFrameResult security_frame;

            while (running_.load()) {
                const long long loop_ms = nowMs();
                if (loop_ms - last_stop_check_ms >= 100) {
                    bool stop_requested = false;
                    std::string stop_error;
                    if (redis_queue_.isPeopleFlowStopRequested(session_id, stop_requested, stop_error)) {
                        state.stop_requested = stop_requested;
                    }
                    if (state.stop_requested) break;
                    last_stop_check_ms = loop_ms;
                }

                const CameraHubStatus hub_status = frame_subscription->hubStatus();
                const SubscriptionMetrics subscription_metrics = frame_subscription->metrics();
                state.capture_state = hub_status.state;
                state.capture_backend = hub_status.backend_name;
                state.shared_hub = true;
                state.hub_instance_id = hub_status.hub_instance_id;
                state.hub_subscribers = hub_status.subscriber_count;
                state.capture_fps = hub_status.capture_fps;
                state.source_fps = hub_status.source_fps;
                state.latest_frame_age_ms = hub_status.latest_frame_age_ms;
                state.last_frame_time_ms = hub_status.last_frame_time_ms;
                state.reconnect_count = hub_status.reconnect_count;
                state.dropped_frames = subscription_metrics.skipped_frames;
                state.width = hub_status.width;
                state.height = hub_status.height;
                state.resolution_changed = hub_status.resolution_changed;
                state.last_error = hub_status.last_error;
                if (hub_status.state == "failed" || hub_status.state == "stopped") {
                    throw std::runtime_error(hub_status.last_error.empty()
                        ? "RTSP capture failed"
                        : hub_status.last_error);
                }
                if (hub_status.reconnect_count != last_reconnect_count) {
                    tracker.reset();
                    counter.resetTrackState();
                    renderer.resetEventMarkers();
                    if (security_pipeline) security_pipeline->reset();
                    if (security_renderer) security_renderer->reset();
                    tracks.clear();
                    all_tracks.clear();
                    filter_result = {};
                    counter_debug_states.clear();
                    frame_debug = {};
                    warmup_remaining = config_.people_flow.warmup_frames_after_reconnect;
                    last_reconnect_count = hub_status.reconnect_count;
                }
                state.status = hub_status.state == "running" ? "running" : "reconnecting";
                setWorkerState(state.status, session_id, state.last_error);

                const auto steady_now = std::chrono::steady_clock::now();
                if (hub_status.state == "running" && steady_now >= next_infer_time) {
                    FrameReadResult read_result;
                    if (frame_subscription->tryReadLatest(read_result) && read_result.frame) {
                        const FrameEnvelope& captured = *read_result.frame;
                        const auto infer_start = std::chrono::steady_clock::now();
                        const ModelOutput output = runner_->infer(captured.image);
                        if (debug_enabled) {
                            filter_result = adapter.filterWithDebug(
                                output, captured.image.size(), captured.capture_time_ms);
                            const auto& updated_tracks = tracker.update(
                                filter_result.accepted,
                                captured.image.cols,
                                captured.image.rows,
                                captured.capture_time_ms);
                            all_tracks.assign(updated_tracks.begin(), updated_tracks.end());
                        }
                        else {
                            const auto detections = adapter.filter(
                                output, captured.image.size(), captured.capture_time_ms);
                            tracker.update(
                                detections,
                                captured.image.cols,
                                captured.image.rows,
                                captured.capture_time_ms);
                        }
                        tracks = tracker.confirmedTracks();
                        if (security_pipeline) {
                            security_frame = security_pipeline->update(
                                tracks, output, captured.image.size(), captured.capture_time_ms);
                        }
                        std::vector<CrossingEvent> events;
                        const bool warmup_active = warmup_remaining > 0;
                        const int warmup_display = warmup_remaining;
                        if (warmup_active) {
                            --warmup_remaining;
                        }
                        else {
                            events = counter.update(tracks, captured.image.cols, captured.image.rows, captured.capture_time_ms);
                        }
                        if (debug_enabled) {
                            counter_debug_states = counter.debugStates(
                                all_tracks, captured.image.cols, captured.image.rows);
                        }

                        std::vector<std::filesystem::path> event_evidence_paths;
                        if (debug_enabled && config_.people_flow.visualization.save_event_frames) {
                            event_evidence_paths.reserve(events.size());
                            for (CrossingEvent& event : events) {
                                std::ostringstream filename;
                                filename << "event_" << std::setw(6) << std::setfill('0')
                                         << ++event_evidence_sequence << '_' << event.direction
                                         << "_track_" << event.track_id << ".jpg";
                                const std::filesystem::path evidence_path = event_frames_dir / filename.str();
                                event.evidence_path = evidence_path.string();
                                event_evidence_paths.push_back(evidence_path);
                            }
                        }

                        const auto infer_end = std::chrono::steady_clock::now();
                        state.last_inference_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
                        ++state.frame_count;
                        ++infer_window_frames;
                        const long long infer_elapsed = nowMs() - infer_window_start_ms;
                        if (infer_elapsed >= 1000) {
                            state.infer_fps = static_cast<double>(infer_window_frames) * 1000.0 / std::max(1LL, infer_elapsed);
                            infer_window_start_ms = nowMs();
                            infer_window_frames = 0;
                        }
                        const PeopleFlowCounts counts = counter.counts();
                        state.in_count = counts.in_count;
                        state.out_count = counts.out_count;
                        state.occupancy = counts.occupancy;
                        state.live_persons = counts.live_persons;

                        const bool snapshot_due = state.frame_count == 1 ||
                            state.frame_count - last_snapshot_frame >= snapshot_interval;
                        const bool event_render_due = debug_enabled && !events.empty() &&
                            (config_.people_flow.visualization.draw_event_markers ||
                             config_.people_flow.visualization.save_event_frames);
                        const bool security_event_due = security_pipeline && !security_frame.new_events.empty();

                        const PeopleFlowFrameDebug* debug_ptr = nullptr;
                        if (debug_enabled) {
                            frame_debug.frame_index = state.frame_count;
                            frame_debug.timestamp_ms = captured.capture_time_ms;
                            frame_debug.inference_frame = true;
                            frame_debug.warmup_active = warmup_active;
                            frame_debug.warmup_frames_remaining = warmup_display;
                            frame_debug.filter_statistics = filter_result.statistics;
                            frame_debug.detection_items = filter_result.debug_items;
                            frame_debug.all_tracks = all_tracks;
                            frame_debug.counter_states = counter_debug_states;
                            frame_debug.new_events = events;
                            debug_ptr = &frame_debug;
                        }

                        if (snapshot_due || event_render_due || security_event_due) {
                            PeopleFlowRenderMetrics render_metrics;
                            render_metrics.capture_fps = state.capture_fps;
                            render_metrics.infer_fps = state.infer_fps;
                            render_metrics.latest_frame_age_ms = state.latest_frame_age_ms;
                            render_metrics.reconnect_count = state.reconnect_count;
                            render_metrics.capture_state = state.capture_state;
                            cv::Mat rendered = renderer.render(
                                captured.image, tracks, counts, render_metrics, debug_ptr);
                            if (security_renderer) {
                                rendered = security_renderer->render(
                                    rendered.empty() ? captured.image : rendered, security_frame);
                            }
                            const cv::Mat& output_frame = rendered.empty() ? captured.image : rendered;
                            const std::vector<int> params{ cv::IMWRITE_JPEG_QUALITY, config_.people_flow.jpeg_quality };
                            if (snapshot_due) {
                                if (!cv::imwrite(snapshot_path.string(), output_frame, params)) {
                                    state.snapshot_degraded = true;
                                    state.last_error = "latest snapshot write failed";
                                }
                                else {
                                    state.snapshot_degraded = false;
                                    last_snapshot_frame = state.frame_count;
                                }
                                if (security_pipeline && !writeJsonAtomically(
                                        security_state_path,
                                        securityFrameJson(
                                            security_frame, config_.people_flow.security,
                                            session_id, camera_id))) {
                                    state.last_error = "security state write failed";
                                }
                            }
                            for (std::size_t i = 0; i < event_evidence_paths.size(); ++i) {
                                if (!cv::imwrite(event_evidence_paths[i].string(), output_frame, params)) {
                                    events[i].evidence_path.clear();
                                    state.snapshot_degraded = true;
                                    state.last_error = "event evidence snapshot write failed";
                                }
                            }
                        }

                        for (const CrossingEvent& event : events) {
                            std::string event_error;
                            if (!redis_queue_.appendPeopleFlowEvent(event, config_.people_flow.storage.events_max_len, event_error)) {
                                redis_event_degraded = true;
                                state.last_error = "Redis crossing event append failed";
                                spdlog::warn("People-flow Redis event append failed: session_id={}, event_id={}", session_id, event.event_id);
                            }
                            if (!repository_ || !repository_->enqueueEvent(event)) {
                                redis_event_degraded = true;
                                state.last_error = "PostgreSQL crossing event enqueue failed";
                            }
                        }
                        next_infer_time = steady_now + std::chrono::milliseconds(
                            std::max(1, 1000 / config_.people_flow.target_infer_fps));
                    }
                }

                if (loop_ms - last_calibration_check_ms >= 500) {
                    long long requested_version = 0;
                    long long requested_occupancy = 0;
                    std::string calibration_error;
                    if (redis_queue_.getPeopleFlowCalibrationRequest(
                            camera_id, requested_version, requested_occupancy, calibration_error) &&
                        requested_version > state.applied_calibration_version) {
                        counter.calibrateOccupancy(requested_occupancy);
                        state.occupancy = std::max(0LL, requested_occupancy);
                        state.applied_calibration_version = requested_version;
                        spdlog::info("People-flow occupancy calibration applied: session_id={}, version={}",
                            session_id, requested_version);
                    }
                    last_calibration_check_ms = loop_ms;
                }

                if (loop_ms - last_status_update_ms >= 1000) {
                    publish();
                    std::string lease_error;
                    if (!redis_queue_.refreshPeopleFlowLease(
                            camera_id, session_id, config_.people_flow.active_ttl_seconds, lease_error)) {
                        throw std::runtime_error("people-flow active lease lost");
                    }
                    writeHeartbeatNoexcept();
                    last_status_update_ms = loop_ms;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            frame_subscription.reset();
            state.status = "stopped";
            state.capture_state = "stopped";
            state.stop_time_ms = nowMs();
            state.last_error.clear();
            publish();
            if (repository_) {
                PeopleFlowSessionRecord session;
                session.session_id = session_id;
                session.camera_id = camera_id;
                session.status = state.status;
                session.start_time_ms = start_time_ms;
                session.stop_time_ms = state.stop_time_ms;
                session.initial_occupancy = task.initial_occupancy;
                session.in_count = state.in_count;
                session.out_count = state.out_count;
                session.final_occupancy = state.occupancy;
                session.config_version = state.config_version;
                session.stop_reason = state.stop_requested ? "requested" : "worker_shutdown";
                if (!repository_->enqueueSessionFinish(session)) {
                    spdlog::warn("People-flow final session record enqueue failed: session_id={}", session_id);
                }
            }
            std::string release_error;
            redis_queue_.releasePeopleFlowLease(camera_id, session_id, release_error);
            processed_count_.fetch_add(1);
            setWorkerState("idle", "", "");
            writeHeartbeatNoexcept();
        }
        catch (const std::exception& e) {
            frame_subscription.reset();
            state.status = "failed";
            state.capture_state = "failed";
            state.stop_time_ms = nowMs();
            state.error = e.what();
            state.last_error = e.what();
            publish();
            if (repository_) {
                PeopleFlowSessionRecord session;
                session.session_id = session_id;
                session.camera_id = camera_id;
                session.status = state.status;
                session.start_time_ms = start_time_ms;
                session.stop_time_ms = state.stop_time_ms;
                session.initial_occupancy = task.initial_occupancy;
                session.in_count = state.in_count;
                session.out_count = state.out_count;
                session.final_occupancy = state.occupancy;
                session.config_version = state.config_version;
                session.stop_reason = "failed";
                session.error = state.error;
                if (!repository_->enqueueSessionFinish(session)) {
                    spdlog::warn("People-flow failed session record enqueue failed: session_id={}", session_id);
                }
            }
            std::string release_error;
            redis_queue_.releasePeopleFlowLease(camera_id, session_id, release_error);
            if (!acknowledged && !task.stream_id.empty()) {
                std::string ack_error;
                redis_queue_.ackTask(task.stream_id, ack_error);
            }
            failed_count_.fetch_add(1);
            setWorkerState("idle", "", e.what());
            writeHeartbeatNoexcept();
            spdlog::error("People-flow session failed: session_id={}, camera_id={}, error={}",
                session_id, camera_id, e.what());
        }
    }

    void PeopleFlowInferenceWorker::heartbeatLoop() noexcept {
        while (running_.load()) {
            writeHeartbeatNoexcept();
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.worker.heartbeat_interval_ms));
        }
        writeHeartbeatNoexcept();
    }

    void PeopleFlowInferenceWorker::writeHeartbeatNoexcept() noexcept {
        if (!config_.worker.heartbeat_enabled) return;
        try {
            WorkerHeartbeatRecord heartbeat;
            heartbeat.consumer_name = config_.redis.consumer_name;
            heartbeat.pid = processIdString();
            heartbeat.host = hostNameString();
            heartbeat.worker_id = worker_id_;
            heartbeat.gpu_id = config_.model.gpu_id;
            heartbeat.model_type = "vision_host";
            heartbeat.runner_model_type = runner_ ? runner_->modelType() : config_.model.type;
            heartbeat.worker_group = config_.worker.worker_group;
            heartbeat.worker_kind = "vision_host";
            heartbeat.task_kind = config_.camera_tasks.enabled
                ? "live_people_flow,camera_frame"
                : "live_people_flow";
            heartbeat.stream_type = "long_running_stream";
            heartbeat.runtime_mode = "legacy_split";
            heartbeat.worker_generation =
                heartbeat.pid + ":" + std::to_string(process_start_time_ms_);
            heartbeat.legacy_people_flow_role = true;
            heartbeat.camera_task_manager_running =
                config_.camera_tasks.enabled &&
                heartbeat.algorithm_runtime.host_running;
            heartbeat.hub_registry_ready = hub_registry_ != nullptr;
            heartbeat.coordination_healthy = true;
            heartbeat.engine_path = config_.model.engine_path;
            heartbeat.labels_path = config_.model.labels_path;
            heartbeat.max_concurrency = 1 +
                (config_.camera_tasks.enabled ? config_.camera_tasks.max_active_runs : 0);
            heartbeat.processed_count = processed_count_.load();
            heartbeat.failed_count = failed_count_.load();
            heartbeat.start_time_ms = process_start_time_ms_;
            heartbeat.last_heartbeat_ms = nowMs();
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                heartbeat.status = worker_status_;
                heartbeat.current_task_id = current_session_id_;
                heartbeat.last_error = last_error_;
            }
            AlgorithmRuntimeSnapshotProvider provider;
            {
                std::lock_guard<std::mutex> lock(algorithm_provider_mutex_);
                provider = algorithm_provider_;
            }
            if (provider) heartbeat.algorithm_runtime = provider();
            heartbeat.camera_task_manager_running =
                config_.camera_tasks.enabled &&
                heartbeat.algorithm_runtime.host_running;
            std::string error;
            if (!heartbeat_queue_.writeWorkerHeartbeat(heartbeat, config_.worker.heartbeat_ttl_seconds, error)) {
                spdlog::warn("People-flow heartbeat failed: {}", error);
            }
        }
        catch (...) {
        }
    }

    void PeopleFlowInferenceWorker::setWorkerState(
        const std::string& status,
        const std::string& session_id,
        const std::string& error
    ) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        worker_status_ = status;
        current_session_id_ = session_id;
        last_error_ = error;
    }

    long long PeopleFlowInferenceWorker::nowMs() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

}  // namespace yolo11_server
