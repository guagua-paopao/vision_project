#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "business/people_flow_debug_types.h"
#include "business/people_flow_types.h"
#include "server/app_config.h"

namespace yolo11_server {

    struct PeopleFlowRenderMetrics {
        double capture_fps = 0.0;
        double infer_fps = 0.0;
        long long latest_frame_age_ms = -1;
        int reconnect_count = 0;
        std::string capture_state;
    };

    class PeopleFlowRenderer {
    public:
        explicit PeopleFlowRenderer(const PeopleFlowSection& config);

        cv::Mat render(
            const cv::Mat& frame,
            const std::vector<PersonTrack>& tracks,
            const PeopleFlowCounts& counts,
            const PeopleFlowRenderMetrics& metrics
        ) const;

        cv::Mat render(
            const cv::Mat& frame,
            const std::vector<PersonTrack>& confirmed_tracks,
            const PeopleFlowCounts& counts,
            const PeopleFlowRenderMetrics& metrics,
            const PeopleFlowFrameDebug* debug
        ) const;

        // Reconnects must not retain event highlights from the previous stream.
        void resetEventMarkers() const;

    private:
        struct ActiveEventMarker {
            CrossingEvent event;
            int remaining_frames = 0;
        };

        PeopleFlowSection config_;
        mutable std::vector<ActiveEventMarker> event_markers_;
    };

}  // namespace yolo11_server
