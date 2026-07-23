#pragma once

#include "business/camera_pipeline.h"

namespace yolo11_server {

// M0-M11 source compatibility. New code should use CameraPipeline.
using CameraFrameExtractionSession = CameraPipeline;

}  // namespace yolo11_server
