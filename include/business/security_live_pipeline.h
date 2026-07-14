#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "business/electronic_fence_engine.h"
#include "business/pose_action_engine.h"
#include "business/temporal_action_engine.h"
#include "business/track_analytics_engine.h"
#include "server/app_config.h"
#include "server/model_output.h"

namespace yolo11_server {

    struct SecurityFrameResult {
        long long timestamp_ms = 0;
        std::vector<TrackedPose> poses;
        std::vector<TrackAnalyticsSnapshot> track_analytics;
        std::vector<TrackZoneStatus> zone_statuses;
        std::vector<SecurityEvent> new_events;
        std::vector<SecurityEvent> recent_events;
        std::map<std::int64_t, std::vector<std::string>> active_pose_actions;
        std::map<std::int64_t, std::vector<std::string>> active_temporal_actions;
        bool temporal_demo_classifier = true;
    };

    class SecurityLivePipeline {
    public:
        SecurityLivePipeline(
            const PeopleFlowSecuritySection& config,
            std::string session_id,
            std::string camera_id
        );

        SecurityFrameResult update(
            const std::vector<PersonTrack>& tracks,
            const ModelOutput& pose_output,
            const cv::Size& frame_size,
            long long timestamp_ms
        );

        void reset();

    private:
        std::vector<TrackedPose> associatePoses(
            const std::vector<PersonTrack>& tracks,
            const ModelOutput& pose_output,
            const cv::Size& frame_size,
            long long timestamp_ms
        ) const;
        void applyActionEvents(
            const std::vector<SecurityEvent>& events,
            std::map<std::int64_t, std::vector<std::string>>& active
        );
        void appendRecent(const std::vector<SecurityEvent>& events);

        PeopleFlowSecuritySection config_;
        ElectronicFenceEngine fence_;
        TrackAnalyticsEngine analytics_;
        PoseActionEngine pose_actions_;
        TemporalActionEngine temporal_actions_;
        std::deque<SecurityEvent> recent_events_;
        std::map<std::int64_t, std::vector<std::string>> active_pose_actions_;
        std::map<std::int64_t, std::vector<std::string>> active_temporal_actions_;
    };

}  // namespace yolo11_server
