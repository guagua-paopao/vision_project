#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "server/app_config.h"
#include "server/rtsp_camera_frame_source.h"
#include "server/shared_camera_frame_hub.h"

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: rtsp_capture_smoke <config.yaml> [stability-seconds]\n";
        return 2;
    }

    try {
        const int stability_seconds = argc == 3 ? std::stoi(argv[2]) : 0;
        if (stability_seconds < 0 || stability_seconds > 600) {
            std::cerr << "FAIL: stability-seconds must be between 0 and 600\n";
            return 2;
        }
        const auto config = yolo11_server::AppConfig::loadFromYaml(argv[1]);
        const std::string profile = config.people_flow.camera_profile;
        auto registry = yolo11_server::createSharedCameraFrameHubRegistry(config);
        if (!registry || profile.empty()) {
            std::cerr << "FAIL: production Camera FrameHub configuration is unavailable\n";
            return 1;
        }

        std::shared_ptr<yolo11_server::FrameSubscription> subscription;
        std::string error;
        if (!registry->subscribe(profile, {"rtsp_capture_smoke", "diagnostic"}, subscription, error)) {
            std::cerr << "FAIL: FrameHub subscription failed: " << error << '\n';
            return 1;
        }

        const auto first_frame_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto stability_deadline = first_frame_deadline;
        std::uint64_t first_sequence = 0;
        yolo11_server::CameraHubStatus status;
        while (std::chrono::steady_clock::now() <
                (first_sequence > 0 ? stability_deadline : first_frame_deadline)) {
            yolo11_server::FrameReadResult frame;
            (void)subscription->tryReadLatest(frame);
            status = subscription->hubStatus();
            if (status.open_count == 1 && status.latest_sequence > 0 &&
                status.backend_name.find("FFMPEG") != std::string::npos) {
                if (first_sequence == 0) {
                    first_sequence = status.latest_sequence;
                    stability_deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(stability_seconds);
                    if (stability_seconds == 0) break;
                }
            }
            if (status.state == "failed" || status.state == "reconnecting") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        status = subscription->hubStatus();
        if (first_sequence > 0 && status.open_count == 1 && status.reconnect_count == 0 &&
            status.latest_sequence >= first_sequence +
                static_cast<std::uint64_t>(stability_seconds > 0 ? stability_seconds : 0)) {
            std::cout << "PASS: backend=" << status.backend_name
                      << ", resolution=" << status.width << 'x' << status.height
                      << ", source_fps=" << status.source_fps
                      << ", open_count=" << status.open_count
                      << ", reconnect_count=" << status.reconnect_count
                      << ", sequence_advance=" << (status.latest_sequence - first_sequence)
                      << ", stability_seconds=" << stability_seconds << '\n';
            registry->stopAll();
            return 0;
        }

        std::cerr << "FAIL: state=" << status.state
                  << ", backend=" << status.backend_name
                  << ", open_count=" << status.open_count
                  << ", reconnect_count=" << status.reconnect_count
                  << ", sequence_advance="
                  << (status.latest_sequence >= first_sequence
                      ? status.latest_sequence - first_sequence : 0)
                  << ", last_error=" << status.last_error << '\n';
        registry->stopAll();
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "FAIL: RTSP smoke raised a standard exception: " << e.what() << '\n';
        return 1;
    }
    catch (...) {
        std::cerr << "FAIL: RTSP smoke raised an unknown exception\n";
        return 1;
    }
}
