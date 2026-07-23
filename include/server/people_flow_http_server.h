#pragma once

#include <memory>
#include <string>

#include <crow.h>

#include "business/people_flow_repository.h"
#include "server/app_config.h"
#include "server/camera_task_http_controller.h"
#include "server/redis_task_queue.h"

namespace yolo11_server {

// Dedicated HTTP producer/query layer for the compact demo. It exposes only
// the routes consumed by the Qt client and never loads TensorRT itself.
class PeopleFlowHttpServer {
public:
    explicit PeopleFlowHttpServer(const AppConfig& config);
    ~PeopleFlowHttpServer() noexcept;

    bool initialize(std::string& error);
    void registerRoutes(crow::SimpleApp& app);

private:
    bool authorized(const crow::request& request) const;
    crow::response health() const;
    crow::response ready() const;
    crow::response start(const crow::request& request);
    crow::response stop(const crow::request& request, const std::string& session_id) const;
    crow::response status(const std::string& session_id) const;
    crow::response snapshot(const std::string& session_id) const;
    crow::response security(const std::string& session_id) const;
    crow::response realtime(const std::string& camera_id) const;
    crow::response events(const crow::request& request, const std::string& camera_id) const;
    crow::response adminAsset(const std::string& file_name, const std::string& content_type) const;

    AppConfig config_;
    mutable RedisTaskQueue redis_;
    std::unique_ptr<PeopleFlowRepository> repository_;
    std::unique_ptr<CameraTaskHttpController> camera_task_controller_;
    std::string admin_token_;
};

}  // namespace yolo11_server
