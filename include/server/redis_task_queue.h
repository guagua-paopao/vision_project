#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "server/algorithm_runtime_snapshot.h"

#include "business/people_flow_types.h"
#include "server/app_config.h"

struct redisContext;

namespace yolo11_server {

    struct RedisTask {
        std::string stream_id;
        std::string task_id;
        std::string task_kind = "image";
        std::string model_type = "detect";

        // Production path: image bytes are stored in Redis, so server/worker can be separated.
        std::string input_image_key;
        std::string result_image_key;

        // Backward-compatible fallback for all-in-one/local-file mode.
        std::string input_image_path;

        // Phase 13 video-file task fields. Videos stay on local disk, not in Redis binary.
        std::string input_video_path;
        std::string output_video_path;
        std::string output_video_filename;
        int video_width = 0;
        int video_height = 0;
        double video_fps = 0.0;
        long long video_total_frames = 0;
        long long video_duration_ms = 0;

        // Phase 14 live stream task fields. stream_id is the user-facing stream task id.
        // RedisTask::stream_id remains the Redis Stream message id for XACK.
        std::string stream_task_id;
        std::string source_type;
        // Phase 19.2 secure transport fields. source_ref contains only an
        // environment/file/camera reference; masked_uri is safe for display.
        std::string source_ref;
        std::string masked_uri;
        std::string camera_profile;

        // Read-only migration fallback for tasks queued by Phase 18.5. New
        // tasks never write this value back to Redis.
        std::string legacy_source_uri;
        int camera_id = 0;
        std::string snapshot_path;
        int snapshot_interval_frames = 5;
        int target_fps = 10;

        // Phase 21 people-flow command fields.
        std::string people_flow_session_id;
        std::string people_flow_camera_id;
        std::string people_flow_config_version;
        long long initial_occupancy = 0;

        long long create_time_ms = 0;
    };

    struct RedisTaskStatus {
        bool found = false;
        std::string task_id;
        std::string status;
        std::string task_kind;
        std::string model_type;
        std::string error;
        std::string result_json_text;
        std::string input_image_key;
        std::string result_image_key;
        std::string result_image_path;
        std::string result_image_filename;
        std::string input_video_path;
        std::string output_video_path;
        std::string output_video_filename;
        std::string worker_id;
        std::string consumer_name;
        long long create_time_ms = 0;
        long long start_time_ms = 0;
        long long finish_time_ms = 0;
        long long queue_wait_ms = 0;
        double infer_ms = 0.0;
        long long total_ms = 0;

        // Phase 13 video progress fields.
        long long total_frames = 0;
        long long processed_frames = 0;
        long long current_frame_index = 0;
        double progress = 0.0;
        double fps = 0.0;
        int video_width = 0;
        int video_height = 0;
        long long duration_ms = 0;
        long long process_ms = 0;
        bool cancel_requested = false;
    };

    struct RedisStreamStats {
        long long stream_len = -1;
        long long pending = -1;
    };

    struct RedisMemoryStats {
        bool found = false;
        long long used_memory_bytes = 0;
        double used_memory_mb = 0.0;
        long long maxmemory_bytes = 0;
        double maxmemory_mb = 0.0;
        std::string used_memory_human;
        std::string maxmemory_human;
    };


    struct RedisRuntimeMetrics {
        bool found = false;
        long long done_count = 0;
        long long failed_count = 0;
        long long total_count = 0;
        long long recent_done_count = 0;
        int recent_window_seconds = 60;
        double qps_recent = 0.0;
        double avg_queue_wait_ms = 0.0;
        double avg_inference_ms = 0.0;
        double avg_total_ms = 0.0;
        long long last_finish_time_ms = 0;
        std::string last_task_id;
        std::map<std::string, long long> worker_done_count;
        std::map<std::string, long long> worker_failed_count;
    };

    struct WorkerHeartbeatRecord {
        bool found = false;
        bool alive = false;
        std::string consumer_name;
        std::string heartbeat_key;
        std::string pid;
        std::string host;
        int worker_id = 0;
        int gpu_id = 0;

        // Phase 15 capability fields.
        // model_type is the public service profile type: detect / obb / video / stream.
        // runner_model_type records the actual detector runner underneath, such as detect.
        std::string model_type;
        std::string runner_model_type;
        std::string worker_group;
        std::string worker_kind;
        std::string task_kind;
        std::string stream_type;
        std::string runtime_mode;
        std::string worker_generation;
        bool legacy_people_flow_role = false;
        bool camera_task_manager_running = false;
        bool hub_registry_ready = false;
        bool coordination_healthy = false;
        std::string engine_path;
        std::string labels_path;
        int max_concurrency = 1;

