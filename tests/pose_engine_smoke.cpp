#include <iostream>
#include <string>

#include "yolo11_pose_api.h"

int main(int argc, char** argv) {
    const std::string engine = argc > 1 ? argv[1] : "engines/yolo11n-pose.engine";
    yolo11::PoseConfig config;
    config.engine_path = engine;
    config.gpu_id = 0;
    config.use_gpu_postprocess = false;

    yolo11::Yolo11PoseDetector detector;
    if (!detector.init(config)) {
        std::cerr << "Failed to load pose TensorRT engine: " << engine << std::endl;
        return 1;
    }
    detector.release();
    std::cout << "PASS: pose TensorRT engine loaded successfully." << std::endl;
    return 0;
}
