#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "business/people_flow_debug_types.h"
#include "business/people_flow_types.h"
#include "server/app_config.h"
#include "server/model_output.h"

namespace yolo11_server {

    class PersonDetectorAdapter {
    public:
        explicit PersonDetectorAdapter(const PeopleFlowSection& config);

        std::vector<PersonDetection> filter(
            const ModelOutput& output,
            const cv::Size& frame_size,
            long long timestamp_ms
        ) const;

        // Compatibility-preserving overload. Passing nullptr follows the
        // original allocation-light path and does not create rejected records.
        std::vector<PersonDetection> filter(
            const ModelOutput& output,
            const cv::Size& frame_size,
            long long timestamp_ms,
            PersonFilterResult* debug_result
        ) const;

        PersonFilterResult filterWithDebug(
            const ModelOutput& output,
            const cv::Size& frame_size,
            long long timestamp_ms
        ) const;

        bool acceptsAnchor(const PfPoint& point, const cv::Size& frame_size) const;

    private:
        PeopleFlowSection config_;
    };

}  // namespace yolo11_server