        std::string status;
        std::string current_task_id;
        long long processed_count = 0;
        long long failed_count = 0;
        long long start_time_ms = 0;
        long long last_heartbeat_ms = 0;
        long long last_heartbeat_age_ms = -1;
        std::string last_error;
        AlgorithmRuntimeSnapshot algorithm_runtime;
    };

    struct StreamStartRequest {
        std::string stream_id;
        std::string source_type = "camera";
        std::string source_ref;
        std::string masked_uri;
        std::string camera_profile;
        int camera_id = 0;
        std::string snapshot_path;
        int snapshot_interval_frames = 5;
        int target_fps = 10;
        std::string model_type = "detect";
        long long create_time_ms = 0;
    };

    struct StreamTaskStatus {
        bool found = false;
        std::string stream_id;
        std::string status;
        std::string model_type;
        std::string source_type;
        std::string source_ref;
        std::string masked_uri;
        std::string camera_profile;
        int camera_id = 0;
        std::string snapshot_path;
        std::string latest_snapshot_path;
        std::string error;
        std::string last_error;
        std::string consumer_name;
        int worker_id = 0;
        bool stop_requested = false;
        long long create_time_ms = 0;
        long long start_time_ms = 0;
        long long stop_time_ms = 0;
        long long last_update_ms = 0;
        long long frame_count = 0;
        double fps = 0.0;
        int width = 0;
        int height = 0;
        int last_num_detections = 0;
        int reconnect_count = 0;
        int no_frame_count = 0;

        // Phase 19.3 RTSP capture observability.
        std::string capture_state;
        std::string capture_backend;
        bool backend_fallback = false;
        double capture_fps = 0.0;
        double infer_fps = 0.0;
        double source_fps = 0.0;
        long long latest_frame_age_ms = -1;
        long long dropped_frames = 0;
        long long last_frame_time_ms = 0;
        bool resolution_changed = false;
        long long resolution_change_count = 0;
    };

    struct StreamCaptureMetrics {
        std::string capture_state;
        std::string capture_backend;
        bool backend_fallback = false;
        double capture_fps = 0.0;
        double infer_fps = 0.0;
        double source_fps = 0.0;
        long long latest_frame_age_ms = -1;
        long long dropped_frames = 0;
        int reconnect_count = 0;
        long long last_frame_time_ms = 0;
        int width = 0;
        int height = 0;
        bool resolution_changed = false;
        long long resolution_change_count = 0;
        long long update_time_ms = 0;
    };

    struct PeopleFlowStartRequest {
        std::string session_id;
        std::string camera_id;
        std::string camera_profile;
        std::string source_ref;
        std::string masked_uri;
        std::string config_version;
        std::string snapshot_path;
        long long initial_occupancy = 0;
        long long create_time_ms = 0;
        int active_ttl_seconds = 60;
        int session_ttl_seconds = 604800;
    };

    struct PeopleFlowSessionStatus {
        bool found = false;
        std::string session_id;
        std::string camera_id;
        std::string camera_profile;
        std::string masked_uri;
        std::string config_version;
        std::string status;
        std::string snapshot_path;
        std::string capture_state;
        std::string capture_backend;
        bool shared_hub = false;
        std::string hub_instance_id;
        int hub_subscribers = 0;
        std::string consumer_name;
        std::string error;
        std::string last_error;
        int worker_id = 0;
        bool stop_requested = false;
        bool storage_degraded = false;
        bool snapshot_degraded = false;
        bool resolution_changed = false;
        long long create_time_ms = 0;
        long long start_time_ms = 0;
        long long stop_time_ms = 0;
        long long last_update_ms = 0;
        long long last_frame_time_ms = 0;
        long long latest_frame_age_ms = -1;
        long long frame_count = 0;
        long long dropped_frames = 0;
        long long event_queue_depth = 0;
        long long in_count = 0;
        long long out_count = 0;
        long long initial_occupancy = 0;
        long long occupancy = 0;
        long long applied_calibration_version = 0;
        int live_persons = 0;
        int reconnect_count = 0;
        int width = 0;
        int height = 0;
        double capture_fps = 0.0;
        double infer_fps = 0.0;
        double source_fps = 0.0;
        double last_inference_ms = 0.0;
    };

