#include "server/app_config.h"

#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cmath>

#include <yaml-cpp/yaml.h>

#include "server/yaml_file_loader.h"

namespace yolo11_server {

    namespace {

        template <typename T>
        T readOrDefault(const YAML::Node& node, const std::string& key, const T& default_value) {
            if (!node || !node[key]) {
                return default_value;
            }
            return node[key].as<T>();
        }

        std::string toLowerString(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        std::string inferWorkerKind(const AppConfig& config) {
            if (config.people_flow.enabled && config.camera_tasks.enabled) {
                return "vision_host";
            }
            if (config.people_flow.enabled) {
                return "people_flow";
            }
            if (config.stream.enabled) {
                return "stream";
            }
            if (config.video.enabled) {
                return "video";
            }
            return "image";
        }

        std::string inferTaskKind(const AppConfig& config) {
            if (config.people_flow.enabled && config.camera_tasks.enabled) {
                return "people_flow,camera_frame";
            }
            if (config.people_flow.enabled) {
                return "live_people_flow";
            }
            if (config.stream.enabled) {
                return "live_stream";
            }
            if (config.video.enabled) {
                return "video_file";
            }
            return "image_async";
        }

        std::string inferStreamType(const AppConfig& config) {
            return (config.stream.enabled || config.people_flow.enabled || config.camera_tasks.enabled)
                ? std::string("long_running_stream")
                : std::string("redis_stream");
        }

        std::string inferProfileType(const AppConfig& config) {
            if (config.people_flow.enabled) {
                return "people_flow";
            }
            if (config.stream.enabled) {
                return "stream";
            }
            if (config.video.enabled) {
                return "video";
            }
            return toLowerString(config.model.type.empty() ? std::string("detect") : config.model.type);
        }

        std::string inferWorkerGroup(const AppConfig& config) {
            const std::string kind = inferWorkerKind(config);
            const std::string profile = inferProfileType(config);
            return kind + "_" + profile + "_gpu" + std::to_string(config.model.gpu_id);
        }

        void normalizeWorkerCapability(AppConfig& config) {
            if (config.worker.worker_kind.empty()) {
                config.worker.worker_kind = inferWorkerKind(config);
            }
            if (config.worker.task_kind.empty()) {
                config.worker.task_kind = inferTaskKind(config);
            }
            if (config.worker.stream_type.empty()) {
                config.worker.stream_type = inferStreamType(config);
            }
            if (config.worker.worker_group.empty()) {
                config.worker.worker_group = inferWorkerGroup(config);
            }
            if (config.worker.max_concurrency <= 0) {
                config.worker.max_concurrency = (config.stream.enabled || config.people_flow.enabled)
                    ? 1
                    : config.worker.worker_num;
            }
        }

        bool readNormalizedPoint(const YAML::Node& node, NormalizedPoint& point) {
            if (!node || !node.IsSequence() || node.size() != 2) {
                return false;
            }
            try {
                const double x = node[0].as<double>();
                const double y = node[1].as<double>();
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                    return false;
                }
                point.x = x;
                point.y = y;
                return true;
            }
            catch (...) {
                return false;
            }
        }

    }  // namespace

