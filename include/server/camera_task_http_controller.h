#pragma once

#include <memory>
#include <string>

#include <crow.h>

#include "business/camera_task_repository.h"
#include "server/algorithm_runtime_snapshot.h"
#include "server/app_config.h"
#include "server/camera_profile_registry.h"
#include "server/camera_task_api_control.h"

namespace yolo11_server {

class UnifiedCameraApplicationService;

struct CameraTaskHttpHealth {
    bool enabled = false;
    bool initialized = false;
    bool token_configured = false;
    bool storage_ok = false;
    bool output_root_writable = false;
    bool worker_num_valid = false;
    bool callback_config_valid = false;
};

class CameraTaskHttpController final {
public:
    explicit CameraTaskHttpController(
        const AppConfig& config,
        std::shared_ptr<CameraTaskRepository> repository = {},
        std::shared_ptr<ICameraTaskApiControl> control = {},
        std::string token_override = {},
        std::shared_ptr<CameraProfileRegistry> profile_registry = {},
        AlgorithmRuntimeSnapshotReader algorithm_runtime_reader = {},
        std::shared_ptr<UnifiedCameraApplicationService> application_service = {}
    );

    bool initialize(std::string& error);
    void registerRoutes(crow::SimpleApp& app);
    CameraTaskHttpHealth health() const;

    // Public handlers keep the contract deterministically testable without a
    // listening socket. All still execute the same authentication path.
    crow::response createTask(const crow::request& request);
    crow::response listTasks(const crow::request& request) const;
    crow::response getTask(const crow::request& request, const std::string& task_id) const;
    crow::response updateTask(
        const crow::request& request,
        const std::string& task_id
    );
    crow::response deleteTask(
        const crow::request& request,
        const std::string& task_id
    );
    crow::response startTask(
        const crow::request& request,
        const std::string& task_id
    );
    crow::response stopTask(
        const crow::request& request,
        const std::string& task_id
    );
    crow::response taskStatus(
        const crow::request& request,
        const std::string& task_id
    ) const;
    crow::response latestFrame(
        const crow::request& request,
        const std::string& task_id
    ) const;
    crow::response analysisSnapshot(
        const crow::request& request,
        const std::string& task_id
    ) const;
    crow::response listRuns(
        const crow::request& request,
        const std::string& task_id
    ) const;
    crow::response listAlerts(
        const crow::request& request,
        const std::string& task_id
    ) const;
    crow::response listHubs(const crow::request& request) const;
    crow::response getHub(
        const crow::request& request,
        const std::string& camera_profile
    ) const;
    crow::response createProfile(const crow::request& request);
    crow::response listProfiles(const crow::request& request) const;
    crow::response getProfile(
        const crow::request& request,
        const std::string& profile_id
    ) const;
    crow::response updateProfile(
        const crow::request& request,
        const std::string& profile_id
    );
    crow::response deleteProfile(
        const crow::request& request,
        const std::string& profile_id
    );
    crow::response operationsMetrics(const crow::request& request) const;
    crow::response prometheusMetrics(const crow::request& request) const;
    crow::response listCallbackDeliveries(
        const crow::request& request) const;
    crow::response replayCallbackDelivery(
        const crow::request& request,
        const std::string& outbox_id);

private:
    bool authorized(const crow::request& request) const;
    bool getActiveRun(
        const std::string& task_id,
        CameraTaskRunRecord& run,
        bool& found,
        std::string& error,
        bool include_stopping = true
    ) const;

    AppConfig config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::shared_ptr<ICameraTaskApiControl> control_;
    std::shared_ptr<CameraProfileRegistry> profile_registry_;
    std::shared_ptr<UnifiedCameraApplicationService> application_service_;
    AlgorithmRuntimeSnapshotReader algorithm_runtime_reader_;
    std::string token_;
    bool initialized_ = false;
    bool storage_ok_ = false;
    bool output_root_writable_ = false;
};

}  // namespace yolo11_server
