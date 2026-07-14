#pragma once

#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "perf_metrics.h"

#include "postprocess.h"

namespace yolo11 {

    struct PoseConfig {
        std::string engine_path;
        int gpu_id = 0;
        bool use_gpu_postprocess = false;
    };

    class Yolo11PoseDetector {
    public:
        Yolo11PoseDetector();
        ~Yolo11PoseDetector();

        bool init(const PoseConfig& config);
        std::vector<Detection> infer(const cv::Mat& image);

        PerfMetrics lastPerf() const;
        std::vector<std::vector<Detection>> inferBatch(const std::vector<cv::Mat>& images);
        cv::Mat draw(const cv::Mat& image, const std::vector<Detection>& detections);
        void release();

    private:
        bool loadEngine(const std::string& engine_path);
        bool prepareBuffers();

    private:
        PoseConfig config_;

        void* runtime_ = nullptr;
        void* engine_ = nullptr;
        void* context_ = nullptr;
        void* stream_ = nullptr;

        float* input_device_ = nullptr;
        float* output_device_ = nullptr;
        float* output_host_ = nullptr;

        int model_bboxes_ = 0;
        PerfMetrics last_perf_;

        bool initialized_ = false;
    };

}  // namespace yolo11
