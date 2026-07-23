#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "server/app_config.h"
#include "server/rtsp_camera_frame_source.h"
#include "server/shared_camera_frame_hub.h"
#include "yolo11_pose_api.h"

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: pose_rtsp_interop_smoke <config.yaml> [stability-seconds]\n";
        return 2;
    }
    try {
        const int stability_seconds = argc == 3 ? std::stoi(argv[2]) : 0;
        if (stability_seconds < 0 || stability_seconds > 600) {
            std::cerr << "FAIL: stability-seconds must be between 0 and 600\n";
            return 2;
        }
        const auto config = yolo11_server::AppConfig::loadFromYaml(argv[1]);
        yolo11::PoseConfig pose;
        pose.engine_path = config.model.engine_path;
        pose.gpu_id = config.model.gpu_id;
        pose.use_gpu_postprocess = false;
        yolo11::Yolo11PoseDetector detector;
        if (!detector.init(pose)) {
            std::cerr << "FAIL: TensorRT pose engine initialization failed\n";
            return 1;
        }

        auto registry = yolo11_server::createSharedCameraFrameHubRegistry(config);
        std::shared_ptr<yolo11_server::FrameSubscription> subscription;
        std::string error;
        if (!registry || !registry->subscribe(config.people_flow.camera_profile,
            {"pose_rtsp_interop_smoke", "diagnostic"}, subscription, error)) {
            detector.release();
            std::cerr << "FAIL: FrameHub subscription failed: " << error << '\n';
            return 1;
        }

        const auto first_frame_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto stability_deadline = first_frame_deadline;
        std::uint64_t last_inferred_sequence = 0;
        long long inference_count = 0;
        auto next_inference = std::chrono::steady_clock::now();
        yolo11_server::CameraHubStatus status;
        while (std::chrono::steady_clock::now() <
                (inference_count > 0 ? stability_deadline : first_frame_deadline)) {
            yolo11_server::FrameReadResult frame;
            (void)subscription->tryReadLatest(frame);
            status = subscription->hubStatus();
            if (status.open_count == 1 && status.latest_sequence > 0 &&
                status.backend_name.find("FFMPEG") != std::string::npos &&
                frame.frame && frame.frame->sequence > last_inferred_sequence &&
                std::chrono::steady_clock::now() >= next_inference) {
                (void)detector.infer(frame.frame->image);
                last_inferred_sequence = frame.frame->sequence;
                ++inference_count;
                next_inference = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(100);
                if (inference_count == 1) {
                    stability_deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(stability_seconds);
                    if (stability_seconds == 0) break;
                }
            }
            if (status.state == "failed" || status.state == "reconnecting") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        status = subscription->hubStatus();
        if (inference_count > 0 && status.open_count == 1 && status.reconnect_count == 0 &&
            inference_count >= (stability_seconds > 0 ? stability_seconds : 1)) {
            registry->stopAll();
            detector.release();
            std::cout << "PASS: TensorRT+FFmpeg interop, resolution="
                      << status.width << 'x' << status.height
                      << ", open_count=" << status.open_count
                      << ", reconnect_count=" << status.reconnect_count
                      << ", inference_count=" << inference_count
                      << ", stability_seconds=" << stability_seconds << '\n';
            return 0;
        }
        registry->stopAll();
        detector.release();
        std::cerr << "FAIL: interop state=" << status.state
                  << ", backend=" << status.backend_name
                  << ", open_count=" << status.open_count
                  << ", reconnect_count=" << status.reconnect_count
                  << ", inference_count=" << inference_count << '\n';
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: interop smoke raised a standard exception: " << e.what() << '\n';
        return 1;
    }
    catch (...) {
        std::cerr << "FAIL: interop smoke raised an unknown exception\n";
        return 1;
    }
}
