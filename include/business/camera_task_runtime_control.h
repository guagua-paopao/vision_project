#pragma once

#include <string>

#include "business/camera_hub_status.h"

namespace yolo11_server {

struct CameraTaskRunHotStatus;

// Runtime control-plane contract. Tests can provide an in-memory implementation;
// production uses the independent Camera Task Redis connection.
class ICameraTaskRuntimeControl {
public:
    virtual ~ICameraTaskRuntimeControl() = default;
    virtual bool acquireRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error) = 0;
    virtual bool refreshRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error) = 0;
    virtual bool releaseRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error) = 0;
    virtual bool isStopRequested(
        const std::string& run_id,
        bool& requested,
        std::string& error) = 0;
    virtual bool updateRunStatus(const CameraTaskRunHotStatus& status, std::string& error) = 0;
    virtual bool updateHubStatus(const CameraHubStatus& status, std::string& error) = 0;
};

class ICameraAnalysisStatusSink {
public:
    virtual ~ICameraAnalysisStatusSink() = default;
    virtual bool updateAnalysisStatus(
        const CameraTaskRunHotStatus& status,
        std::string& error) = 0;
};

}  // namespace yolo11_server
