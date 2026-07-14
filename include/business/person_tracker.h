#pragma once

#include <cstdint>
#include <vector>

#include "business/people_flow_types.h"
#include "server/app_config.h"

namespace yolo11_server {

    class PersonTracker {
    public:
        explicit PersonTracker(const PeopleFlowTrackerSection& config);

        // Returns every active track, including tentative and predicted/missed
        // tracks. matched_this_frame distinguishes a detector match from motion
        // prediction without changing the matching algorithm.
        const std::vector<PersonTrack>& update(
            const std::vector<PersonDetection>& detections,
            int frame_width,
            int frame_height,
            long long timestamp_ms
        );

        std::vector<PersonTrack> confirmedTracks() const;
        void reset();
        std::size_t activeCount() const;

    private:
        PfRect predict(const PersonTrack& track, long long timestamp_ms) const;
        void applyDetection(PersonTrack& track, const PersonDetection& detection, long long timestamp_ms);

    private:
        PeopleFlowTrackerSection config_;
        std::vector<PersonTrack> tracks_;
        std::int64_t next_track_id_ = 1;
    };

}  // namespace yolo11_server
