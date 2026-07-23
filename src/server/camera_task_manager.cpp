#include "server/camera_task_manager.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

namespace yolo11_server {

namespace {
long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

CameraTaskManager::CameraTaskManager(
    int max_active_runs,
    std::unique_ptr<ICameraTaskCommandSource> command_source,
    CameraTaskSessionFactory session_factory,
    CameraTaskCommandFailureCallback failure_callback
) : max_active_runs_(std::max(1, max_active_runs)),
    command_source_(std::move(command_source)),
    session_factory_(std::move(session_factory)),
    failure_callback_(std::move(failure_callback)) {
}

CameraTaskManager::~CameraTaskManager() noexcept {
    stop();
}

bool CameraTaskManager::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (!command_source_ || !session_factory_) {
        error = "camera task manager dependencies are unavailable";
        return false;
    }
    if (!command_source_->start(error)) return false;
    running_.store(true);
    try {
        consumer_thread_ = std::thread([this]() { loop(); });
    }
    catch (const std::exception& e) {
        running_.store(false);
        command_source_->interrupt();
        error = std::string("failed to create camera task consumer thread: ") + e.what();
        return false;
    }
    return true;
}

void CameraTaskManager::stop() noexcept {
    if (!running_.exchange(false)) return;
    try {
        if (command_source_) command_source_->interrupt();
        if (consumer_thread_.joinable()) consumer_thread_.join();

        std::vector<std::shared_ptr<PipelineControl>> controls;
        {
            std::lock_guard<std::mutex> lock(pipelines_mutex_);
            for (const auto& entry : pipelines_) controls.push_back(entry.second);
        }
        for (const auto& control : controls) {
            if (control->session) control->session->requestStop();
        }
        for (const auto& control : controls) {
            if (control->thread.joinable()) control->thread.join();
        }
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        pipelines_.clear();
    }
    catch (...) {
        spdlog::error("Camera task manager stop exception ignored");
    }
}

bool CameraTaskManager::running() const {
    return running_.load();
}

std::vector<std::string> CameraTaskManager::activeRunIds() const {
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (const auto& entry : pipelines_) {
        if (!entry.second->completed.load()) result.push_back(entry.second->run_id);
    }
    return result;
}

std::vector<CameraPipelineThreadSnapshot> CameraTaskManager::activePipelines() const {
    std::vector<CameraPipelineThreadSnapshot> result;
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (const auto& entry : pipelines_) {
        if (entry.second->completed.load()) continue;
        result.push_back({
            entry.second->task_id,
            entry.second->run_id,
            entry.second->thread_started_at_ms,
            false
        });
    }
    return result;
}

std::size_t CameraTaskManager::activePipelineCount() const {
    return activePipelines().size();
}

void CameraTaskManager::loop() noexcept {
    while (running_.load()) {
        reapCompleted();
        CameraTaskCommand command;
        std::string error;
        bool received = false;
        try {
            received = command_source_->poll(command, error);
        }
        catch (const std::exception& e) {
            error = e.what();
        }
        catch (...) {
            error = "unknown command source error";
        }
        if (!received) {
            if (!error.empty() && running_.load()) {
                spdlog::warn("Camera task command poll failed: {}", error);
            }
            continue;
        }
        process(command);
    }
    reapCompleted();
}

void CameraTaskManager::process(const CameraTaskCommand& command) noexcept {
    if (command.task_id.empty() || command.run_id.empty()) {
        failNoexcept(command, "INVALID_CAMERA_COMMAND", "camera command has an empty camera_id or run_id");
        acknowledgeNoexcept(command);
        return;
    }

    if (command.kind == CameraTaskCommandKind::stop) {
        std::shared_ptr<ICameraTaskSession> session;
        {
            std::lock_guard<std::mutex> lock(pipelines_mutex_);
            const auto found = pipelines_.find(command.task_id);
            if (found != pipelines_.end() &&
                (command.run_id.empty() || found->second->run_id == command.run_id)) {
                session = found->second->session;
            }
        }
        // A delayed STOP for an older generation must never stop the current
        // CameraPipeline for the same stable camera_id.
        if (session) session->requestStop();
        acknowledgeNoexcept(command);
        return;
    }

    bool duplicate_active = false;
    std::shared_ptr<PipelineControl> replaced;
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        const auto duplicate = pipelines_.find(command.task_id);
        if (duplicate != pipelines_.end() && !duplicate->second->completed.load()) {
            if (duplicate->second->run_id == command.run_id) duplicate_active = true;
            else replaced = duplicate->second;
        }
    }
    if (duplicate_active) {
        acknowledgeNoexcept(command);
        return;
    }
    // A new generation for the same stable camera id replaces its old
    // extraction object. Joining here guarantees that only one extraction
    // thread per camera id exists and that the old Hub subscription/lease is
    // released before the new session factory runs.
    if (replaced) {
        if (replaced->session) replaced->session->requestStop();
        if (replaced->thread.joinable()) replaced->thread.join();
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        const auto found = pipelines_.find(command.task_id);
        if (found != pipelines_.end() && found->second == replaced) pipelines_.erase(found);
    }
    bool capacity_exceeded = false;
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        const auto active_count = std::count_if(
            pipelines_.begin(), pipelines_.end(),
            [](const auto& entry) { return !entry.second->completed.load(); });
        capacity_exceeded = active_count >= max_active_runs_;
    }
    if (capacity_exceeded) {
        failNoexcept(command, "CAMERA_RUN_CAPACITY_EXCEEDED", "camera run capacity exceeded");
        return;
    }

    std::string factory_error;
    std::shared_ptr<ICameraTaskSession> session;
    try {
        session = session_factory_(command, factory_error);
    }
    catch (const std::exception& e) {
        factory_error = e.what();
    }
    catch (...) {
        factory_error = "unknown camera extraction session factory error";
    }
    if (!session) {
        failNoexcept(command, "CAMERA_SESSION_CREATE_FAILED",
            factory_error.empty() ? "camera extraction session creation failed" : factory_error);
        acknowledgeNoexcept(command);
        return;
    }

    auto control = std::make_shared<PipelineControl>();
    control->task_id = command.task_id;
    control->run_id = command.run_id;
    control->thread_started_at_ms = wallNowMs();
    control->session = std::move(session);
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        pipelines_[command.task_id] = control;
    }
    try {
        control->thread = std::thread([control]() {
            control->session->run();
            control->completed.store(true);
        });
    }
    catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(pipelines_mutex_);
            pipelines_.erase(command.task_id);
        }
        failNoexcept(command, "CAMERA_SESSION_THREAD_FAILED", e.what());
    }
    acknowledgeNoexcept(command);
}

void CameraTaskManager::reapCompleted() noexcept {
    std::vector<std::shared_ptr<PipelineControl>> completed;
    {
        std::lock_guard<std::mutex> lock(pipelines_mutex_);
        for (auto it = pipelines_.begin(); it != pipelines_.end();) {
            if (it->second->completed.load()) {
                completed.push_back(it->second);
                it = pipelines_.erase(it);
            }
            else {
                ++it;
            }
        }
    }
    for (const auto& control : completed) {
        if (control->thread.joinable()) control->thread.join();
    }
}

void CameraTaskManager::acknowledgeNoexcept(const CameraTaskCommand& command) noexcept {
    if (command.message_id.empty()) return;
    try {
        std::string error;
        if (!command_source_->acknowledge(command.message_id, error) && !error.empty()) {
            spdlog::warn("Camera task command acknowledge failed: message_id={}, error={}",
                command.message_id, error);
        }
    }
    catch (...) {
    }
}

void CameraTaskManager::failNoexcept(
    const CameraTaskCommand& command,
    const std::string& error_code,
    const std::string& message
) noexcept {
    try {
        if (failure_callback_) failure_callback_(command, error_code, message);
    }
    catch (...) {
    }
}

}  // namespace yolo11_server
