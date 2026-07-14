#pragma once

#include <memory>
#include <string>

#include <opencv2/opencv.hpp>

#include "server/app_config.h"
#include "server/model_output.h"
#include "yolo11_pose_api.h"

namespace yolo11_server {

class IModelRunner {
public:
    virtual ~IModelRunner() = default;
    virtual std::string modelType() const = 0;
    virtual bool init(const AppConfig& config, std::string& error) = 0;
    virtual ModelOutput infer(const cv::Mat& image) = 0;
    virtual cv::Mat draw(const cv::Mat& image, const ModelOutput& output) = 0;
    virtual void release() noexcept = 0;
};

// This compact project deliberately supports only the pose engine used by the
// four-stage demo. Detection boxes and COCO keypoints come from one inference.
class PoseModelRunner final : public IModelRunner {
public:
    std::string modelType() const override;
    bool init(const AppConfig& config, std::string& error) override;
    ModelOutput infer(const cv::Mat& image) override;
    cv::Mat draw(const cv::Mat& image, const ModelOutput& output) override;
    void release() noexcept override;

private:
    std::unique_ptr<yolo11::Yolo11PoseDetector> detector_;
};

std::unique_ptr<IModelRunner> createModelRunner(const std::string& model_type);

}  // namespace yolo11_server
