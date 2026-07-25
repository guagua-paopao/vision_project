#pragma once

#include <memory>
#include <string>

#include <crow.h>

#include "business/camera_task_repository.h"
#include "business/people_flow_repository.h"
#include "server/app_config.h"
#include "server/camera_profile_registry.h"
#include "server/camera_task_api_control.h"
#include "server/unified_camera_application_service.h"

namespace yolo11_server {

// Projects the canonical Camera Run back onto the frozen People Flow v1 HTTP
// contract. Authentication remains route-owned so its matrix cannot drift.
class PeopleFlowCompatibilityController final {
public:
    PeopleFlowCompatibilityController(
        AppConfig config,
        std::shared_ptr<CameraTaskRepository> repository,
        std::shared_ptr<ICameraTaskApiControl> control,
        std::shared_ptr<CameraProfileRegistry> profile_registry,
        std::shared_ptr<UnifiedCameraApplicationService> application_service,
        PeopleFlowRepository* legacy_repository = nullptr
    );

    crow::response start(const crow::request& request);
    crow::response stop(const std::string& session_id);
    crow::response status(const std::string& session_id) const;
    crow::response snapshot(const std::string& session_id) const;
    crow::response security(const std::string& session_id) const;
    crow::response realtime(const std::string& camera_id) const;
    crow::response events(
        const crow::request& request,
        const std::string& camera_id
    ) const;

private:
    bool ensureDefinition(
        const std::string& camera_id,
        const std::string& camera_profile,
        long long operation_time_ms,
        std::string& error_code,
        std::string& error
    );
    bool findRun(
        const std::string& session_id,
        CameraTaskRunRecord& run,
        bool& found,
        std::string& error
    ) const;

    AppConfig config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::shared_ptr<ICameraTaskApiControl> control_;
    std::shared_ptr<CameraProfileRegistry> profile_registry_;
    std::shared_ptr<UnifiedCameraApplicationService> application_service_;
    PeopleFlowRepository* legacy_repository_ = nullptr;
};

}  // namespace yolo11_server
