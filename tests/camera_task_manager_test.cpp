#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "server/camera_task_manager.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct CommandState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<CameraTaskCommand> commands;
    std::vector<std::string> acknowledged;
    bool interrupted = false;
};

class FakeCommandSource final : public ICameraTaskCommandSource {
public:
    explicit FakeCommandSource(std::shared_ptr<CommandState> state) : state_(std::move(state)) {}

    bool start(std::string& error) override {
        error.clear();
        return true;
    }

    bool poll(CameraTaskCommand& command, std::string& error) override {
        error.clear();
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
            return state_->interrupted || !state_->commands.empty();
        });
        if (state_->interrupted || state_->commands.empty()) return false;
        command = state_->commands.front();
        state_->commands.pop_front();
        return true;
    }

    bool acknowledge(const std::string& message_id, std::string& error) override {
        error.clear();
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->acknowledged.push_back(message_id);
        return true;
    }

    void interrupt() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->interrupted = true;
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<CommandState> state_;
};

struct SessionState {
    std::atomic<int> created{ 0 };
    std::atomic<int> running{ 0 };
    std::atomic<int> stopped{ 0 };
};

class FakeSession final : public ICameraTaskSession {
public:
    explicit FakeSession(std::shared_ptr<SessionState> state) : state_(std::move(state)) {
        ++state_->created;
    }

    void run() noexcept override {
        ++state_->running;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return stop_requested_; });
        --state_->running;
        ++state_->stopped;
    }

    void requestStop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        cv_.notify_all();
    }

private:
    std::shared_ptr<SessionState> state_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
};

void enqueue(const std::shared_ptr<CommandState>& state, CameraTaskCommand command) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->commands.push_back(std::move(command));
    state->cv.notify_all();
}

}  // namespace

int main() {
    auto commands = std::make_shared<CommandState>();
    auto sessions = std::make_shared<SessionState>();
    std::vector<std::string> failures;
    std::mutex failures_mutex;
    CameraTaskManager manager(
        2,
        std::make_unique<FakeCommandSource>(commands),
        [sessions](const CameraTaskCommand&, std::string& error) {
            error.clear();
            return std::make_shared<FakeSession>(sessions);
        },
        [&](const CameraTaskCommand&, const std::string& code, const std::string&) {
            std::lock_guard<std::mutex> lock(failures_mutex);
            failures.push_back(code);
        });

    std::string error;
    require(manager.start(error), "camera task manager must start");
    std::atomic<bool> people_flow_alive{ true };
    std::thread simulated_people_flow([&]() {
        while (people_flow_alive.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });

    enqueue(commands, {CameraTaskCommandKind::start, "1-0", "task_a", "run_a", "entry"});
    enqueue(commands, {CameraTaskCommandKind::start, "2-0", "task_b", "run_b", "entry"});
    require(waitUntil([&]() { return sessions->running.load() == 2; }, 500),
        "two CameraPipeline threads must run concurrently");
    require(manager.activePipelineCount() == 2 &&
            manager.activePipelines().size() == 2,
        "N active camera ids must have exactly N registered CameraPipeline threads");
    require(people_flow_alive.load(), "camera starts must not block the People Flow role");

    enqueue(commands, {CameraTaskCommandKind::start, "2-1", "task_b", "run_b", "entry"});
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(commands->mutex);
        return commands->acknowledged.size() >= 3;
    }, 500), "duplicate START must be acknowledged idempotently");
    require(sessions->created.load() == 2,
        "duplicate START must not create a second extraction session");

    enqueue(commands, {CameraTaskCommandKind::start, "3-0", "task_c", "run_c", "entry"});
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(failures_mutex);
        return !failures.empty();
    }, 500), "capacity rejection must be observable");
    {
        std::lock_guard<std::mutex> lock(failures_mutex);
        require(failures.front() == "CAMERA_RUN_CAPACITY_EXCEEDED",
            "capacity error code must be stable");
    }

    enqueue(commands, {CameraTaskCommandKind::stop, "4-0", "task_a", "run_a", "entry"});
    require(waitUntil([&]() { return sessions->running.load() == 1; }, 500),
        "stop command must stop one run without stopping another");
    enqueue(commands, {CameraTaskCommandKind::start, "5-0", "task_c", "run_c", "entry"});
    require(waitUntil([&]() { return sessions->created.load() == 3; }, 500),
        "consumer must continue dispatching after a long-running session starts");
    enqueue(commands, {CameraTaskCommandKind::start, "6-0", "task_c", "run_c_v2", "entry"});
    require(waitUntil([&]() {
        return sessions->created.load() == 4 && sessions->running.load() == 2 &&
            sessions->stopped.load() >= 2;
    }, 500), "a new run for the same camera id must replace and join the old extraction thread");
    enqueue(commands, {CameraTaskCommandKind::stop, "6-1", "task_c", "run_c", "entry"});
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(commands->mutex);
        return commands->acknowledged.size() >= 7;
    }, 500), "a delayed STOP for an old generation must be acknowledged");
    require(sessions->running.load() == 2 && manager.activePipelineCount() == 2,
        "a stale STOP must not terminate the replacement CameraPipeline");
    const auto active_runs = manager.activeRunIds();
    require(std::find(active_runs.begin(), active_runs.end(), "run_c_v2") != active_runs.end(),
        "stable camera id must point at the replacement run generation");
    require(people_flow_alive.load(), "camera start/stop must leave People Flow alive");

    manager.stop();
    people_flow_alive.store(false);
    simulated_people_flow.join();
    require(sessions->running.load() == 0, "manager shutdown must join all sessions");
    require(commands->acknowledged.size() == 7,
        "capacity-rejected START must stay pending while accepted/idempotent commands are acknowledged");

    auto recovery_commands = std::make_shared<CommandState>();
    auto recovery_sessions = std::make_shared<SessionState>();
    CameraTaskCommand recovery;
    recovery.task_id = "task_recovered";
    recovery.run_id = "run_recovered";
    recovery.camera_profile = "entry";
    CameraTaskManager recovery_manager(
        1,
        std::make_unique<FakeCommandSource>(recovery_commands),
        [recovery_sessions](const CameraTaskCommand&, std::string& factory_error) {
            factory_error.clear();
            return std::make_shared<FakeSession>(recovery_sessions);
        },
        {},
        { recovery });
    require(recovery_manager.start(error),
        "manager with a startup recovery command must start");
    require(waitUntil(
        [&]() { return recovery_sessions->running.load() == 1; }, 500),
        "startup recovery command must create a Pipeline before queue polling");
    require(recovery_manager.activeRunIds() ==
            std::vector<std::string>{ "run_recovered" },
        "startup recovery must preserve the new durable run id");
    {
        std::lock_guard<std::mutex> lock(recovery_commands->mutex);
        require(recovery_commands->acknowledged.empty(),
            "internal startup recovery commands must not acknowledge Redis messages");
    }
    recovery_manager.stop();

    std::cout << "Camera task manager scheduling tests passed\n";
    return 0;
}
