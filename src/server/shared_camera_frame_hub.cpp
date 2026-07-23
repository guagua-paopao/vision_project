#include "server/shared_camera_frame_hub.h"

#include <algorithm>
#include <utility>

namespace yolo11_server {

namespace {

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

FrameSubscription::FrameSubscription(
    std::weak_ptr<SharedCameraFrameHub> hub,
    std::string subscriber_id,
    std::string subscriber_type
) : hub_(std::move(hub)) {
    metrics_.subscriber_id = std::move(subscriber_id);
    metrics_.subscriber_type = std::move(subscriber_type);
}

FrameSubscription::~FrameSubscription() noexcept {
    if (const auto hub = hub_.lock()) hub->release(metrics_.subscriber_id);
}

bool FrameSubscription::tryReadLatest(FrameReadResult& result) {
    result = FrameReadResult{};
    const auto hub = hub_.lock();
    if (!hub) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const SharedCameraFrame frame = hub->latest(metrics_.last_seen_sequence);
    if (!frame) return false;
    const std::uint64_t skipped = metrics_.last_seen_sequence > 0 &&
        frame->sequence > metrics_.last_seen_sequence + 1
        ? frame->sequence - metrics_.last_seen_sequence - 1
        : 0;
    metrics_.last_seen_sequence = frame->sequence;
    ++metrics_.consumed_frames;
    metrics_.skipped_frames += static_cast<long long>(skipped);
    metrics_.last_consume_time_ms = wallNowMs();
    result.frame = frame;
    result.skipped_since_last_read = skipped;
    return true;
}

CameraHubStatus FrameSubscription::hubStatus() const {
    const auto hub = hub_.lock();
    return hub ? hub->status() : CameraHubStatus{};
}

SubscriptionMetrics FrameSubscription::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

SharedCameraFrameHub::SharedCameraFrameHub(
    std::string camera_profile,
    std::string hub_instance_id,
    int idle_grace_ms,
    std::unique_ptr<ICameraFrameSource> source
) : camera_profile_(std::move(camera_profile)),
    hub_instance_id_(std::move(hub_instance_id)),
    idle_grace_ms_(std::max(0, idle_grace_ms)),
    source_(std::move(source)) {
}

SharedCameraFrameHub::~SharedCameraFrameHub() noexcept {
    stopNow();
}

bool SharedCameraFrameHub::subscribe(
    const SubscriberDescriptor& descriptor,
    std::shared_ptr<FrameSubscription>& subscription,
    std::string& error
) {
    subscription.reset();
    error.clear();
    if (!source_ || descriptor.subscriber_id.empty() || descriptor.subscriber_type.empty()) {
        error = "invalid camera hub subscription";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_state_ == "stopping") {
        error = "camera hub is stopping";
        return false;
    }
    if (lifecycle_state_ != "stopped" && source_->metrics().state == "failed") {
        if (!subscribers_.empty()) {
            error = "camera hub source is failed";
            return false;
        }
        source_->stop();
        lifecycle_state_ = "stopped";
        idle_deadline_set_ = false;
    }
    if (subscribers_.find(descriptor.subscriber_id) != subscribers_.end()) {
        error = "duplicate camera hub subscriber id";
        return false;
    }
    if (lifecycle_state_ == "stopped") {
        lifecycle_state_ = "starting";
        if (!source_->start(error)) {
            lifecycle_state_ = "stopped";
            return false;
        }
    }
    if (lifecycle_state_ == "failed") {
        error = "camera hub source is failed";
        return false;
    }
    idle_deadline_set_ = false;
    subscribers_.emplace(descriptor.subscriber_id, descriptor);
    subscription = std::shared_ptr<FrameSubscription>(new FrameSubscription(
        weak_from_this(), descriptor.subscriber_id, descriptor.subscriber_type));
    return true;
}

SharedCameraFrame SharedCameraFrameHub::latest(std::uint64_t after_sequence) const {
    return source_ ? source_->latest(after_sequence) : SharedCameraFrame{};
}

CameraHubStatus SharedCameraFrameHub::status() const {
    const RtspCaptureMetrics source_metrics = source_ ? source_->metrics() : RtspCaptureMetrics{};
    const SharedCameraFrame frame = source_ ? source_->latest(0) : SharedCameraFrame{};
    CameraHubStatus result;
    result.hub_instance_id = hub_instance_id_;
    result.camera_profile = camera_profile_;
    result.backend_name = source_metrics.backend_name;
    result.last_error = source_metrics.last_error;
    result.open_count = source_metrics.open_count;
    result.reconnect_count = source_metrics.reconnect_count;
    result.capture_fps = source_metrics.capture_fps;
    result.source_fps = source_metrics.source_fps;
    result.latest_frame_age_ms = source_metrics.latest_frame_age_ms;
    result.last_frame_time_ms = source_metrics.last_frame_time_ms;
    result.width = source_metrics.width;
    result.height = source_metrics.height;
    result.resolution_changed = source_metrics.resolution_changed;
    result.resolution_change_count = source_metrics.resolution_change_count;
    if (frame) result.latest_sequence = frame->sequence;

    std::lock_guard<std::mutex> lock(mutex_);
    result.state = idle_deadline_set_ ? "idle_grace" :
        (lifecycle_state_ == "starting" && !source_metrics.state.empty()
            ? source_metrics.state
            : lifecycle_state_);
    if (!idle_deadline_set_ && lifecycle_state_ != "stopped" &&
        lifecycle_state_ != "stopping" && lifecycle_state_ != "failed" &&
        !source_metrics.state.empty()) {
        result.state = source_metrics.state == "opening" ? "starting" : source_metrics.state;
    }
    result.subscriber_count = static_cast<int>(subscribers_.size());
    for (const auto& entry : subscribers_) ++result.subscriber_types[entry.second.subscriber_type];
    return result;
}

void SharedCameraFrameHub::release(const std::string& subscriber_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.erase(subscriber_id);
        if (subscribers_.empty() && lifecycle_state_ != "stopped" && lifecycle_state_ != "stopping") {
            idle_deadline_set_ = true;
            idle_deadline_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(idle_grace_ms_);
        }
    }
    catch (...) {
    }
}

void SharedCameraFrameHub::maintenance(std::chrono::steady_clock::time_point now) noexcept {
    bool should_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idle_deadline_set_ && subscribers_.empty() && now >= idle_deadline_) {
            idle_deadline_set_ = false;
            lifecycle_state_ = "stopping";
            should_stop = true;
        }
    }
    if (should_stop) {
        if (source_) source_->stop();
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_state_ = "stopped";
    }
}

