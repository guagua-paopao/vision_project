#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "server/shared_camera_frame_hub.h"

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

struct ControlledSourceState {
    std::atomic<int> starts{ 0 };
    std::atomic<int> stops{ 0 };
    std::atomic<bool> active{ false };
    mutable std::mutex mutex;
    SharedCameraFrame latest;
    RtspCaptureMetrics metrics;

    void setState(
        const std::string& state,
        int reconnect_count,
        long long open_count,
        const std::string& error = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        metrics.state = state;
        metrics.backend_name = "FFMPEG";
        metrics.reconnect_count = reconnect_count;
        metrics.open_count = open_count;
        metrics.last_error = error;
    }

    void publish(std::uint64_t sequence, int width, int height, bool resolution_changed = false) {
        auto envelope = std::make_shared<FrameEnvelope>();
        envelope->sequence = sequence;
        envelope->capture_time_ms = 1784500000000LL + static_cast<long long>(sequence);
        envelope->publish_time = std::chrono::steady_clock::now();
        envelope->resolution_changed = resolution_changed;
        envelope->image = cv::Mat(height, width, CV_8UC3,
            cv::Scalar(sequence % 255, 80, 160)).clone();
        std::lock_guard<std::mutex> lock(mutex);
        latest = std::static_pointer_cast<const FrameEnvelope>(envelope);
        metrics.state = "running";
        metrics.backend_name = "FFMPEG";
        metrics.width = width;
        metrics.height = height;
        metrics.resolution_changed = resolution_changed;
        if (resolution_changed) ++metrics.resolution_change_count;
        metrics.last_frame_time_ms = envelope->capture_time_ms;
        metrics.latest_frame_age_ms = 0;
        metrics.capture_fps = 25.0;
        metrics.source_fps = 25.0;
        metrics.last_error.clear();
    }
};

class ControlledSource final : public ICameraFrameSource {
public:
    explicit ControlledSource(std::shared_ptr<ControlledSourceState> state)
        : state_(std::move(state)) {}

    bool start(std::string& error) override {
        error.clear();
        ++state_->starts;
        state_->active.store(true);
        state_->setState("starting", 0, 1);
        return true;
    }

    void stop() noexcept override {
        if (state_->active.exchange(false)) ++state_->stops;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->metrics.state = "stopped";
    }

    SharedCameraFrame latest(std::uint64_t after_sequence) const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->latest || state_->latest->sequence <= after_sequence) return {};
        return state_->latest;
    }

    RtspCaptureMetrics metrics() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->metrics;
    }

    bool active() const override { return state_->active.load(); }

private:
    std::shared_ptr<ControlledSourceState> state_;
};

}  // namespace