    class RedisTaskQueue {
    public:
        explicit RedisTaskQueue(const RedisSection& config);
        ~RedisTaskQueue();

        RedisTaskQueue(const RedisTaskQueue&) = delete;
        RedisTaskQueue& operator=(const RedisTaskQueue&) = delete;

        bool connect(std::string& error) const;
        void disconnect() const;
        bool ping(std::string& error) const;

        bool submitTask(const RedisTask& task, std::string& error) const;

        bool markRunning(
            const std::string& task_id,
            long long start_time_ms,
            int worker_id,
            const std::string& consumer_name,
            std::string& error
        ) const;

        bool markDone(
            const std::string& task_id,
            const std::string& result_json_text,
            const std::string& result_image_key,
            const std::string& result_image_path,
            const std::string& result_image_filename,
            long long finish_time_ms,
            long long queue_wait_ms,
            double infer_ms,
            long long total_ms,
            int worker_id,
            const std::string& consumer_name,
            std::string& error
        ) const;

        bool markFailed(
            const std::string& task_id,
            const std::string& error_message,
            long long finish_time_ms,
            long long queue_wait_ms,
            long long total_ms,
            int worker_id,
            const std::string& consumer_name,
            std::string& error
        ) const;

        bool updateVideoProgress(
            const std::string& task_id,
            long long processed_frames,
            long long total_frames,
            long long current_frame_index,
            double progress,
            double fps,
            int width,
            int height,
            long long process_ms,
            std::string& error
        ) const;

        bool markVideoDone(
            const std::string& task_id,
            const std::string& result_json_text,
            const std::string& output_video_path,
            const std::string& output_video_filename,
            long long finish_time_ms,
            long long queue_wait_ms,
            long long process_ms,
            long long total_ms,
            long long processed_frames,
            long long total_frames,
            double fps,
            int width,
            int height,
            long long duration_ms,
            int worker_id,
            const std::string& consumer_name,
            std::string& error
        ) const;

        bool markVideoCanceled(
            const std::string& task_id,
            const std::string& result_json_text,
            long long finish_time_ms,
            long long queue_wait_ms,
            long long process_ms,
            long long total_ms,
            long long processed_frames,
            long long total_frames,
            int worker_id,
            const std::string& consumer_name,
            std::string& error
        ) const;

        bool requestCancelTask(const std::string& task_id, std::string& error) const;
        bool isCancelRequested(const std::string& task_id, bool& cancel_requested, std::string& error) const;

        // Phase 14 live stream task helpers.
        bool submitStreamTask(const StreamStartRequest& request, std::string& error) const;
        bool markStreamStarting(const std::string& stream_id, long long start_time_ms, int worker_id, const std::string& consumer_name, std::string& error) const;
        bool markStreamRunning(const std::string& stream_id, int width, int height, double fps, long long start_time_ms, int worker_id, const std::string& consumer_name, std::string& error) const;
        bool markStreamReconnecting(const std::string& stream_id, const std::string& reason, int reconnect_count, int no_frame_count, long long update_time_ms, std::string& error) const;
        bool updateStreamCaptureMetrics(const std::string& stream_id, const StreamCaptureMetrics& metrics, std::string& error) const;
        bool updateStreamLatest(const std::string& stream_id, const std::string& latest_snapshot_path, long long frame_count, double fps, int width, int height, int last_num_detections, long long update_time_ms, std::string& error) const;
        bool refreshStreamLease(const std::string& stream_id, std::string& error) const;
        bool markStreamStopped(const std::string& stream_id, long long stop_time_ms, long long frame_count, std::string& error) const;
        bool markStreamFailed(const std::string& stream_id, const std::string& error_message, long long stop_time_ms, long long frame_count, std::string& error) const;
        bool requestStopStreamTask(const std::string& stream_id, std::string& error) const;
        bool isStreamStopRequested(const std::string& stream_id, bool& stop_requested, std::string& error) const;
        bool getStreamTaskStatus(const std::string& stream_id, StreamTaskStatus& status, std::string& error) const;

        // Phase 14.0 Hotfix: guard single-live-stream mode.
        // The active stream key prevents repeated /stream/start calls from queuing
        // several long-running camera/RTSP tasks for one stream worker.
        bool getActiveStreamTask(StreamTaskStatus& status, std::string& error) const;

