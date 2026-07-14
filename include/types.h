#pragma once
#include "config.h"

struct alignas(float) Detection {
    // Detect/pose/seg: left, top, right, bottom in letterboxed model space.
    // OBB specializes the same storage as center_x, center_y, width, height.
    float bbox[4];
    float conf;  // bbox_conf * cls_conf
    float class_id;
    float mask[32];
    float keypoints[kNumberOfPoints * 3];  // 17*3 keypoints
    float angle;                           // obb angle
};

struct AffineMatrix {
    float value[6];
};

const int bbox_element =
        sizeof(AffineMatrix) / sizeof(float) + 1;  // left, top, right, bottom, confidence, class, keepflag
