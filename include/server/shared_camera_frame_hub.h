#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "business/camera_frame_types.h"
#include "business/rtsp_capture_reader.h"
#include "server/app_config.h"

namespace yolo11_server {

class ICameraFrameSource {
public:
    virtual ~ICameraFrameSource() = default;
    virtual bool start(std::string& error) = 0;
    virtual void stop() noexcept = 0;
    virtual SharedCameraFrame latest(std::uint64_t after_sequence) const = 0;
    virtual RtspCaptureMetrics metrics() const = 0;
    virtual bool active() const = 0;
};

using CameraFrameSourceFactory =
    std::function<std::unique_ptr<ICameraFrameSource>(const std::string& camera_profile)>;

class SharedCameraFrameHub;

class FrameSubscription final {
public:
    ~FrameSubscription() noexcept;
    FrameSubscription(const FrameSubscription&) = delete;
    FrameSubscription& operator=(const FrameSubscription&) = delete;

    bool tryReadLatest(FrameReadResult& result);
    CameraHubStatus hubStatus() const;
    SubscriptionMetrics metrics() const;

private:
    friend class SharedCameraFrameHub;
    FrameSubscription(
        std::weak_ptr<SharedCameraFrameHub> hub,
        std::string subscriber_id,
        std::string subscriber_type
    );

    std::weak_ptr<SharedCameraFrameHub> hub_;
    mutable std::mutex mutex_;
    SubscriptionMetrics metrics_;
};

class SharedCameraFrameHub final : public std::enable_shared_from_this<SharedCameraFrameHub> {
public:
    SharedCameraFrameHub(
        std::string camera_profile,
        std::string hub_instance_id,
        int idle_grace_ms,
        std::unique_ptr<ICameraFrameSource> source
    );
    ~SharedCameraFrameHub() noexcept;

    bool subscribe(
        const SubscriberDescriptor& descriptor,
        std::shared_ptr<FrameSubscription>& subscription,
        std::string& error
    );
    SharedCameraFrame latest(std::uint64_t after_sequence) const;
    CameraHubStatus status() const;
    void maintenance(std::chrono::steady_clock::time_point now) noexcept;
    void stopNow() noexcept;
    bool evictable() const;

private:
    friend class FrameSubscription;
    void release(const std::string& subscriber_id) noexcept;

    std::string camera_profile_;
    std::string hub_instance_id_;
    int idle_grace_ms_ = 0;
    std::unique_ptr<ICameraFrameSource> source_;

    mutable std::mutex mutex_;
    std::map<std::string, SubscriberDescriptor> subscribers_;
    std::string lifecycle_state_ = "stopped";
    bool idle_deadline_set_ = false;
    std::chrono::steady_clock::time_point idle_deadline_{};
};

class SharedCameraFrameHubRegistry final {
public:
    SharedCameraFrameHubRegistry(
        const CameraHubSection& config,
        CameraFrameSourceFactory source_factory
    );
    ~SharedCameraFrameHubRegistry() noexcept;
    SharedCameraFrameHubRegistry(const SharedCameraFrameHubRegistry&) = delete;
    SharedCameraFrameHubRegistry& operator=(const SharedCameraFrameHubRegistry&) = delete;

    bool subscribe(
        const std::string& camera_profile,
        const SubscriberDescriptor& descriptor,
        std::shared_ptr<FrameSubscription>& subscription,
        std::string& error
    );
    std::vector<CameraHubStatus> snapshots() const;
    void stopAll() noexcept;

private:
    void maintenanceLoop() noexcept;
    static long long nowMs();

    CameraHubSection config_;
    CameraFrameSourceFactory source_factory_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<SharedCameraFrameHub>> hubs_;
    std::thread maintenance_thread_;
    std::atomic<bool> running_{ true };
    std::atomic<unsigned long long> instance_sequence_{ 0 };
};

}  // namespace yolo11_server