        // Phase 21 independent people-flow lifecycle and hot-state methods.
        bool submitPeopleFlowTask(const PeopleFlowStartRequest& request, std::string& error) const;
        bool updatePeopleFlowSession(const PeopleFlowSessionStatus& status, int session_ttl_seconds, int realtime_ttl_seconds, std::string& error) const;
        bool getPeopleFlowSession(const std::string& session_id, PeopleFlowSessionStatus& status, std::string& error) const;
        bool getPeopleFlowRealtime(const std::string& camera_id, PeopleFlowSessionStatus& status, std::string& error) const;
        bool getActivePeopleFlowSession(const std::string& camera_id, std::string& session_id, std::string& error) const;
        bool getLastPeopleFlowSession(const std::string& camera_id, std::string& session_id, std::string& error) const;
        bool refreshPeopleFlowLease(const std::string& camera_id, const std::string& session_id, int ttl_seconds, std::string& error) const;
        bool releasePeopleFlowLease(const std::string& camera_id, const std::string& session_id, std::string& error) const;
        bool requestStopPeopleFlow(const std::string& session_id, std::string& error) const;
        bool isPeopleFlowStopRequested(const std::string& session_id, bool& stop_requested, std::string& error) const;
        bool appendPeopleFlowEvent(const CrossingEvent& event, int max_len, std::string& error) const;
        bool requestPeopleFlowCalibration(
            const std::string& camera_id,
            const std::string& session_id,
            long long occupancy,
            long long& calibration_version,
            std::string& error
        ) const;
        bool getPeopleFlowCalibrationRequest(
            const std::string& camera_id,
            long long& calibration_version,
            long long& occupancy,
            std::string& error
        ) const;

        bool getTaskStatus(const std::string& task_id, RedisTaskStatus& status, std::string& error) const;

        bool popTask(RedisTask& task, std::string& error) const;
        bool claimPendingTask(RedisTask& task, std::string& error) const;
        bool ackTask(const std::string& stream_id, std::string& error) const;

        bool getStreamStats(RedisStreamStats& stats, std::string& error) const;
        bool getRedisMemoryStats(RedisMemoryStats& stats, std::string& error) const;
        bool getRuntimeMetrics(RedisRuntimeMetrics& metrics, std::string& error) const;

        // Binary value helpers used for image bytes.
        bool setBinaryValue(const std::string& key, const std::string& value, std::string& error) const;
        bool setBinaryValueWithTtl(const std::string& key, const std::string& value, int ttl_seconds, std::string& error) const;
        bool getBinaryValue(const std::string& key, std::string& value, std::string& error) const;
        bool deleteKey(const std::string& key, std::string& error) const;

        // Worker heartbeat helpers.
        bool writeWorkerHeartbeat(const WorkerHeartbeatRecord& heartbeat, int ttl_seconds, std::string& error) const;
        bool getWorkerHeartbeats(
            const std::string& consumer_name_prefix,
            int expected_worker_num,
            std::vector<WorkerHeartbeatRecord>& workers,
            std::string& error
        ) const;

        std::string inputImageKey(const std::string& task_id) const;
        std::string resultImageKey(const std::string& task_id) const;
        std::string workerHeartbeatKey(const std::string& consumer_name) const;
        std::string metricsGlobalKey() const;
        std::string metricsWorkerDoneKey() const;
        std::string metricsWorkerFailedKey() const;
        std::string metricsRecentDoneKey() const;

    private:
        std::string statusKey(const std::string& task_id) const;
        std::string resultKey(const std::string& task_id) const;
        std::string metaKey(const std::string& task_id) const;
        std::string streamTaskStatusKey(const std::string& stream_id) const;
        std::string streamTaskMetaKey(const std::string& stream_id) const;
        std::string streamTaskLatestKey(const std::string& stream_id) const;
        std::string activeStreamTaskKey() const;
        std::string peopleFlowSessionKey(const std::string& session_id) const;
        std::string peopleFlowActiveKey(const std::string& camera_id) const;
        std::string peopleFlowLastKey(const std::string& camera_id) const;
        std::string peopleFlowRealtimeKey(const std::string& camera_id) const;
        std::string peopleFlowEventsKey(const std::string& camera_id) const;

    private:
        RedisSection config_;

        // One hiredis connection per RedisTaskQueue instance.
        // Protected because HttpController can be called by multiple Crow threads.
        mutable std::mutex context_mutex_;
        mutable redisContext* context_ = nullptr;
    };

}  // namespace yolo11_server
