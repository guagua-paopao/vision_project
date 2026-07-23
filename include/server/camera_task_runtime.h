#pragma once

#include <memory>
#include <string>

#include "server/app_config.h"
#include "server/camera_task_manager.h"
#include "server/shared_camera_frame_hub.h"

namespace yolo11_server {

std::unique_ptr<CameraTaskManager> createProductionCameraTaskManager(
    const AppConfig& config,
    const std::string& consumer_name,
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
    std::string& error
);

}  // namespace yolo11_server