void SharedCameraFrameHub::stopNow() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_state_ == "stopped" && !source_) return;
        lifecycle_state_ = "stopping";
        idle_deadline_set_ = false;
        subscribers_.clear();
    }
    if (source_) source_->stop();
    std::lock_guard<std::mutex> lock(mutex_);
    lifecycle_state_ = "stopped";
}

bool SharedCameraFrameHub::evictable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lifecycle_state_ == "stopped" && subscribers_.empty();
}

SharedCameraFrameHubRegistry::SharedCameraFrameHubRegistry(
    const CameraHubSection& config,
    CameraFrameSourceFactory source_factory
) : config_(config), source_factory_(std::move(source_factory)) {
    maintenance_thread_ = std::thread([this]() { maintenanceLoop(); });
}

SharedCameraFrameHubRegistry::~SharedCameraFrameHubRegistry() noexcept {
    stopAll();
}

bool SharedCameraFrameHubRegistry::subscribe(
    const std::string& camera_profile,
    const SubscriberDescriptor& descriptor,
    std::shared_ptr<FrameSubscription>& subscription,
    std::string& error
) {
    subscription.reset();
    error.clear();
    if (!running_.load() || camera_profile.empty() || !source_factory_) {
        error = "camera hub registry is unavailable";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<SharedCameraFrameHub> hub;
    auto found = hubs_.find(camera_profile);
    if (found != hubs_.end()) {
        hub = found->second;
    }
    else {
        if (static_cast<int>(hubs_.size()) >= config_.max_active_hubs) {
            error = "CAMERA_HUB_CAPACITY_EXCEEDED";
            return false;
        }
        std::unique_ptr<ICameraFrameSource> source = source_factory_(camera_profile);
        if (!source) {
            error = "CAMERA_PROFILE_UNAVAILABLE";
            return false;
        }
        const auto sequence = ++instance_sequence_;
        const std::string instance_id = "hub_" + camera_profile + "_" +
            std::to_string(nowMs()) + "_" + std::to_string(sequence);
        hub = std::make_shared<SharedCameraFrameHub>(
            camera_profile, instance_id, config_.idle_grace_ms, std::move(source));
        hubs_.emplace(camera_profile, hub);
    }
    if (hub->subscribe(descriptor, subscription, error)) return true;
    if (hub->evictable()) {
        const auto found = hubs_.find(camera_profile);
        if (found != hubs_.end() && found->second == hub) hubs_.erase(found);
    }
    return false;
}

std::vector<CameraHubStatus> SharedCameraFrameHubRegistry::snapshots() const {
    std::vector<std::shared_ptr<SharedCameraFrameHub>> hubs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : hubs_) hubs.push_back(entry.second);
    }
    std::vector<CameraHubStatus> result;
    result.reserve(hubs.size());
    for (const auto& hub : hubs) result.push_back(hub->status());
    return result;
}

void SharedCameraFrameHubRegistry::stopAll() noexcept {
    if (!running_.exchange(false)) return;
    try {
        if (maintenance_thread_.joinable()) maintenance_thread_.join();
        std::vector<std::shared_ptr<SharedCameraFrameHub>> hubs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : hubs_) hubs.push_back(entry.second);
            hubs_.clear();
        }
        for (const auto& hub : hubs) hub->stopNow();
    }
    catch (...) {
    }
}

void SharedCameraFrameHubRegistry::maintenanceLoop() noexcept {
    while (running_.load()) {
        std::vector<std::pair<std::string, std::shared_ptr<SharedCameraFrameHub>>> hubs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : hubs_) hubs.push_back(entry);
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& entry : hubs) entry.second->maintenance(now);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : hubs) {
                const auto found = hubs_.find(entry.first);
                if (found != hubs_.end() && found->second == entry.second && entry.second->evictable()) {
                    hubs_.erase(found);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

long long SharedCameraFrameHubRegistry::nowMs() {
    return wallNowMs();
}

}  // namespace yolo11_server
