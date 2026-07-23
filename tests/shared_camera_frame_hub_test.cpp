#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "server/shared_camera_frame_hub.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
struct FakeSourceState {
    std::atomic<int> start_count{ 0 };
    std::atomic<int> stop_count{ 0 };
    std::atomic<bool> active{ false };
    mutable std::mutex mutex;
    SharedCameraFrame latest;
    RtspCaptureMetrics metrics;

    void publish(std::uint64_t sequence) {
        auto frame = std::make_shared<FrameEnvelope>();
        frame->sequence = sequence;
        frame->capture_time_ms = 1700000000000LL + static_cast<long long>(sequence);
        frame->publish_time = std::chrono::steady_clock::now();
        frame->image = cv::Mat(4, 4, CV_8UC3, cv::Scalar(sequence % 255, 0, 0)).clone();
        std::lock_guard<std::mutex> lock(mutex);
        latest = std::static_pointer_cast<const FrameEnvelope>(frame);
        metrics.state = "running";
        metrics.backend_name = "FFMPEG";
        metrics.open_count = start_count.load();
        metrics.last_frame_time_ms = frame->capture_time_ms;
        metrics.width = frame->image.cols;
        metrics.height = frame->image.rows;
    }
};

class FakeSource final : public ICameraFrameSource {
public:
    explicit FakeSource(std::shared_ptr<FakeSourceState> state) : state_(std::move(state)) {}

    bool start(std::string& error) override {
        error.clear();
        ++state_->start_count;
        state_->active.store(true);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->metrics.state = "starting";
        state_->metrics.backend_name = "FFMPEG";
        state_->metrics.open_count = state_->start_count.load();
        return true;
    }

    void stop() noexcept override {
        if (state_->active.exchange(false)) ++state_->stop_count;
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
    std::shared_ptr<FakeSourceState> state_;
};

bool waitUntil(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

int main() {
    using namespace yolo11_server;

    CameraHubSection config;
    config.max_active_hubs = 1;
    config.idle_grace_ms = 25;
    auto state = std::make_shared<FakeSourceState>();
    std::atomic<int> factory_count{ 0 };
    SharedCameraFrameHubRegistry registry(config,
        [&](const std::string&) -> std::unique_ptr<ICameraFrameSource> {
            ++factory_count;
            return std::make_unique<FakeSource>(state);
        });

    std::vector<std::shared_ptr<FrameSubscription>> subscriptions(10);
    std::vector<std::thread> threads;
    std::atomic<int> subscribed{ 0 };
    for (int index = 0; index < 10; ++index) {
        threads.emplace_back([&, index]() {
            std::string error;
            if (registry.subscribe("entry_camera_01",
                    {"subscriber_" + std::to_string(index), index == 0 ? "people_flow" : "camera_task"},
                    subscriptions[index], error)) {
                ++subscribed;
            }
        });
    }
    for (auto& thread : threads) thread.join();

    require(subscribed.load() == 10, "ten concurrent subscribers must succeed");
    require(factory_count.load() == 1, "one profile must create exactly one source");
    require(state->start_count.load() == 1, "one profile must start exactly one source");

    state->publish(1);
    for (const auto& subscription : subscriptions) {
        FrameReadResult read;
        require(subscription->tryReadLatest(read), "every subscriber must read sequence one");
        require(read.frame && read.frame->sequence == 1, "shared sequence must be one");
        require(read.skipped_since_last_read == 0, "first read must not report skipped frames");
    }

    state->publish(4);
    FrameReadResult fast_read;
    require(subscriptions[0]->tryReadLatest(fast_read), "subscriber must read newest sequence");
    require(fast_read.frame->sequence == 4 && fast_read.skipped_since_last_read == 2,
        "subscription must account for independently skipped sequences");
    require(subscriptions[1]->metrics().last_seen_sequence == 1,
        "one subscription cursor must not mutate another subscription cursor");

    const auto hub_status = subscriptions[0]->hubStatus();
    require(hub_status.subscriber_count == 10, "hub must expose all subscribers");
    require(hub_status.subscriber_types.at("people_flow") == 1,
        "hub must count people-flow subscribers");
    require(hub_status.subscriber_types.at("camera_task") == 9,
        "hub must count camera-task subscribers");
    require(hub_status.open_count == 1, "hub open count must remain one");

    std::shared_ptr<FrameSubscription> capacity_subscription;
    std::string capacity_error;
    require(!registry.subscribe("second_camera", {"capacity", "camera_task"},
            capacity_subscription, capacity_error),
        "a second profile must be rejected at hub capacity");
    require(capacity_error == "CAMERA_HUB_CAPACITY_EXCEEDED",
        "capacity rejection must use a stable error code");

    for (auto& subscription : subscriptions) subscription.reset();
    require(waitUntil([&]() { return state->stop_count.load() == 1; }, 500),
        "last release must stop the source after idle grace");
    require(waitUntil([&]() { return registry.snapshots().empty(); }, 500),
        "stopped unreferenced hub must be evicted");

    registry.stopAll();
    std::cout << "Shared camera FrameHub tests passed\n";
    return 0;
}
