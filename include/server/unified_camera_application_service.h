#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "business/camera_task_repository.h"
#include "server/app_config.h"
#include "server/camera_profile_registry.h"
#include "server/camera_task_api_control.h"

namespace yolo11_server {

enum class CameraApplicationFailure {
    none,
    not_found,
    invalid_request,
    conflict,
    unavailable
};

struct CameraStartApplicationResult {
    CameraTaskDefinition task;
    CameraTaskRunRecord run;
    bool idempotent_replay = false;
    bool recovered_active_run_conflict = false;
    CameraApplicationFailure failure = CameraApplicationFailure::none;
};

struct CameraStopApplicationResult {
    CameraTaskDefinition task;
    std::string run_id;
    std::string status;
    bool idempotent_replay = false;
    CameraApplicationFailure failure = CameraApplicationFailure::none;
};

// Application boundary shared by the canonical Camera HTTP API and, in the
// next migration phase, the legacy People Flow compatibility controller.
// HTTP authentication, request parsing, and response projection deliberately
// stay outside this class.
class UnifiedCameraApplicationService final {
public:
    UnifiedCameraApplicationService(
        AppConfig config,
        std::shared_ptr<CameraTaskRepository> repository,
        std::shared_ptr<ICameraTaskApiControl> control,
        std::shared_ptr<CameraProfileRegistry> profile_registry = {}
    );

    std::unique_lock<std::recursive_mutex> lockLifecycle() const;

    bool getActiveRun(
        const std::string& task_id,
        CameraTaskRunRecord& run,
        bool& found,
        std::string& error,
        bool include_stopping = true
    ) const;

    bool startCamera(
        const std::string& task_id,
        CameraStartApplicationResult& result,
        std::string& error_code,
        std::string& error,
        long long operation_time_ms = 0
    );

    bool stopCamera(
        const std::string& task_id,
        CameraStopApplicationResult& result,
        std::string& error_code,
        std::string& error,
        long long operation_time_ms = 0
    );

private:
    AppConfig config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::shared_ptr<ICameraTaskApiControl> control_;
    std::shared_ptr<CameraProfileRegistry> profile_registry_;
    mutable std::recursive_mutex lifecycle_mutex_;
};

}  // namespace yolo11_server
