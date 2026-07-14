#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "business/security_live_pipeline.h"
#include "server/app_config.h"

namespace yolo11_server {

    class SecurityOverlayRenderer {
    public:
        explicit SecurityOverlayRenderer(PeopleFlowSecuritySection config);

        cv::Mat render(const cv::Mat& frame, const SecurityFrameResult& security) const;
        void reset() const;

    private:
        struct EventMarker {
            SecurityEvent event;
            int remaining_frames = 0;
        };

        PeopleFlowSecuritySection config_;
        mutable std::vector<EventMarker> markers_;
    };

}  // namespace yolo11_server
