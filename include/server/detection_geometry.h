#pragma once

#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>

#include "config.h"
#include "types.h"

namespace yolo11_server {

    // Detect/pose/seg TensorRT plugins expose axis-aligned boxes as
    // [left, top, right, bottom] in the letterboxed network-input space.
    // Business modules must convert that geometry before applying pixel-size,
    // ROI, tracking, or line-crossing logic on the original image.
    struct DetectionImageGeometry {
        bool valid = false;
        cv::Rect2d mapped_bbox;
        cv::Rect clipped_bbox;
    };

    inline DetectionImageGeometry detectionToImageGeometry(
        const Detection& detection,
        const cv::Size& image_size
    ) {
        DetectionImageGeometry geometry;
        if (image_size.width <= 0 || image_size.height <= 0) return geometry;

        const double model_left = detection.bbox[0];
        const double model_top = detection.bbox[1];
        const double model_right = detection.bbox[2];
        const double model_bottom = detection.bbox[3];
        if (!std::isfinite(model_left) || !std::isfinite(model_top) ||
            !std::isfinite(model_right) || !std::isfinite(model_bottom) ||
            model_right <= model_left || model_bottom <= model_top) {
            return geometry;
        }

        const double scale = std::min(
            static_cast<double>(kInputW) / image_size.width,
            static_cast<double>(kInputH) / image_size.height
        );
        if (!std::isfinite(scale) || scale <= 0.0) return geometry;

        const double pad_x = (kInputW - image_size.width * scale) * 0.5;
        const double pad_y = (kInputH - image_size.height * scale) * 0.5;
        const double left = (model_left - pad_x) / scale;
        const double top = (model_top - pad_y) / scale;
        const double right = (model_right - pad_x) / scale;
        const double bottom = (model_bottom - pad_y) / scale;
        if (!std::isfinite(left) || !std::isfinite(top) ||
            !std::isfinite(right) || !std::isfinite(bottom) ||
            right <= left || bottom <= top) {
            return geometry;
        }

        geometry.mapped_bbox = cv::Rect2d(left, top, right - left, bottom - top);
        const double clipped_left = std::clamp(left, 0.0, static_cast<double>(image_size.width));
        const double clipped_top = std::clamp(top, 0.0, static_cast<double>(image_size.height));
        const double clipped_right = std::clamp(right, 0.0, static_cast<double>(image_size.width));
        const double clipped_bottom = std::clamp(bottom, 0.0, static_cast<double>(image_size.height));
        if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) return geometry;

        const int x1 = std::clamp(
            static_cast<int>(std::lround(clipped_left)), 0, image_size.width - 1);
        const int y1 = std::clamp(
            static_cast<int>(std::lround(clipped_top)), 0, image_size.height - 1);
        const int x2 = std::clamp(
            static_cast<int>(std::lround(clipped_right)), x1 + 1, image_size.width);
        const int y2 = std::clamp(
            static_cast<int>(std::lround(clipped_bottom)), y1 + 1, image_size.height);
        geometry.clipped_bbox = cv::Rect(x1, y1, x2 - x1, y2 - y1);
        geometry.valid = geometry.clipped_bbox.area() > 0;
        return geometry;
    }

}  // namespace yolo11_server
