#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

struct SourceState {
    std::atomic<int> starts{ 0 };
    std::atomic<int> stops{ 0 };
    mutable std::mutex mutex;
    SharedCameraFrame latest;
    RtspCaptureMetrics metrics;

    void publish(std::uint64_t sequence, const cv::Mat& image) {
        auto envelope = std::make_shared<FrameEnvelope>();
        envelope->sequence = sequence;
        envelope->capture_time_ms = 1700000000000LL + static_cast<long long>(sequence);
        envelope->publish_time = std::chrono::steady_clock::now();
        envelope->image = image.clone();
        std::lock_guard<std::mutex> lock(mutex);
        latest = std::static_pointer_cast<const FrameEnvelope>(envelope);
        metrics.state = "running";
        metrics.backend_name = "FFMPEG";
        metrics.open_count = starts.load();
        metrics.width = image.cols;
        metrics.height = image.rows;
        metrics.last_frame_time_ms = envelope->capture_time_ms;
    }
};

class Source final : public ICameraFrameSource {
public:
    explicit Source(std::shared_ptr<SourceState> state) : state_(std::move(state)) {}

    bool start(std::string& error) override {
        error.clear();
        ++state_->starts;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->metrics.state = "starting";
        state_->metrics.backend_name = "FFMPEG";
        state_->metrics.open_count = state_->starts.load();
        return true;
    }

    void stop() noexcept override {
        ++state_->stops;
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

    bool active() const override { return state_->starts.load() > state_->stops.load(); }

private:
    std::shared_ptr<SourceState> state_;
};

double fixedReadOnlyInference(const cv::Mat& input) {
    const cv::Scalar channels = cv::sum(input);
    return channels[0] + channels[1] * 3.0 + channels[2] * 7.0;
}

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
    config.idle_grace_ms = 20;
    auto state = std::make_shared<SourceState>();
    SharedCameraFrameHubRegistry registry(config,
        [state](const std::string&) { return std::make_unique<Source>(state); });

    std::shared_ptr<FrameSubscription> people_flow;
    std::shared_ptr<FrameSubscription> camera_task;
    std::string error;
    require(registry.subscribe("entry_camera_01", {"pf_session", "people_flow"},
        people_flow, error), "People Flow subscription must start");
    require(registry.subscribe("entry_camera_01", {"camera_run", "camera_task"},
        camera_task, error), "camera task must share the existing Hub");
    require(state->starts.load() == 1, "both consumers must share one source start");

    cv::Mat fixed(8, 12, CV_8UC3);
    for (int row = 0; row < fixed.rows; ++row) {
        for (int col = 0; col < fixed.cols; ++col) {
            fixed.at<cv::Vec3b>(row, col) = cv::Vec3b(row, col, row + col);
        }
    }
    state->publish(1, fixed);

    FrameReadResult pf_read;
    FrameReadResult camera_read;
    require(people_flow->tryReadLatest(pf_read) && camera_task->tryReadLatest(camera_read),
        "both consumers must receive the fixed frame");
    require(pf_read.frame.get() == camera_read.frame.get(),
        "People Flow and extraction must reference the same immutable envelope");
    require(fixedReadOnlyInference(fixed.clone()) == fixedReadOnlyInference(pf_read.frame->image),
        "shared-frame inference input must equal the legacy cloned input");

    const double source_score = fixedReadOnlyInference(pf_read.frame->image);
    cv::Mat rendered = pf_read.frame->image.clone();
    rendered.setTo(cv::Scalar(255, 255, 255));
    require(fixedReadOnlyInference(pf_read.frame->image) == source_score,
        "rendering a clone must not mutate the shared frame");

    state->publish(4, fixed);
    require(people_flow->tryReadLatest(pf_read) && pf_read.skipped_since_last_read == 2,
        "People Flow skipped frames must be subscription-local");
    require(camera_task->metrics().skipped_frames == 0,
        "People Flow sampling must not move the camera task cursor");

    people_flow.reset();
    require(state->stops.load() == 0,
        "stopping People Flow must not stop a Hub with a camera task subscriber");
    state->publish(5, fixed);
    require(camera_task->tryReadLatest(camera_read) && camera_read.frame->sequence == 5,
        "camera task must keep receiving frames after People Flow stops");

    camera_task.reset();
    require(waitUntil([&]() { return state->stops.load() == 1; }, 500),
        "the last subscriber release must stop the source after idle grace");
    registry.stopAll();
    std::cout << "People Flow Hub regression tests passed\n";
    return 0;
}