int main() {
    using namespace yolo11_server;

    // AC-01/02/03/13: one People Flow subscriber and three extraction tasks
    // share one source object through disconnect/recovery and a resolution change.
    CameraHubSection single_config;
    single_config.max_active_hubs = 1;
    single_config.idle_grace_ms = 25;
    auto source_state = std::make_shared<ControlledSourceState>();
    std::atomic<int> source_factory_calls{ 0 };
    SharedCameraFrameHubRegistry registry(single_config,
        [&](const std::string&) {
            ++source_factory_calls;
            return std::make_unique<ControlledSource>(source_state);
        });

    std::vector<std::shared_ptr<FrameSubscription>> shared(4);
    std::string error;
    require(registry.subscribe("entry_camera_01", {"pf", "people_flow"}, shared[0], error),
        "People Flow subscription must start the shared source");
    for (int index = 1; index < 4; ++index) {
        require(registry.subscribe("entry_camera_01",
            {"camera_run_" + std::to_string(index), "camera_task"}, shared[index], error),
            "three camera tasks must share the People Flow source");
    }
    auto status = shared.front()->hubStatus();
    require(source_factory_calls.load() == 1 && source_state->starts.load() == 1 &&
        status.open_count == 1, "stable same-profile operation must have one source open");
    require(status.subscriber_count == 4 && status.subscriber_types["people_flow"] == 1 &&
        status.subscriber_types["camera_task"] == 3,
        "Hub diagnostics must report one People Flow and three Camera Task subscribers");

    source_state->publish(1, 320, 240);
    SharedCameraFrame shared_envelope;
    for (const auto& subscription : shared) {
        FrameReadResult read;
        require(subscription->tryReadLatest(read) && read.frame->sequence == 1,
            "all consumers must receive the stable frame");
        if (!shared_envelope) shared_envelope = read.frame;
        require(read.frame.get() == shared_envelope.get(),
            "all consumers must reference the same immutable envelope");
    }

    source_state->setState("reconnecting", 1, 1, "camera capture error");
    for (const auto& subscription : shared) {
        const auto reconnecting = subscription->hubStatus();
        require(reconnecting.state == "reconnecting" && reconnecting.reconnect_count == 1,
            "all consumers must observe the same reconnecting Hub state");
    }

    // open_count=2 models the one reader reopening after failure. The source
    // factory/start counts remain one, proving that recovery did not create a
    // concurrent second reader or Hub.
    source_state->setState("reconnecting", 1, 2);
    source_state->publish(2, 640, 360, true);
    for (const auto& subscription : shared) {
        FrameReadResult read;
        require(subscription->tryReadLatest(read) && read.frame->sequence == 2 &&
            read.frame->resolution_changed && read.frame->image.cols == 640 &&
            read.frame->image.rows == 360,
            "each cursor must resume on the post-reconnect resolution");
    }
    status = shared.front()->hubStatus();
    require(status.state == "running" && status.open_count == 2 &&
        status.reconnect_count == 1 && status.resolution_change_count == 1 &&
        source_factory_calls.load() == 1 && source_state->starts.load() == 1,
        "recovery must reuse the sole source while exposing reopen and resolution metrics");

    shared[1].reset();
    require(source_state->active.load(), "stopping one Camera Task must not stop the Hub");
    shared[0].reset();
    require(source_state->active.load(), "stopping People Flow must preserve remaining Camera Tasks");
    shared[2].reset();
    require(source_state->active.load(), "the penultimate subscriber must preserve the source");
    shared[3].reset();
    require(waitUntil([&]() { return source_state->stops.load() == 1; }, 500),
        "the final subscriber release must stop the sole source after idle grace");
    registry.stopAll();

    // AC-17: four independent profiles, two subscribers each. Each Hub keeps
    // only the latest frame and opens its own source exactly once.
    CameraHubSection multi_config;
    multi_config.max_active_hubs = 4;
    multi_config.idle_grace_ms = 20;
    std::mutex states_mutex;
    std::map<std::string, std::shared_ptr<ControlledSourceState>> states;
    SharedCameraFrameHubRegistry multi_registry(multi_config,
        [&](const std::string& profile) {
            auto state = std::make_shared<ControlledSourceState>();
            std::lock_guard<std::mutex> lock(states_mutex);
            states[profile] = state;
            return std::make_unique<ControlledSource>(state);
        });
    std::vector<std::shared_ptr<FrameSubscription>> multi_subscriptions;
    for (int profile_index = 0; profile_index < 4; ++profile_index) {
        const std::string profile = "camera_" + std::to_string(profile_index);
        for (int consumer = 0; consumer < 2; ++consumer) {
            std::shared_ptr<FrameSubscription> subscription;
            require(multi_registry.subscribe(profile,
                {profile + "_consumer_" + std::to_string(consumer),
                    consumer == 0 ? "people_flow" : "camera_task"}, subscription, error),
                "four-profile capacity test subscription must succeed");
            multi_subscriptions.push_back(std::move(subscription));
        }
    }
    require(multi_registry.snapshots().size() == 4 && states.size() == 4,
        "registry must host exactly four configured profiles");
    for (auto& entry : states) {
        require(entry.second->starts.load() == 1,
            "each profile must open exactly one source for both consumers");
        for (std::uint64_t sequence = 1; sequence <= 2000; ++sequence) {
            entry.second->publish(sequence, 32, 24);
        }
    }
    for (const auto& subscription : multi_subscriptions) {
        FrameReadResult read;
        require(subscription->tryReadLatest(read) && read.frame->sequence == 2000,
            "latest-only Hub storage must remain bounded under producer pressure");
    }
    multi_subscriptions.clear();
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(states_mutex);
        for (const auto& entry : states) if (entry.second->stops.load() != 1) return false;
        return true;
    }, 1000), "all four sources must stop after their final subscribers release");
    multi_registry.stopAll();

    std::cout << "Camera FrameHub resilience and capacity tests passed\n";
    return 0;
}
