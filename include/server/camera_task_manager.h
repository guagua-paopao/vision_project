#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yolo11_server {

enum class CameraTaskCommandKind {
    start,
    stop
};

struct CameraTaskCommand {
    CameraTaskCommandKind kind = CameraTaskCommandKind::start;
    std::string message_id;
    std::string task_id;
    std::string run_id;
    std::string camera_profile;
    int definition_version = 0;
    int frame_interval_ms = 1000;
    std::string output_mode = "latest";
    int jpeg_quality = 90;
    int max_width = 0;
    int max_height = 0;
    int retention_days = 7;
    int max_saved_frames = 100000;
    bool analysis_enabled = false;
    double target_infer_fps = 5.0;
    std::string algorithm_profile;
    std::vector<std::string> algorithms;
    std::string callback_profile;
    long long create_time_ms = 0;
    // command_version >= 3 additive RunSpec fields. Defaults preserve
    // deserialization of command_version 1/2 messages.
    std::string origin = "camera_api";
    std::string analysis_config_version;
    long long initial_occupancy = 0;
    int snapshot_fps = 0;
    std::string algorithm_parameters_json = "{}";
    std::string legacy_session_id;
    bool preserve_pf_projection = false;
    int legacy_response_version = 0;
};

class ICameraTaskCommandSource {
public:
    virtual ~ICameraTaskCommandSource() = default;
    virtual bool start(std::string& error) = 0;
    virtual bool poll(CameraTaskCommand& command, std::string& error) = 0;
    virtual bool acknowledge(const std::string& message_id, std::string& error) = 0;
    virtual void interrupt() noexcept = 0;
};

class ICameraTaskSession {
public:
    virtual ~ICameraTaskSession() = default;
    virtual void run() noexcept = 0;
    virtual void requestStop() noexcept = 0;
};

using CameraTaskSessionFactory = std::function<std::shared_ptr<ICameraTaskSession>(
    const CameraTaskCommand& command,
    std::string& error)>;
using CameraTaskCommandFailureCallback = std::function<void(
    const CameraTaskCommand& command,
    const std::string& error_code,
    const std::string& message)>;

struct CameraPipelineThreadSnapshot {
    std::string camera_id;
    std::string run_id;
    long long thread_started_at_ms = 0;
    bool completed = false;
};

class CameraTaskManager final {
public:
    CameraTaskManager(
        int max_active_runs,
        std::unique_ptr<ICameraTaskCommandSource> command_source,
        CameraTaskSessionFactory session_factory,
        CameraTaskCommandFailureCallback failure_callback = {},
        std::vector<CameraTaskCommand> startup_commands = {}
    );
    ~CameraTaskManager() noexcept;

    CameraTaskManager(const CameraTaskManager&) = delete;
    CameraTaskManager& operator=(const CameraTaskManager&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool running() const;
    std::vector<std::string> activeRunIds() const;
    std::vector<CameraPipelineThreadSnapshot> activePipelines() const;
    std::size_t activePipelineCount() const;

private:
    struct PipelineControl {
        std::string task_id;
        std::string run_id;
        long long thread_started_at_ms = 0;
        std::shared_ptr<ICameraTaskSession> session;
        std::thread thread;
        std::atomic<bool> completed{ false };
    };

    void loop() noexcept;
    void process(const CameraTaskCommand& command) noexcept;
    void reapCompleted() noexcept;
    void acknowledgeNoexcept(const CameraTaskCommand& command) noexcept;
    void failNoexcept(
        const CameraTaskCommand& command,
        const std::string& error_code,
        const std::string& message
    ) noexcept;

    int max_active_runs_ = 1;
    std::unique_ptr<ICameraTaskCommandSource> command_source_;
    CameraTaskSessionFactory session_factory_;
    CameraTaskCommandFailureCallback failure_callback_;
    std::vector<CameraTaskCommand> startup_commands_;
    std::atomic<bool> running_{ false };
    std::thread consumer_thread_;
    mutable std::mutex pipelines_mutex_;
    // Stable camera/task id is the ownership key. run_id changes every time
    // a CameraPipeline is restarted and is retained only for audit.
    std::map<std::string, std::shared_ptr<PipelineControl>> pipelines_;
};

}  // namespace yolo11_server
