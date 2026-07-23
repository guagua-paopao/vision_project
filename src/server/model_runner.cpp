#include "server/model_runner.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>

#include "yolo11_pose_api.h"

namespace yolo11_server {

namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}
}  // namespace

PoseModelRunner::~PoseModelRunner() = default;

std::string PoseModelRunner::modelType() const {
    return "pose";
}

bool PoseModelRunner::init(const AppConfig& config, std::string& error) {
    try {
        yolo11::PoseConfig pose_config;
        pose_config.engine_path = config.model.engine_path;
        pose_config.gpu_id = config.model.gpu_id;
        pose_config.use_gpu_postprocess = false;

        detector_ = std::make_unique<yolo11::Yolo11PoseDetector>();
        if (!detector_->init(pose_config)) {
            error = "Yolo11PoseDetector::init returned false";
            detector_.reset();
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        error = e.what();
        detector_.reset();
        return false;
    }
    catch (...) {
        error = "unknown exception while initializing PoseModelRunner";
        detector_.reset();
        return false;
    }
}

ModelOutput PoseModelRunner::infer(const cv::Mat& image) {
    if (!detector_) throw std::runtime_error("PoseModelRunner is not initialized");
    ModelOutput output;
    output.model_type = "pose";
    output.detections = detector_->infer(image);
    output.perf = detector_->lastPerf();
    return output;
}

cv::Mat PoseModelRunner::draw(const cv::Mat& image, const ModelOutput& output) {
    if (!detector_) throw std::runtime_error("PoseModelRunner is not initialized");
    return detector_->draw(image, output.detections);
}

void PoseModelRunner::release() noexcept {
    try {
        if (detector_) detector_->release();
    }
    catch (...) {
    }
    detector_.reset();
}

std::unique_ptr<IModelRunner> createModelRunner(const std::string& model_type) {
    if (lower(model_type) == "pose") return std::make_unique<PoseModelRunner>();
    return nullptr;
}

}  // namespace yolo11_server
