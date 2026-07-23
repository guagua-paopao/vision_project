#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "business/people_flow_repository.h"
#include "server/algorithm_runtime_snapshot.h"
#include "server/app_config.h"
#include "server/model_runner.h"
#include "server/redis_task_queue.h"

namespace yolo11_server {

    class SharedCameraFrameHubRegistry;

    class PeopleFlowInferenceWorker {
    public:
        PeopleFlowInferenceWorker(
            int worker_id,
            const AppConfig& config,
            const std::string& consumer_name,
            std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry = {}
        );
        ~PeopleFlowInferenceWorker() noexcept;

        PeopleFlowInferenceWorker(const PeopleFlowInferenceWorker&) = delete;
        PeopleFlowInferenceWorker& operator=(const PeopleFlowInferenceWorker&) = delete;

        bool start();
        void stop() noexcept;
        bool running() const;
        void setAlgorithmRuntimeProvider(
            AlgorithmRuntimeSnapshotProvider provider);

    private:
        void loop();
        bool initModelRunner();
        void releaseRunnerNoexcept() noexcept;
        void heartbeatLoop() noexcept;
        void writeHeartbeatNoexcept() noexcept;
        void setWorkerState(const std::string& status, const std::string& session_id, const std::string& error);
        static long long nowMs();

    private:
        int worker_id_ = 0;
        AppConfig config_;
        RedisTaskQueue redis_queue_;
        RedisTaskQueue heartbeat_queue_;
        std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry_;
        bool owns_hub_registry_ = false;
        std::unique_ptr<PeopleFlowRepository> repository_;
        std::unique_ptr<IModelRunner> runner_;
        std::thread thread_;
        std::thread session_thread_;
        std::thread heartbeat_thread_;
        std::atomic<bool> running_{ false };
        std::atomic<bool> session_active_{ false };
        bool runner_initialized_ = false;
        mutable std::mutex state_mutex_;
        mutable std::mutex algorithm_provider_mutex_;
        AlgorithmRuntimeSnapshotProvider algorithm_provider_;
        std::string worker_status_ = "starting";
        std::string current_session_id_;
        std::string last_error_;
        long long process_start_time_ms_ = 0;
        std::atomic<long long> processed_count_{ 0 };
        std::atomic<long long> failed_count_{ 0 };
    };

}  // namespace yolo11_server