    AppConfig AppConfig::loadFromYaml(const std::string& yaml_path) {
        AppConfig config;

        YAML::Node root;
        try {
            root = loadYamlFileAbiSafe(yaml_path);
        }
        catch (const std::exception& e) {
            throw std::runtime_error(
                "CONFIG_FILE_LOAD_FAILED: " + yaml_path + ": " + e.what());
        }

        auto server = root["server"];
        config.server.host = readOrDefault<std::string>(server, "host", config.server.host);
        config.server.port = readOrDefault<int>(server, "port", config.server.port);
        config.server.threads = readOrDefault<int>(server, "threads", config.server.threads);
        config.server.enable_sync_detect = readOrDefault<bool>(server, "enable_sync_detect", config.server.enable_sync_detect);
        config.server.max_body_size_mb = readOrDefault<int>(server, "max_body_size_mb", config.server.max_body_size_mb);
        if (config.server.threads <= 0) {
            config.server.threads = 1;
        }
        if (config.server.port <= 0 || config.server.port > 65535) {
            config.server.port = 8080;
        }
        if (config.server.max_body_size_mb < 0) {
            config.server.max_body_size_mb = 0;
        }

        auto model = root["model"];
        config.model.type = readOrDefault<std::string>(model, "type", config.model.type);
        config.model.engine_path = readOrDefault<std::string>(model, "engine_path", config.model.engine_path);
        config.model.labels_path = readOrDefault<std::string>(model, "labels_path", config.model.labels_path);
        config.model.gpu_id = readOrDefault<int>(model, "gpu_id", config.model.gpu_id);
        config.model.use_gpu_postprocess = readOrDefault<bool>(model, "use_gpu_postprocess", config.model.use_gpu_postprocess);
        config.model.cls_topk = readOrDefault<int>(model, "cls_topk", config.model.cls_topk);
        if (config.model.cls_topk <= 0) {
            config.model.cls_topk = 5;
        }

        auto output = root["output"];
        config.output.save_result_image = readOrDefault<bool>(output, "save_result_image", config.output.save_result_image);
        config.output.input_dir = readOrDefault<std::string>(output, "input_dir", config.output.input_dir);
        config.output.output_dir = readOrDefault<std::string>(output, "output_dir", config.output.output_dir);
        config.output.jpeg_quality = readOrDefault<int>(output, "jpeg_quality", config.output.jpeg_quality);
        if (config.output.jpeg_quality < 1) {
            config.output.jpeg_quality = 1;
        }
        if (config.output.jpeg_quality > 100) {
            config.output.jpeg_quality = 100;
        }


        auto video = root["video"];
        config.video.enabled = readOrDefault<bool>(video, "enabled", config.video.enabled);
        config.video.input_dir = readOrDefault<std::string>(video, "input_dir", config.video.input_dir);
        config.video.output_dir = readOrDefault<std::string>(video, "output_dir", config.video.output_dir);
        config.video.max_video_bytes = readOrDefault<long long>(video, "max_video_bytes", config.video.max_video_bytes);
        config.video.progress_update_interval_frames = readOrDefault<int>(video, "progress_update_interval_frames", config.video.progress_update_interval_frames);
        config.video.max_process_frames = readOrDefault<int>(video, "max_process_frames", config.video.max_process_frames);
        config.video.output_extension = readOrDefault<std::string>(video, "output_extension", config.video.output_extension);
        config.video.output_fourcc = readOrDefault<std::string>(video, "output_fourcc", config.video.output_fourcc);
        config.video.fallback_fps = readOrDefault<double>(video, "fallback_fps", config.video.fallback_fps);
        if (config.video.input_dir.empty()) {
            config.video.input_dir = "./temp/video/input";
        }
        if (config.video.output_dir.empty()) {
            config.video.output_dir = "./temp/video/output";
        }
        if (config.video.max_video_bytes < 0) {
            config.video.max_video_bytes = 0;
        }
        if (config.video.progress_update_interval_frames <= 0) {
            config.video.progress_update_interval_frames = 30;
        }
        if (config.video.max_process_frames < 0) {
            config.video.max_process_frames = 0;
        }
        if (config.video.output_extension.empty()) {
            config.video.output_extension = ".mp4";
        }
        if (!config.video.output_extension.empty() && config.video.output_extension[0] != '.') {
            config.video.output_extension = "." + config.video.output_extension;
        }
        if (config.video.output_fourcc.size() != 4) {
            config.video.output_fourcc = "mp4v";
        }
        if (config.video.fallback_fps <= 0.0 || config.video.fallback_fps > 240.0) {
            config.video.fallback_fps = 25.0;
        }


        auto stream = root["stream"];
        config.stream.enabled = readOrDefault<bool>(stream, "enabled", config.stream.enabled);
        config.stream.default_source_type = readOrDefault<std::string>(stream, "default_source_type", config.stream.default_source_type);
        config.stream.default_camera_id = readOrDefault<int>(stream, "default_camera_id", config.stream.default_camera_id);
        config.stream.default_file_path = readOrDefault<std::string>(stream, "default_file_path", config.stream.default_file_path);
        config.stream.camera_profiles_path = readOrDefault<std::string>(stream, "camera_profiles_path", config.stream.camera_profiles_path);
        config.stream.default_camera_profile = readOrDefault<std::string>(stream, "default_camera_profile", config.stream.default_camera_profile);
        config.stream.snapshot_dir = readOrDefault<std::string>(stream, "snapshot_dir", config.stream.snapshot_dir);
        config.stream.snapshot_interval_frames = readOrDefault<int>(stream, "snapshot_interval_frames", config.stream.snapshot_interval_frames);
        config.stream.target_fps = readOrDefault<int>(stream, "target_fps", config.stream.target_fps);
        config.stream.max_no_frame_count = readOrDefault<int>(stream, "max_no_frame_count", config.stream.max_no_frame_count);
        config.stream.enable_reconnect = readOrDefault<bool>(stream, "enable_reconnect", config.stream.enable_reconnect);
        config.stream.reconnect_max_attempts = readOrDefault<int>(stream, "reconnect_max_attempts", config.stream.reconnect_max_attempts);
        config.stream.reconnect_delay_ms = readOrDefault<int>(stream, "reconnect_delay_ms", config.stream.reconnect_delay_ms);
        config.stream.max_runtime_seconds = readOrDefault<int>(stream, "max_runtime_seconds", config.stream.max_runtime_seconds);
        config.stream.stale_timeout_ms = readOrDefault<int>(stream, "stale_timeout_ms", config.stream.stale_timeout_ms);
        config.stream.jpeg_quality = readOrDefault<int>(stream, "jpeg_quality", config.stream.jpeg_quality);
        if (config.stream.default_source_type.empty()) {
            config.stream.default_source_type = "camera";
        }
        if (config.stream.default_camera_id < 0) {
            config.stream.default_camera_id = 0;
        }
        if (config.stream.snapshot_dir.empty()) {
            config.stream.snapshot_dir = "./runtime/output/streams";
        }
        if (config.stream.snapshot_interval_frames <= 0) {
            config.stream.snapshot_interval_frames = 5;
        }
        if (config.stream.target_fps < 0) {
            config.stream.target_fps = 0;
        }
        if (config.stream.target_fps > 120) {
            config.stream.target_fps = 120;
        }
        if (config.stream.max_no_frame_count <= 0) {
            config.stream.max_no_frame_count = 30;
        }
        if (config.stream.reconnect_max_attempts < 0) {
            config.stream.reconnect_max_attempts = 0;
        }
        if (config.stream.reconnect_max_attempts > 100) {
            config.stream.reconnect_max_attempts = 100;
        }
        if (config.stream.reconnect_delay_ms < 100) {
            config.stream.reconnect_delay_ms = 100;
        }
        if (config.stream.reconnect_delay_ms > 60000) {
            config.stream.reconnect_delay_ms = 60000;
        }
        if (config.stream.max_runtime_seconds < 0) {
            config.stream.max_runtime_seconds = 0;
        }
        if (config.stream.stale_timeout_ms < 5000) {
            config.stream.stale_timeout_ms = 5000;
        }
        if (config.stream.jpeg_quality < 1) {
            config.stream.jpeg_quality = 1;
        }
        if (config.stream.jpeg_quality > 100) {
            config.stream.jpeg_quality = 100;
        }

        if (!config.stream.camera_profiles_path.empty()) {
            config.camera_profiles = loadCameraProfilesFromYaml(
                config.stream.camera_profiles_path,
                config.camera_profiles_error
            );
            if (!config.camera_profiles_error.empty()) {
                std::cerr << "Camera profile warning: " << config.camera_profiles_error << std::endl;
            }
        }

        auto capture = root["capture"];
        config.capture.backend = readOrDefault<std::string>(capture, "backend", config.capture.backend);
        config.capture.open_timeout_ms = readOrDefault<int>(capture, "open_timeout_ms", config.capture.open_timeout_ms);
        config.capture.read_timeout_ms = readOrDefault<int>(capture, "read_timeout_ms", config.capture.read_timeout_ms);
        config.capture.stale_frame_timeout_ms = readOrDefault<int>(capture, "stale_frame_timeout_ms", config.capture.stale_frame_timeout_ms);
        config.capture.buffer_size = readOrDefault<int>(capture, "buffer_size", config.capture.buffer_size);
        config.capture.transport = readOrDefault<std::string>(capture, "transport", config.capture.transport);
        config.capture.latest_frame_only = readOrDefault<bool>(capture, "latest_frame_only", config.capture.latest_frame_only);
        config.capture.warmup_frames = readOrDefault<int>(capture, "warmup_frames", config.capture.warmup_frames);
        config.capture.reconnect_max_attempts = readOrDefault<int>(capture, "reconnect_max_attempts", config.capture.reconnect_max_attempts);
        config.capture.reconnect_initial_delay_ms = readOrDefault<int>(capture, "reconnect_initial_delay_ms", config.capture.reconnect_initial_delay_ms);
        config.capture.reconnect_max_delay_ms = readOrDefault<int>(capture, "reconnect_max_delay_ms", config.capture.reconnect_max_delay_ms);
        config.capture.status_update_interval_ms = readOrDefault<int>(capture, "status_update_interval_ms", config.capture.status_update_interval_ms);
        config.capture.allow_backend_fallback = readOrDefault<bool>(capture, "allow_backend_fallback", config.capture.allow_backend_fallback);

        std::transform(config.capture.backend.begin(), config.capture.backend.end(), config.capture.backend.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(config.capture.transport.begin(), config.capture.transport.end(), config.capture.transport.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (config.capture.backend != "ffmpeg") config.capture.backend = "ffmpeg";
        if (config.capture.transport != "tcp" && config.capture.transport != "udp") config.capture.transport = "tcp";
        config.capture.open_timeout_ms = std::clamp(config.capture.open_timeout_ms, 1000, 120000);
        config.capture.read_timeout_ms = std::clamp(config.capture.read_timeout_ms, 500, 120000);
        config.capture.stale_frame_timeout_ms = std::clamp(config.capture.stale_frame_timeout_ms, 1000, 300000);
        config.capture.buffer_size = std::clamp(config.capture.buffer_size, 1, 16);
        config.capture.warmup_frames = std::clamp(config.capture.warmup_frames, 0, 1000);
        config.capture.reconnect_max_attempts = std::clamp(config.capture.reconnect_max_attempts, 0, 1000000);
        config.capture.reconnect_initial_delay_ms = std::clamp(config.capture.reconnect_initial_delay_ms, 100, 60000);
        config.capture.reconnect_max_delay_ms = std::clamp(config.capture.reconnect_max_delay_ms,
            config.capture.reconnect_initial_delay_ms, 300000);
        config.capture.status_update_interval_ms = std::clamp(config.capture.status_update_interval_ms, 200, 10000);

        auto camera_hub = root["camera_hub"];
        config.camera_hub.enabled = readOrDefault<bool>(camera_hub, "enabled", config.camera_hub.enabled);
        config.camera_hub.max_active_hubs = readOrDefault<int>(
            camera_hub, "max_active_hubs", config.camera_hub.max_active_hubs);
        config.camera_hub.idle_grace_ms = readOrDefault<int>(
            camera_hub, "idle_grace_ms", config.camera_hub.idle_grace_ms);
        config.camera_hub.require_ffmpeg_backend = readOrDefault<bool>(
            camera_hub, "require_ffmpeg_backend", config.camera_hub.require_ffmpeg_backend);
        config.camera_hub.status_update_interval_ms = readOrDefault<int>(
            camera_hub, "status_update_interval_ms", config.camera_hub.status_update_interval_ms);
        config.camera_hub.max_active_hubs = std::clamp(config.camera_hub.max_active_hubs, 1, 64);
        config.camera_hub.idle_grace_ms = std::clamp(config.camera_hub.idle_grace_ms, 0, 300000);
        config.camera_hub.status_update_interval_ms = std::clamp(
            config.camera_hub.status_update_interval_ms, 200, 10000);

        auto camera_tasks = root["camera_tasks"];
        config.camera_tasks.enabled = readOrDefault<bool>(camera_tasks, "enabled", config.camera_tasks.enabled);
        config.camera_tasks.postgres_dsn_env = readOrDefault<std::string>(
            camera_tasks, "postgres_dsn_env", config.camera_tasks.postgres_dsn_env);
        config.camera_tasks.output_dir = readOrDefault<std::string>(
            camera_tasks, "output_dir", config.camera_tasks.output_dir);
        config.camera_tasks.admin_ui_dir = readOrDefault<std::string>(
            camera_tasks, "admin_ui_dir", config.camera_tasks.admin_ui_dir);
        config.camera_tasks.command_stream_key = readOrDefault<std::string>(
            camera_tasks, "command_stream_key", config.camera_tasks.command_stream_key);
        config.camera_tasks.consumer_group = readOrDefault<std::string>(
            camera_tasks, "consumer_group", config.camera_tasks.consumer_group);
        config.camera_tasks.max_active_runs = readOrDefault<int>(
            camera_tasks, "max_active_runs", config.camera_tasks.max_active_runs);
        config.camera_tasks.writer_threads = readOrDefault<int>(
            camera_tasks, "writer_threads", config.camera_tasks.writer_threads);
        config.camera_tasks.writer_queue_capacity = readOrDefault<int>(
            camera_tasks, "writer_queue_capacity", config.camera_tasks.writer_queue_capacity);
        config.camera_tasks.writer_queue_capacity_per_run = readOrDefault<int>(
            camera_tasks, "writer_queue_capacity_per_run", config.camera_tasks.writer_queue_capacity_per_run);
        config.camera_tasks.status_ttl_seconds = readOrDefault<int>(
            camera_tasks, "status_ttl_seconds", config.camera_tasks.status_ttl_seconds);
        config.camera_tasks.lease_ttl_seconds = readOrDefault<int>(
            camera_tasks, "lease_ttl_seconds", config.camera_tasks.lease_ttl_seconds);
        config.camera_tasks.lease_refresh_seconds = readOrDefault<int>(
            camera_tasks, "lease_refresh_seconds", config.camera_tasks.lease_refresh_seconds);
        config.camera_tasks.stale_run_timeout_ms = readOrDefault<int>(
            camera_tasks, "stale_run_timeout_ms", config.camera_tasks.stale_run_timeout_ms);
        config.camera_tasks.retention_sweep_interval_seconds = readOrDefault<int>(
            camera_tasks, "retention_sweep_interval_seconds",
            config.camera_tasks.retention_sweep_interval_seconds);
        config.camera_tasks.retention_batch_size = readOrDefault<int>(
            camera_tasks, "retention_batch_size", config.camera_tasks.retention_batch_size);
        config.camera_tasks.admin_token_env = readOrDefault<std::string>(
            camera_tasks, "admin_token_env", config.camera_tasks.admin_token_env);
        const auto camera_task_storage = camera_tasks["storage"];
        config.camera_tasks.storage.max_archive_bytes = readOrDefault<long long>(
            camera_task_storage, "max_archive_bytes", config.camera_tasks.storage.max_archive_bytes);
        config.camera_tasks.storage.min_free_bytes = readOrDefault<long long>(
            camera_task_storage, "min_free_bytes", config.camera_tasks.storage.min_free_bytes);
        config.camera_tasks.storage.high_watermark_percent = readOrDefault<int>(
            camera_task_storage, "high_watermark_percent",
            config.camera_tasks.storage.high_watermark_percent);
        config.camera_tasks.storage.critical_watermark_percent = readOrDefault<int>(
            camera_task_storage, "critical_watermark_percent",
            config.camera_tasks.storage.critical_watermark_percent);
        config.camera_tasks.storage.pressure_cleanup_batch_size = readOrDefault<int>(
            camera_task_storage, "pressure_cleanup_batch_size",
            config.camera_tasks.storage.pressure_cleanup_batch_size);
        config.camera_tasks.storage.backup_dir = readOrDefault<std::string>(
            camera_task_storage, "backup_dir", config.camera_tasks.storage.backup_dir);
        config.camera_tasks.storage.backup_retention_count = readOrDefault<int>(
            camera_task_storage, "backup_retention_count",
            config.camera_tasks.storage.backup_retention_count);
        const auto camera_task_defaults = camera_tasks["defaults"];
        config.camera_tasks.defaults.frame_interval_ms = readOrDefault<int>(
            camera_task_defaults, "frame_interval_ms", config.camera_tasks.defaults.frame_interval_ms);
        config.camera_tasks.defaults.output_mode = toLowerString(readOrDefault<std::string>(
            camera_task_defaults, "output_mode", config.camera_tasks.defaults.output_mode));
        config.camera_tasks.defaults.jpeg_quality = readOrDefault<int>(
            camera_task_defaults, "jpeg_quality", config.camera_tasks.defaults.jpeg_quality);
        config.camera_tasks.defaults.max_width = readOrDefault<int>(
            camera_task_defaults, "max_width", config.camera_tasks.defaults.max_width);
        config.camera_tasks.defaults.max_height = readOrDefault<int>(
            camera_task_defaults, "max_height", config.camera_tasks.defaults.max_height);
        config.camera_tasks.defaults.retention_days = readOrDefault<int>(
            camera_task_defaults, "retention_days", config.camera_tasks.defaults.retention_days);
        config.camera_tasks.defaults.max_saved_frames = readOrDefault<int>(
            camera_task_defaults, "max_saved_frames", config.camera_tasks.defaults.max_saved_frames);
        config.camera_tasks.defaults.analysis_enabled = readOrDefault<bool>(
            camera_task_defaults, "analysis_enabled", config.camera_tasks.defaults.analysis_enabled);
        config.camera_tasks.defaults.target_infer_fps = readOrDefault<double>(
            camera_task_defaults, "target_infer_fps", config.camera_tasks.defaults.target_infer_fps);
        config.camera_tasks.defaults.algorithm_profile = readOrDefault<std::string>(
            camera_task_defaults, "algorithm_profile", config.camera_tasks.defaults.algorithm_profile);
        config.camera_tasks.defaults.algorithms = readOrDefault<std::vector<std::string>>(
            camera_task_defaults, "algorithms", config.camera_tasks.defaults.algorithms);
        config.camera_tasks.defaults.callback_profile = readOrDefault<std::string>(
            camera_task_defaults, "callback_profile", config.camera_tasks.defaults.callback_profile);

        const auto analysis = root["analysis"];
        config.analysis.enabled = readOrDefault<bool>(
            analysis, "enabled", config.analysis.enabled);
        config.analysis.inference_workers = readOrDefault<int>(
            analysis, "inference_workers", config.analysis.inference_workers);
        config.analysis.model_init_timeout_ms = readOrDefault<int>(
            analysis, "model_init_timeout_ms", config.analysis.model_init_timeout_ms);
        config.analysis.supported_algorithms = readOrDefault<std::vector<std::string>>(
            analysis, "supported_algorithms", config.analysis.supported_algorithms);
        config.analysis.inference_workers = std::clamp(
            config.analysis.inference_workers, 1, 16);
        config.analysis.model_init_timeout_ms = std::clamp(
            config.analysis.model_init_timeout_ms, 1000, 600000);
        std::sort(
            config.analysis.supported_algorithms.begin(),
            config.analysis.supported_algorithms.end());
        config.analysis.supported_algorithms.erase(
            std::unique(
                config.analysis.supported_algorithms.begin(),
                config.analysis.supported_algorithms.end()),
            config.analysis.supported_algorithms.end());

        config.camera_tasks.max_active_runs = std::clamp(config.camera_tasks.max_active_runs, 1, 64);
        config.camera_tasks.writer_threads = std::clamp(config.camera_tasks.writer_threads, 1, 16);
        config.camera_tasks.writer_queue_capacity = std::clamp(
            config.camera_tasks.writer_queue_capacity, 1, 10000);
        config.camera_tasks.writer_queue_capacity_per_run = std::clamp(
            config.camera_tasks.writer_queue_capacity_per_run, 1,
            config.camera_tasks.writer_queue_capacity);
        config.camera_tasks.status_ttl_seconds = std::clamp(
            config.camera_tasks.status_ttl_seconds, 60, 31536000);
        config.camera_tasks.lease_ttl_seconds = std::clamp(
            config.camera_tasks.lease_ttl_seconds, 5, 3600);
        config.camera_tasks.lease_refresh_seconds = std::clamp(
            config.camera_tasks.lease_refresh_seconds, 1,
            std::max(1, config.camera_tasks.lease_ttl_seconds - 1));
        config.camera_tasks.stale_run_timeout_ms = std::clamp(
            config.camera_tasks.stale_run_timeout_ms, 5000, 3600000);
        config.camera_tasks.retention_sweep_interval_seconds = std::clamp(
            config.camera_tasks.retention_sweep_interval_seconds, 5, 86400);
        config.camera_tasks.retention_batch_size = std::clamp(
            config.camera_tasks.retention_batch_size, 1, 10000);
        config.camera_tasks.storage.max_archive_bytes = std::max(0LL,
            config.camera_tasks.storage.max_archive_bytes);
        config.camera_tasks.storage.min_free_bytes = std::max(0LL,
            config.camera_tasks.storage.min_free_bytes);
        config.camera_tasks.storage.high_watermark_percent = std::clamp(
            config.camera_tasks.storage.high_watermark_percent, 1, 98);
        config.camera_tasks.storage.critical_watermark_percent = std::clamp(
            config.camera_tasks.storage.critical_watermark_percent,
            config.camera_tasks.storage.high_watermark_percent + 1, 99);
        config.camera_tasks.storage.pressure_cleanup_batch_size = std::clamp(
            config.camera_tasks.storage.pressure_cleanup_batch_size, 1, 10000);
        config.camera_tasks.storage.backup_retention_count = std::clamp(
            config.camera_tasks.storage.backup_retention_count, 1, 365);
        config.camera_tasks.defaults.frame_interval_ms = std::clamp(
            config.camera_tasks.defaults.frame_interval_ms, 100, 3600000);
        config.camera_tasks.defaults.jpeg_quality = std::clamp(
            config.camera_tasks.defaults.jpeg_quality, 1, 100);
        config.camera_tasks.defaults.max_width = std::clamp(
            config.camera_tasks.defaults.max_width, 0, 8192);
        config.camera_tasks.defaults.max_height = std::clamp(
            config.camera_tasks.defaults.max_height, 0, 8192);
        config.camera_tasks.defaults.retention_days = std::clamp(
            config.camera_tasks.defaults.retention_days, 1, 3650);
        config.camera_tasks.defaults.max_saved_frames = std::clamp(
            config.camera_tasks.defaults.max_saved_frames, 1, 1000000);
        config.camera_tasks.defaults.target_infer_fps = std::clamp(
            config.camera_tasks.defaults.target_infer_fps, 0.1, 120.0);
        if (config.camera_tasks.defaults.output_mode != "latest" &&
            config.camera_tasks.defaults.output_mode != "archive" &&
            config.camera_tasks.defaults.output_mode != "both") {
            config.camera_tasks.config_error = "camera_tasks.defaults.output_mode must be latest, archive, or both";
        }
        if (config.camera_tasks.enabled &&
            (config.camera_tasks.postgres_dsn_env.empty() || config.camera_tasks.output_dir.empty() ||
             config.camera_tasks.command_stream_key.empty() || config.camera_tasks.consumer_group.empty())) {
            config.camera_tasks.config_error = "camera_tasks PostgreSQL DSN env, paths, and Redis stream/group must not be empty";
        }
        if (config.camera_hub.require_ffmpeg_backend && config.capture.allow_backend_fallback) {
            config.camera_tasks.config_error = "camera_hub requires FFmpeg but capture fallback is enabled";
        }

        auto people_flow = root["people_flow"];
        config.people_flow.enabled = readOrDefault<bool>(people_flow, "enabled", config.people_flow.enabled);
        config.people_flow.camera_id = readOrDefault<std::string>(people_flow, "camera_id", config.people_flow.camera_id);
        config.people_flow.camera_profile = readOrDefault<std::string>(people_flow, "camera_profile", config.people_flow.camera_profile);
        config.people_flow.config_version = readOrDefault<std::string>(people_flow, "config_version", config.people_flow.config_version);
        config.people_flow.output_dir = readOrDefault<std::string>(people_flow, "output_dir", config.people_flow.output_dir);
        config.people_flow.report_dir = readOrDefault<std::string>(people_flow, "report_dir", config.people_flow.report_dir);
        config.people_flow.target_infer_fps = readOrDefault<int>(people_flow, "target_infer_fps", config.people_flow.target_infer_fps);
        config.people_flow.snapshot_fps = readOrDefault<int>(people_flow, "snapshot_fps", config.people_flow.snapshot_fps);
        config.people_flow.jpeg_quality = readOrDefault<int>(people_flow, "jpeg_quality", config.people_flow.jpeg_quality);
        config.people_flow.initial_occupancy = readOrDefault<int>(people_flow, "initial_occupancy", config.people_flow.initial_occupancy);
        config.people_flow.warmup_frames_after_reconnect = readOrDefault<int>(people_flow, "warmup_frames_after_reconnect", config.people_flow.warmup_frames_after_reconnect);
        config.people_flow.active_ttl_seconds = readOrDefault<int>(people_flow, "active_ttl_seconds", config.people_flow.active_ttl_seconds);
        config.people_flow.realtime_ttl_seconds = readOrDefault<int>(people_flow, "realtime_ttl_seconds", config.people_flow.realtime_ttl_seconds);
        config.people_flow.session_ttl_seconds = readOrDefault<int>(people_flow, "session_ttl_seconds", config.people_flow.session_ttl_seconds);
        config.people_flow.stale_timeout_ms = readOrDefault<int>(people_flow, "stale_timeout_ms", config.people_flow.stale_timeout_ms);
        config.people_flow.admin_token_env = readOrDefault<std::string>(people_flow, "admin_token_env", config.people_flow.admin_token_env);

        const auto person = people_flow["person"];
        config.people_flow.person.class_id = readOrDefault<int>(person, "class_id", config.people_flow.person.class_id);
        config.people_flow.person.conf_high = readOrDefault<double>(person, "conf_high", config.people_flow.person.conf_high);
        config.people_flow.person.conf_low = readOrDefault<double>(person, "conf_low", config.people_flow.person.conf_low);
        config.people_flow.person.min_width_px = readOrDefault<int>(person, "min_width_px", config.people_flow.person.min_width_px);
        config.people_flow.person.min_height_px = readOrDefault<int>(person, "min_height_px", config.people_flow.person.min_height_px);
        config.people_flow.person.max_aspect_ratio = readOrDefault<double>(person, "max_aspect_ratio", config.people_flow.person.max_aspect_ratio);
        config.people_flow.person.anchor_point = toLowerString(readOrDefault<std::string>(person, "anchor_point", config.people_flow.person.anchor_point));

        const auto tracker = people_flow["tracker"];
        config.people_flow.tracker.min_hits = readOrDefault<int>(tracker, "min_hits", config.people_flow.tracker.min_hits);
        config.people_flow.tracker.max_age_frames = readOrDefault<int>(tracker, "max_age_frames", config.people_flow.tracker.max_age_frames);
        config.people_flow.tracker.match_iou_threshold = readOrDefault<double>(tracker, "match_iou_threshold", config.people_flow.tracker.match_iou_threshold);
        config.people_flow.tracker.center_distance_gate_norm = readOrDefault<double>(tracker, "center_distance_gate_norm", config.people_flow.tracker.center_distance_gate_norm);
        config.people_flow.tracker.velocity_smoothing = readOrDefault<double>(tracker, "velocity_smoothing", config.people_flow.tracker.velocity_smoothing);
        config.people_flow.tracker.trail_length = readOrDefault<int>(tracker, "trail_length", config.people_flow.tracker.trail_length);
        config.people_flow.tracker.use_alpha_beta_filter = readOrDefault<bool>(tracker, "use_alpha_beta_filter", config.people_flow.tracker.use_alpha_beta_filter);
        config.people_flow.tracker.motion_alpha = readOrDefault<double>(tracker, "motion_alpha", config.people_flow.tracker.motion_alpha);
        config.people_flow.tracker.motion_beta = readOrDefault<double>(tracker, "motion_beta", config.people_flow.tracker.motion_beta);
        config.people_flow.tracker.max_prediction_ms = readOrDefault<int>(tracker, "max_prediction_ms", config.people_flow.tracker.max_prediction_ms);

        const auto security = people_flow["security"];
        auto& security_config = config.people_flow.security;
        security_config.enabled = readOrDefault<bool>(security, "enabled", security_config.enabled);
        security_config.mode = toLowerString(readOrDefault<std::string>(security, "mode", security_config.mode));
        security_config.draw_zones = readOrDefault<bool>(security, "draw_zones", security_config.draw_zones);
        security_config.draw_pose = readOrDefault<bool>(security, "draw_pose", security_config.draw_pose);
        security_config.draw_stage_panel = readOrDefault<bool>(security, "draw_stage_panel", security_config.draw_stage_panel);
        security_config.max_recent_events = readOrDefault<int>(security, "max_recent_events", security_config.max_recent_events);
        security_config.event_marker_hold_frames = readOrDefault<int>(security, "event_marker_hold_frames", security_config.event_marker_hold_frames);
        security_config.pose_match_iou_threshold = readOrDefault<double>(security, "pose_match_iou_threshold", security_config.pose_match_iou_threshold);
        security_config.pose_match_distance_norm = readOrDefault<double>(security, "pose_match_distance_norm", security_config.pose_match_distance_norm);
        security_config.temporal_motion_speed_px_s = readOrDefault<double>(security, "temporal_motion_speed_px_s", security_config.temporal_motion_speed_px_s);
        security_config.temporal_demo_label = readOrDefault<std::string>(security, "temporal_demo_label", security_config.temporal_demo_label);
        security_config.state_file_name = readOrDefault<std::string>(security, "state_file_name", security_config.state_file_name);
        if (security && security["zones"]) {
            const auto zone_nodes = security["zones"];
            if (!zone_nodes.IsSequence() || zone_nodes.size() == 0) {
                config.people_flow.config_error = "people_flow.security.zones must be a non-empty sequence";
            }
            else {
                std::vector<SecurityZoneConfig> zones;
                bool valid_zones = true;
                for (const auto& zone_node : zone_nodes) {
                    SecurityZoneConfig zone;
                    zone.zone_id = readOrDefault<std::string>(zone_node, "zone_id", "");
                    zone.name = readOrDefault<std::string>(zone_node, "name", zone.zone_id);
                    zone.enter_confirm_frames = readOrDefault<int>(zone_node, "enter_confirm_frames", zone.enter_confirm_frames);
                    zone.exit_confirm_frames = readOrDefault<int>(zone_node, "exit_confirm_frames", zone.exit_confirm_frames);
                    zone.dwell_alarm_ms = readOrDefault<long long>(zone_node, "dwell_alarm_ms", zone.dwell_alarm_ms);
                    zone.cooldown_ms = readOrDefault<long long>(zone_node, "cooldown_ms", zone.cooldown_ms);
                    zone.max_missed_frames = readOrDefault<int>(zone_node, "max_missed_frames", zone.max_missed_frames);
                    zone.severity = readOrDefault<int>(zone_node, "severity", zone.severity);
                    zone.emit_enter = readOrDefault<bool>(zone_node, "emit_enter", zone.emit_enter);
                    zone.emit_exit = readOrDefault<bool>(zone_node, "emit_exit", zone.emit_exit);
                    zone.emit_dwell = readOrDefault<bool>(zone_node, "emit_dwell", zone.emit_dwell);
                    const auto polygon = zone_node["polygon_norm"];
                    if (zone.zone_id.empty() || !polygon || !polygon.IsSequence() || polygon.size() < 3) {
                        valid_zones = false;
                        break;
                    }
                    for (const auto& point_node : polygon) {
                        if (!point_node.IsSequence() || point_node.size() != 2) {
                            valid_zones = false;
                            break;
                        }
                        const double x = point_node[0].as<double>();
                        const double y = point_node[1].as<double>();
                        if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                            valid_zones = false;
                            break;
                        }
                        zone.polygon_norm.push_back({ x, y });
                    }
                    if (!valid_zones) break;
                    zones.push_back(std::move(zone));
                }
                if (valid_zones) security_config.zones = std::move(zones);
                else config.people_flow.config_error = "people_flow.security.zones contains an invalid zone or polygon";
            }
        }
        const auto pose_action = security["pose_actions"];
        security_config.pose.min_keypoint_confidence = readOrDefault<double>(pose_action, "min_keypoint_confidence", security_config.pose.min_keypoint_confidence);
        security_config.pose.confirm_frames = readOrDefault<int>(pose_action, "confirm_frames", security_config.pose.confirm_frames);
        security_config.pose.release_frames = readOrDefault<int>(pose_action, "release_frames", security_config.pose.release_frames);
        security_config.pose.cooldown_ms = readOrDefault<long long>(pose_action, "cooldown_ms", security_config.pose.cooldown_ms);
        security_config.pose.fall_trunk_angle_deg = readOrDefault<double>(pose_action, "fall_trunk_angle_deg", security_config.pose.fall_trunk_angle_deg);
        security_config.pose.fall_bbox_aspect_ratio = readOrDefault<double>(pose_action, "fall_bbox_aspect_ratio", security_config.pose.fall_bbox_aspect_ratio);
        security_config.pose.crouch_knee_angle_deg = readOrDefault<double>(pose_action, "crouch_knee_angle_deg", security_config.pose.crouch_knee_angle_deg);
        security_config.pose.running_speed_px_s = readOrDefault<double>(pose_action, "running_speed_px_s", security_config.pose.running_speed_px_s);
        const auto temporal_action = security["temporal_action"];
        security_config.temporal.window_size = readOrDefault<std::size_t>(temporal_action, "window_size", security_config.temporal.window_size);
        security_config.temporal.min_samples = readOrDefault<std::size_t>(temporal_action, "min_samples", security_config.temporal.min_samples);
        security_config.temporal.confirm_windows = readOrDefault<int>(temporal_action, "confirm_windows", security_config.temporal.confirm_windows);
        security_config.temporal.release_windows = readOrDefault<int>(temporal_action, "release_windows", security_config.temporal.release_windows);
        security_config.temporal.cooldown_ms = readOrDefault<long long>(temporal_action, "cooldown_ms", security_config.temporal.cooldown_ms);
        security_config.temporal.start_threshold = readOrDefault<double>(temporal_action, "start_threshold", security_config.temporal.start_threshold);
        security_config.temporal.end_threshold = readOrDefault<double>(temporal_action, "end_threshold", security_config.temporal.end_threshold);
        security_config.temporal.severity = readOrDefault<int>(temporal_action, "severity", security_config.temporal.severity);

        const auto counting = people_flow["counting"];
        config.people_flow.counting.line_id = readOrDefault<std::string>(counting, "line_id", config.people_flow.counting.line_id);
        config.people_flow.counting.transition_positive_to_negative = readOrDefault<std::string>(counting, "transition_positive_to_negative", config.people_flow.counting.transition_positive_to_negative);
        config.people_flow.counting.hysteresis_px = readOrDefault<double>(counting, "hysteresis_px", config.people_flow.counting.hysteresis_px);
        config.people_flow.counting.min_hits_for_count = readOrDefault<int>(counting, "min_hits_for_count", config.people_flow.counting.min_hits_for_count);
        config.people_flow.counting.finite_segment_extension_norm = readOrDefault<double>(counting, "finite_segment_extension_norm", config.people_flow.counting.finite_segment_extension_norm);
        config.people_flow.counting.rearm_distance_px = readOrDefault<double>(counting, "rearm_distance_px", config.people_flow.counting.rearm_distance_px);
        config.people_flow.counting.rearm_frames = readOrDefault<int>(counting, "rearm_frames", config.people_flow.counting.rearm_frames);
        config.people_flow.counting.min_crossing_interval_ms = readOrDefault<int>(counting, "min_crossing_interval_ms", config.people_flow.counting.min_crossing_interval_ms);
        config.people_flow.counting.max_crossing_gap_ms = readOrDefault<int>(counting, "max_crossing_gap_ms", config.people_flow.counting.max_crossing_gap_ms);
        if (counting && counting["line_a_norm"] && !readNormalizedPoint(counting["line_a_norm"], config.people_flow.counting.line_a_norm)) {
            config.people_flow.config_error = "people_flow.counting.line_a_norm must contain two values in [0,1]";
        }
        if (counting && counting["line_b_norm"] && !readNormalizedPoint(counting["line_b_norm"], config.people_flow.counting.line_b_norm)) {
            config.people_flow.config_error = "people_flow.counting.line_b_norm must contain two values in [0,1]";
        }

        const auto roi = people_flow["roi"];
        config.people_flow.roi.enabled = readOrDefault<bool>(roi, "enabled", config.people_flow.roi.enabled);
        if (roi && roi["polygon_norm"]) {
            const auto polygon = roi["polygon_norm"];
            if (!polygon.IsSequence() || polygon.size() < 3) {
                config.people_flow.config_error = "people_flow.roi.polygon_norm requires at least three points";
            }
            else {
                std::vector<NormalizedPoint> points;
                bool valid = true;
                for (const auto& node : polygon) {
                    NormalizedPoint point;
                    if (!readNormalizedPoint(node, point)) {
                        valid = false;
                        break;
                    }
                    points.push_back(point);
                }
                if (valid) config.people_flow.roi.polygon_norm = std::move(points);
                else config.people_flow.config_error = "people_flow.roi.polygon_norm contains an invalid point";
            }
        }

        const auto visualization = people_flow["visualization"];
        auto& visual = config.people_flow.visualization;
        visual.enabled = readOrDefault<bool>(visualization, "enabled", visual.enabled);
        visual.draw_raw_person_detections = readOrDefault<bool>(visualization, "draw_raw_person_detections", visual.draw_raw_person_detections);
        visual.draw_accepted_high_detections = readOrDefault<bool>(visualization, "draw_accepted_high_detections", visual.draw_accepted_high_detections);
        visual.draw_accepted_low_detections = readOrDefault<bool>(visualization, "draw_accepted_low_detections", visual.draw_accepted_low_detections);
        visual.draw_rejected_detections = readOrDefault<bool>(visualization, "draw_rejected_detections", visual.draw_rejected_detections);
        visual.draw_rejection_reason = readOrDefault<bool>(visualization, "draw_rejection_reason", visual.draw_rejection_reason);
        visual.draw_tentative_tracks = readOrDefault<bool>(visualization, "draw_tentative_tracks", visual.draw_tentative_tracks);
        visual.draw_confirmed_tracks = readOrDefault<bool>(visualization, "draw_confirmed_tracks", visual.draw_confirmed_tracks);
        visual.draw_missed_tracks = readOrDefault<bool>(visualization, "draw_missed_tracks", visual.draw_missed_tracks);
        visual.draw_track_id = readOrDefault<bool>(visualization, "draw_track_id", visual.draw_track_id);
        visual.draw_track_stats = readOrDefault<bool>(visualization, "draw_track_stats", visual.draw_track_stats);
        visual.draw_velocity = readOrDefault<bool>(visualization, "draw_velocity", visual.draw_velocity);
        visual.draw_anchor_points = readOrDefault<bool>(visualization, "draw_anchor_points", visual.draw_anchor_points);
        visual.draw_trails = readOrDefault<bool>(visualization, "draw_trails", visual.draw_trails);
        visual.draw_roi = readOrDefault<bool>(visualization, "draw_roi", visual.draw_roi);
        visual.fill_roi = readOrDefault<bool>(visualization, "fill_roi", visual.fill_roi);
        visual.draw_counting_line = readOrDefault<bool>(visualization, "draw_counting_line", visual.draw_counting_line);
        visual.draw_line_endpoints = readOrDefault<bool>(visualization, "draw_line_endpoints", visual.draw_line_endpoints);
        visual.draw_hysteresis_band = readOrDefault<bool>(visualization, "draw_hysteresis_band", visual.draw_hysteresis_band);
        visual.draw_side_labels = readOrDefault<bool>(visualization, "draw_side_labels", visual.draw_side_labels);
        visual.draw_counter_state = readOrDefault<bool>(visualization, "draw_counter_state", visual.draw_counter_state);
        visual.draw_event_markers = readOrDefault<bool>(visualization, "draw_event_markers", visual.draw_event_markers);
        visual.draw_status_panel = readOrDefault<bool>(visualization, "draw_status_panel", visual.draw_status_panel);
        visual.draw_filter_statistics = readOrDefault<bool>(visualization, "draw_filter_statistics", visual.draw_filter_statistics);
        visual.draw_legend = readOrDefault<bool>(visualization, "draw_legend", visual.draw_legend);
        visual.compact_panel_auto = readOrDefault<bool>(visualization, "compact_panel_auto", visual.compact_panel_auto);
        visual.save_event_frames = readOrDefault<bool>(visualization, "save_event_frames", visual.save_event_frames);
        visual.event_marker_hold_frames = readOrDefault<int>(visualization, "event_marker_hold_frames", visual.event_marker_hold_frames);
        visual.save_debug_frame_json = readOrDefault<bool>(visualization, "save_debug_frame_json", visual.save_debug_frame_json);
        visual.box_thickness = readOrDefault<int>(visualization, "box_thickness", visual.box_thickness);
        visual.line_thickness = readOrDefault<int>(visualization, "line_thickness", visual.line_thickness);
        visual.trail_thickness = readOrDefault<int>(visualization, "trail_thickness", visual.trail_thickness);
        visual.font_scale = readOrDefault<double>(visualization, "font_scale", visual.font_scale);
        visual.ui_scale = readOrDefault<double>(visualization, "ui_scale", visual.ui_scale);

        const auto storage = people_flow["storage"];
        config.people_flow.storage.postgres_dsn_env = readOrDefault<std::string>(
            storage, "postgres_dsn_env", config.people_flow.storage.postgres_dsn_env);
        config.people_flow.storage.writer_queue_capacity = readOrDefault<int>(storage, "writer_queue_capacity", config.people_flow.storage.writer_queue_capacity);
        config.people_flow.storage.writer_batch_size = readOrDefault<int>(storage, "writer_batch_size", config.people_flow.storage.writer_batch_size);
        config.people_flow.storage.writer_flush_interval_ms = readOrDefault<int>(storage, "writer_flush_interval_ms", config.people_flow.storage.writer_flush_interval_ms);
        config.people_flow.storage.events_max_len = readOrDefault<int>(storage, "events_max_len", config.people_flow.storage.events_max_len);
        config.people_flow.storage.event_retention_days = readOrDefault<int>(storage, "event_retention_days", config.people_flow.storage.event_retention_days);
        config.people_flow.storage.aggregate_retention_days = readOrDefault<int>(storage, "aggregate_retention_days", config.people_flow.storage.aggregate_retention_days);
        config.people_flow.storage.evidence_on_crossing = readOrDefault<bool>(storage, "evidence_on_crossing", config.people_flow.storage.evidence_on_crossing);

        if (config.people_flow.camera_id.empty()) config.people_flow.camera_id = "entry_camera_01";
        if (config.people_flow.camera_profile.empty()) config.people_flow.camera_profile = config.people_flow.camera_id;
        if (config.people_flow.config_version.empty()) config.people_flow.config_version = "entry-line-v1";
        if (config.people_flow.output_dir.empty()) config.people_flow.output_dir = "./runtime/output/people_flow";
        if (config.people_flow.report_dir.empty()) config.people_flow.report_dir = "./reports/people_flow";
        config.people_flow.target_infer_fps = std::clamp(config.people_flow.target_infer_fps, 1, 60);
        config.people_flow.snapshot_fps = std::clamp(config.people_flow.snapshot_fps, 1, config.people_flow.target_infer_fps);
        config.people_flow.jpeg_quality = std::clamp(config.people_flow.jpeg_quality, 1, 100);
        config.people_flow.initial_occupancy = std::max(0, config.people_flow.initial_occupancy);
        config.people_flow.warmup_frames_after_reconnect = std::clamp(config.people_flow.warmup_frames_after_reconnect, 0, 1000);
        config.people_flow.active_ttl_seconds = std::clamp(config.people_flow.active_ttl_seconds, 10, 3600);
        config.people_flow.realtime_ttl_seconds = std::clamp(config.people_flow.realtime_ttl_seconds, 3, 3600);
        config.people_flow.session_ttl_seconds = std::clamp(config.people_flow.session_ttl_seconds, 60, 31536000);
        config.people_flow.stale_timeout_ms = std::clamp(config.people_flow.stale_timeout_ms, 5000, 300000);
        config.people_flow.active_ttl_seconds = std::max(
            config.people_flow.active_ttl_seconds,
            (config.people_flow.stale_timeout_ms + 999) / 1000 + 30
        );
        // The TensorRT decode plugin has an internal 0.10 candidate floor.
        config.people_flow.person.conf_low = std::clamp(config.people_flow.person.conf_low, 0.10, 1.0);
        config.people_flow.person.conf_high = std::clamp(config.people_flow.person.conf_high, config.people_flow.person.conf_low, 1.0);
        config.people_flow.person.min_width_px = std::max(1, config.people_flow.person.min_width_px);
        config.people_flow.person.min_height_px = std::max(1, config.people_flow.person.min_height_px);
        config.people_flow.person.max_aspect_ratio = std::clamp(config.people_flow.person.max_aspect_ratio, 1.0, 20.0);
        if (config.people_flow.person.anchor_point != "center" && config.people_flow.person.anchor_point != "bottom_center") {
            config.people_flow.person.anchor_point = "bottom_center";
        }
        config.people_flow.tracker.min_hits = std::clamp(config.people_flow.tracker.min_hits, 1, 100);
        config.people_flow.tracker.max_age_frames = std::clamp(config.people_flow.tracker.max_age_frames, 1, 10000);
        config.people_flow.tracker.match_iou_threshold = std::clamp(config.people_flow.tracker.match_iou_threshold, 0.0, 1.0);
        config.people_flow.tracker.center_distance_gate_norm = std::clamp(config.people_flow.tracker.center_distance_gate_norm, 0.001, 1.0);
        config.people_flow.tracker.velocity_smoothing = std::clamp(config.people_flow.tracker.velocity_smoothing, 0.0, 1.0);
        config.people_flow.tracker.trail_length = std::clamp(config.people_flow.tracker.trail_length, 1, 1000);
        config.people_flow.tracker.motion_alpha = std::clamp(config.people_flow.tracker.motion_alpha, 0.0, 1.0);
        config.people_flow.tracker.motion_beta = std::clamp(config.people_flow.tracker.motion_beta, 0.0, 1.0);
        config.people_flow.tracker.max_prediction_ms = std::clamp(config.people_flow.tracker.max_prediction_ms, 1, 10000);
        security_config.mode = "demo";
        security_config.max_recent_events = std::clamp(security_config.max_recent_events, 1, 500);
        security_config.event_marker_hold_frames = std::clamp(security_config.event_marker_hold_frames, 1, 600);
        security_config.pose_match_iou_threshold = std::clamp(security_config.pose_match_iou_threshold, 0.0, 1.0);
        security_config.pose_match_distance_norm = std::clamp(security_config.pose_match_distance_norm, 0.001, 1.0);
        security_config.temporal_motion_speed_px_s = std::clamp(security_config.temporal_motion_speed_px_s, 1.0, 10000.0);
        if (security_config.temporal_demo_label.empty()) security_config.temporal_demo_label = "RAPID_MOTION_DEMO";
        if (security_config.state_file_name.empty() ||
            security_config.state_file_name.find('/') != std::string::npos ||
            security_config.state_file_name.find('\\') != std::string::npos) {
            security_config.state_file_name = "security.json";
        }
        security_config.pose.min_keypoint_confidence = std::clamp(security_config.pose.min_keypoint_confidence, 0.0, 1.0);
        security_config.pose.confirm_frames = std::clamp(security_config.pose.confirm_frames, 1, 100);
        security_config.pose.release_frames = std::clamp(security_config.pose.release_frames, 1, 100);
        security_config.pose.cooldown_ms = std::clamp(security_config.pose.cooldown_ms, 0LL, 600000LL);
        security_config.pose.fall_trunk_angle_deg = std::clamp(security_config.pose.fall_trunk_angle_deg, 0.0, 90.0);
        security_config.pose.fall_bbox_aspect_ratio = std::clamp(security_config.pose.fall_bbox_aspect_ratio, 0.1, 10.0);
        security_config.pose.crouch_knee_angle_deg = std::clamp(security_config.pose.crouch_knee_angle_deg, 1.0, 179.0);
        security_config.pose.running_speed_px_s = std::clamp(security_config.pose.running_speed_px_s, 1.0, 10000.0);
        security_config.temporal.window_size = std::clamp<std::size_t>(security_config.temporal.window_size, 2, 300);
        security_config.temporal.min_samples = std::clamp<std::size_t>(security_config.temporal.min_samples, 1, security_config.temporal.window_size);
        security_config.temporal.confirm_windows = std::clamp(security_config.temporal.confirm_windows, 1, 100);
        security_config.temporal.release_windows = std::clamp(security_config.temporal.release_windows, 1, 100);
        security_config.temporal.cooldown_ms = std::clamp(security_config.temporal.cooldown_ms, 0LL, 600000LL);
        security_config.temporal.start_threshold = std::clamp(security_config.temporal.start_threshold, 0.0, 1.0);
        security_config.temporal.end_threshold = std::clamp(security_config.temporal.end_threshold, 0.0, security_config.temporal.start_threshold);
        security_config.temporal.severity = std::clamp(security_config.temporal.severity, 1, 5);
        for (SecurityZoneConfig& zone : security_config.zones) {
            zone.enter_confirm_frames = std::clamp(zone.enter_confirm_frames, 1, 100);
            zone.exit_confirm_frames = std::clamp(zone.exit_confirm_frames, 1, 100);
            zone.dwell_alarm_ms = std::clamp(zone.dwell_alarm_ms, 0LL, 86400000LL);
            zone.cooldown_ms = std::clamp(zone.cooldown_ms, 0LL, 86400000LL);
            zone.max_missed_frames = std::clamp(zone.max_missed_frames, 0, 100);
            zone.severity = std::clamp(zone.severity, 1, 5);
        }
        config.people_flow.counting.hysteresis_px = std::clamp(config.people_flow.counting.hysteresis_px, 0.0, 1000.0);
        config.people_flow.counting.min_hits_for_count = std::clamp(config.people_flow.counting.min_hits_for_count, 1, 100);
        config.people_flow.counting.finite_segment_extension_norm = std::clamp(config.people_flow.counting.finite_segment_extension_norm, 0.0, 1.0);
        config.people_flow.counting.rearm_distance_px = std::clamp(
            config.people_flow.counting.rearm_distance_px,
            config.people_flow.counting.hysteresis_px + 1.0,
            2000.0);
        config.people_flow.counting.rearm_frames = std::clamp(
            config.people_flow.counting.rearm_frames, 1, 300);
        config.people_flow.counting.min_crossing_interval_ms = std::clamp(
            config.people_flow.counting.min_crossing_interval_ms, 0, 60000);
        config.people_flow.counting.max_crossing_gap_ms = std::clamp(
            config.people_flow.counting.max_crossing_gap_ms, 50, 60000);
        config.people_flow.counting.transition_positive_to_negative = toLowerString(config.people_flow.counting.transition_positive_to_negative) == "out" ? "OUT" : "IN";
        const double line_dx = config.people_flow.counting.line_b_norm.x - config.people_flow.counting.line_a_norm.x;
        const double line_dy = config.people_flow.counting.line_b_norm.y - config.people_flow.counting.line_a_norm.y;
        if (line_dx * line_dx + line_dy * line_dy < 1e-8) {
            config.people_flow.config_error = "people_flow counting line endpoints must be different";
        }
        config.people_flow.visualization.event_marker_hold_frames = std::clamp(
            config.people_flow.visualization.event_marker_hold_frames, 1, 1000);
        config.people_flow.visualization.box_thickness = std::clamp(
            config.people_flow.visualization.box_thickness, 1, 12);
        config.people_flow.visualization.line_thickness = std::clamp(
            config.people_flow.visualization.line_thickness, 1, 12);
        config.people_flow.visualization.trail_thickness = std::clamp(
            config.people_flow.visualization.trail_thickness, 1, 12);
        config.people_flow.visualization.font_scale = std::clamp(
            config.people_flow.visualization.font_scale, 0.25, 3.0);
        config.people_flow.visualization.ui_scale = std::clamp(
            config.people_flow.visualization.ui_scale, 0.0, 4.0);
        config.people_flow.storage.writer_queue_capacity = std::clamp(config.people_flow.storage.writer_queue_capacity, 100, 1000000);
        config.people_flow.storage.writer_batch_size = std::clamp(config.people_flow.storage.writer_batch_size, 1, 10000);
        config.people_flow.storage.writer_flush_interval_ms = std::clamp(config.people_flow.storage.writer_flush_interval_ms, 50, 60000);
        config.people_flow.storage.events_max_len = std::clamp(config.people_flow.storage.events_max_len, 100, 1000000);
        config.people_flow.storage.event_retention_days = std::clamp(config.people_flow.storage.event_retention_days, 1, 3650);
        config.people_flow.storage.aggregate_retention_days = std::clamp(config.people_flow.storage.aggregate_retention_days, 1, 36500);

        auto redis = root["redis"];
        config.redis.enabled = readOrDefault<bool>(redis, "enabled", config.redis.enabled);
        config.redis.host = readOrDefault<std::string>(redis, "host", config.redis.host);
        config.redis.port = readOrDefault<int>(redis, "port", config.redis.port);
        config.redis.password = readOrDefault<std::string>(redis, "password", config.redis.password);
        config.redis.db = readOrDefault<int>(redis, "db", config.redis.db);
        config.redis.stream_key = readOrDefault<std::string>(redis, "stream_key", config.redis.stream_key);
        config.redis.consumer_group = readOrDefault<std::string>(redis, "consumer_group", config.redis.consumer_group);
        config.redis.consumer_name = readOrDefault<std::string>(redis, "consumer_name", config.redis.consumer_name);
        config.redis.block_ms = readOrDefault<int>(redis, "block_ms", config.redis.block_ms);
        config.redis.ttl_seconds = readOrDefault<int>(redis, "ttl_seconds", config.redis.ttl_seconds);
        config.redis.task_ttl_seconds = readOrDefault<int>(redis, "task_ttl_seconds", config.redis.task_ttl_seconds);
        config.redis.input_image_ttl_seconds = readOrDefault<int>(redis, "input_image_ttl_seconds", config.redis.input_image_ttl_seconds);
        config.redis.result_image_ttl_seconds = readOrDefault<int>(redis, "result_image_ttl_seconds", config.redis.result_image_ttl_seconds);
        config.redis.max_image_bytes = readOrDefault<long long>(redis, "max_image_bytes", config.redis.max_image_bytes);
        config.redis.max_result_image_bytes = readOrDefault<long long>(redis, "max_result_image_bytes", config.redis.max_result_image_bytes);
        config.redis.delete_input_after_done = readOrDefault<bool>(redis, "delete_input_after_done", config.redis.delete_input_after_done);
        config.redis.max_redis_used_memory_mb = readOrDefault<long long>(redis, "max_redis_used_memory_mb", config.redis.max_redis_used_memory_mb);
        config.redis.stream_max_len = readOrDefault<long long>(redis, "stream_max_len", config.redis.stream_max_len);
        config.redis.enable_pending_reclaim = readOrDefault<bool>(redis, "enable_pending_reclaim", config.redis.enable_pending_reclaim);
        config.redis.pending_min_idle_ms = readOrDefault<long long>(redis, "pending_min_idle_ms", config.redis.pending_min_idle_ms);

        auto metrics = root["metrics"];
        config.redis.metrics_enabled = readOrDefault<bool>(metrics, "enabled", config.redis.metrics_enabled);
        config.redis.metrics_recent_window_seconds = readOrDefault<int>(metrics, "recent_window_seconds", config.redis.metrics_recent_window_seconds);
        config.redis.metrics_ttl_seconds = readOrDefault<int>(metrics, "ttl_seconds", config.redis.metrics_ttl_seconds);

        if (config.redis.port <= 0) {
            config.redis.port = 6379;
        }
        if (config.redis.db < 0) {
            config.redis.db = 0;
        }
        if (config.redis.block_ms < 100) {
            config.redis.block_ms = 100;
        }
        if (config.redis.ttl_seconds < 60) {
            config.redis.ttl_seconds = 60;
        }
        if (config.redis.task_ttl_seconds <= 0) {
            config.redis.task_ttl_seconds = config.redis.ttl_seconds;
        }
        if (config.redis.task_ttl_seconds < 60) {
            config.redis.task_ttl_seconds = 60;
        }
        if (config.redis.input_image_ttl_seconds < 60) {
            config.redis.input_image_ttl_seconds = 60;
        }
        if (config.redis.result_image_ttl_seconds < 60) {
            config.redis.result_image_ttl_seconds = 60;
        }
        if (config.redis.max_image_bytes < 0) {
            config.redis.max_image_bytes = 0;
        }
        if (config.redis.max_result_image_bytes < 0) {
            config.redis.max_result_image_bytes = 0;
        }
        if (config.redis.max_redis_used_memory_mb < 0) {
            config.redis.max_redis_used_memory_mb = 0;
        }
        if (config.redis.stream_max_len < 0) {
            config.redis.stream_max_len = 0;
        }
        if (config.redis.pending_min_idle_ms < 1000) {
            config.redis.pending_min_idle_ms = 1000;
        }
        if (config.redis.metrics_recent_window_seconds < 1) {
            config.redis.metrics_recent_window_seconds = 60;
        }
        if (config.redis.metrics_ttl_seconds < 60) {
            config.redis.metrics_ttl_seconds = 60;
        }
        if (config.redis.stream_key.empty()) {
            config.redis.stream_key = "yolo:stream:detect";
        }
        if (config.redis.consumer_group.empty()) {
            config.redis.consumer_group = "yolo11_group";
        }
        if (config.redis.consumer_name.empty()) {
            config.redis.consumer_name = "worker_1";
        }

        auto logging = root["logging"];
        config.logging.enabled = readOrDefault<bool>(logging, "enabled", config.logging.enabled);
        config.logging.log_dir = readOrDefault<std::string>(logging, "log_dir", config.logging.log_dir);
        config.logging.level = readOrDefault<std::string>(logging, "level", config.logging.level);
        config.logging.console = readOrDefault<bool>(logging, "console", config.logging.console);
        config.logging.file = readOrDefault<bool>(logging, "file", config.logging.file);
        config.logging.flush_interval_sec = readOrDefault<int>(logging, "flush_interval_sec", config.logging.flush_interval_sec);
        config.logging.max_file_size_mb = readOrDefault<int>(logging, "max_file_size_mb", config.logging.max_file_size_mb);
        config.logging.max_files = readOrDefault<int>(logging, "max_files", config.logging.max_files);
        if (config.logging.log_dir.empty()) {
            config.logging.log_dir = "./logs";
        }
        if (config.logging.flush_interval_sec < 0) {
            config.logging.flush_interval_sec = 0;
        }
        if (config.logging.max_file_size_mb <= 0) {
            config.logging.max_file_size_mb = 50;
        }
        if (config.logging.max_files <= 0) {
            config.logging.max_files = 5;
        }

        auto worker = root["worker"];
        config.worker.enabled = readOrDefault<bool>(worker, "enabled", config.worker.enabled);
        config.worker.worker_num = readOrDefault<int>(worker, "worker_num", config.worker.worker_num);
        config.worker.min_alive_workers = readOrDefault<int>(worker, "min_alive_workers", config.worker.min_alive_workers);
        config.worker.consumer_name_prefix = readOrDefault<std::string>(
            worker,
            "consumer_name_prefix",
            config.worker.consumer_name_prefix
        );
        config.worker.log_task_done = readOrDefault<bool>(
            worker,
            "log_task_done",
            config.worker.log_task_done
        );
        config.worker.worker_group = readOrDefault<std::string>(
            worker,
            "worker_group",
            config.worker.worker_group
        );
        config.worker.worker_kind = readOrDefault<std::string>(
            worker,
            "worker_kind",
            config.worker.worker_kind
        );
        config.worker.task_kind = readOrDefault<std::string>(
            worker,
            "task_kind",
            config.worker.task_kind
        );
        config.worker.stream_type = readOrDefault<std::string>(
            worker,
            "stream_type",
            config.worker.stream_type
        );
        config.worker.max_concurrency = readOrDefault<int>(
            worker,
            "max_concurrency",
            config.worker.max_concurrency
        );
        config.worker.heartbeat_enabled = readOrDefault<bool>(
            worker,
            "heartbeat_enabled",
            config.worker.heartbeat_enabled
        );
        config.worker.heartbeat_interval_ms = readOrDefault<int>(
            worker,
            "heartbeat_interval_ms",
            config.worker.heartbeat_interval_ms
        );
        config.worker.heartbeat_ttl_seconds = readOrDefault<int>(
            worker,
            "heartbeat_ttl_seconds",
            config.worker.heartbeat_ttl_seconds
        );

        if (config.worker.worker_num <= 0) {
            config.worker.worker_num = 1;
        }
        if (config.worker.worker_num > 16) {
            std::cerr << "worker.worker_num is too large, clamp to 16." << std::endl;
            config.worker.worker_num = 16;
        }
        if (config.worker.min_alive_workers <= 0) {
            config.worker.min_alive_workers = 1;
        }
        if (config.worker.min_alive_workers > config.worker.worker_num) {
            config.worker.min_alive_workers = config.worker.worker_num;
        }
        if (config.worker.consumer_name_prefix.empty()) {
            config.worker.consumer_name_prefix = "worker_";
        }
        if (config.worker.heartbeat_interval_ms < 500) {
            config.worker.heartbeat_interval_ms = 500;
        }
        if (config.worker.heartbeat_ttl_seconds < 3) {
            config.worker.heartbeat_ttl_seconds = 3;
        }
        if (config.worker.max_concurrency <= 0) {
            config.worker.max_concurrency = 1;
        }


        // Phase 10.5: optional multi-model profile registry.
        // Example:
        // active_model: "obb"
        // models:
        //   obb:
        //     type: "obb"
        //     engine_path: "..."
        //     labels_path: "..."
        //     stream_key: "yolo:stream:obb"
        //     consumer_group: "yolo11_obb_group"
        //     consumer_name_prefix: "obb_worker_"
        auto models = root["models"];
        if (models && models.IsMap()) {
            for (const auto& entry : models) {
                const std::string name = entry.first.as<std::string>();
                const YAML::Node node = entry.second;
                ModelProfile profile;
                profile.name = name;
                profile.type = readOrDefault<std::string>(node, "type", name);
                profile.engine_path = readOrDefault<std::string>(node, "engine_path", config.model.engine_path);
                profile.labels_path = readOrDefault<std::string>(node, "labels_path", config.model.labels_path);
                profile.gpu_id = readOrDefault<int>(node, "gpu_id", config.model.gpu_id);
                profile.use_gpu_postprocess = readOrDefault<bool>(node, "use_gpu_postprocess", config.model.use_gpu_postprocess);
                profile.cls_topk = readOrDefault<int>(node, "cls_topk", config.model.cls_topk);
                profile.stream_key = readOrDefault<std::string>(node, "stream_key", config.redis.stream_key);
                profile.consumer_group = readOrDefault<std::string>(node, "consumer_group", config.redis.consumer_group);
                profile.consumer_name_prefix = readOrDefault<std::string>(node, "consumer_name_prefix", config.worker.consumer_name_prefix);
                profile.worker_num = readOrDefault<int>(node, "worker_num", config.worker.worker_num);
                profile.min_alive_workers = readOrDefault<int>(node, "min_alive_workers", config.worker.min_alive_workers);
                config.model_profiles[name] = profile;
            }
        }

        config.active_model = readOrDefault<std::string>(root, "active_model", config.active_model);
        if (!config.active_model.empty()) {
            const auto it = config.model_profiles.find(config.active_model);
            if (it == config.model_profiles.end()) {
                std::cerr << "active_model='" << config.active_model
                          << "' is not found in models. Use legacy model/redis/worker sections." << std::endl;
            }
            else {
                const ModelProfile& profile = it->second;
                config.model.type = profile.type;
                config.model.engine_path = profile.engine_path;
                config.model.labels_path = profile.labels_path;
                config.model.gpu_id = profile.gpu_id;
                config.model.use_gpu_postprocess = profile.use_gpu_postprocess;
                config.model.cls_topk = profile.cls_topk;
                config.redis.stream_key = profile.stream_key;
                config.redis.consumer_group = profile.consumer_group;
                config.worker.consumer_name_prefix = profile.consumer_name_prefix;
                config.worker.worker_num = profile.worker_num;
                config.worker.min_alive_workers = profile.min_alive_workers;

                // Keep consumer_name only as a default. yolo11_worker.exe normally overrides it by
                // --consumer-name. For convenience, derive one when the YAML did not set it clearly.
                if (config.redis.consumer_name.empty() || config.redis.consumer_name == "worker_1") {
                    config.redis.consumer_name = config.worker.consumer_name_prefix + "1";
                }
            }
        }

        // Re-validate fields that may have been overridden by an active model profile.
        if (config.worker.worker_num <= 0) {
            config.worker.worker_num = 1;
        }
        if (config.worker.worker_num > 16) {
            std::cerr << "worker.worker_num is too large, clamp to 16." << std::endl;
            config.worker.worker_num = 16;
        }
        if (config.worker.min_alive_workers <= 0) {
            config.worker.min_alive_workers = 1;
        }
        if (config.worker.min_alive_workers > config.worker.worker_num) {
            config.worker.min_alive_workers = config.worker.worker_num;
        }
        if (config.worker.consumer_name_prefix.empty()) {
            config.worker.consumer_name_prefix = "worker_";
        }
        if (config.redis.stream_key.empty()) {
            if (config.model.type == "obb") {
                config.redis.stream_key = "yolo:stream:obb";
            }
            else if (config.model.type == "cls") {
                config.redis.stream_key = "yolo:stream:cls";
            }
            else if (config.model.type == "pose") {
                config.redis.stream_key = "yolo:stream:pose";
            }
            else if (config.model.type == "seg") {
                config.redis.stream_key = "yolo:stream:seg";
            }
            else {
                config.redis.stream_key = "yolo:stream:detect";
            }
        }
        if (config.redis.consumer_group.empty()) {
            if (config.model.type == "obb") {
                config.redis.consumer_group = "yolo11_obb_group";
            }
            else if (config.model.type == "cls") {
                config.redis.consumer_group = "yolo11_cls_group";
            }
            else if (config.model.type == "pose") {
                config.redis.consumer_group = "yolo11_pose_group";
            }
            else if (config.model.type == "seg") {
                config.redis.consumer_group = "yolo11_seg_group";
            }
            else {
                config.redis.consumer_group = "yolo11_group";
            }
        }

        normalizeWorkerCapability(config);
        return config;
    }

}  // namespace yolo11_server
