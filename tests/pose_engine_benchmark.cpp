#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "yolo11_pose_api.h"

namespace {

struct Sample {
    double wall_ms = 0.0;
    PerfMetrics perf;
    std::size_t detections = 0;
};

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
}

cv::Mat makeSyntheticImage() {
    cv::Mat image(1080, 1920, CV_8UC3, cv::Scalar(114, 114, 114));
    cv::rectangle(image, cv::Rect(700, 150, 420, 820),
        cv::Scalar(90, 130, 170), cv::FILLED);
    cv::circle(image, cv::Point(910, 250), 90,
        cv::Scalar(155, 175, 200), cv::FILLED);
    cv::putText(image, "YOLO11 POSE BENCHMARK", cv::Point(60, 1010),
        cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(230, 230, 230), 4);
    return image;
}

int parsePositiveInt(const char* text, const char* name, int max_value) {
    const int value = std::stoi(text);
    if (value <= 0 || value > max_value) {
        throw std::invalid_argument(
            std::string(name) + " must be in [1, " + std::to_string(max_value) + "]");
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: pose_engine_benchmark <engine> [image|-] "
                     "[iterations] [warmup]\n";
        return 2;
    }

    try {
        const std::string engine_path = argv[1];
        const std::string image_path = argc >= 3 ? argv[2] : "-";
        const int iterations = argc >= 4
            ? parsePositiveInt(argv[3], "iterations", 100000) : 200;
        const int warmup = argc >= 5
            ? parsePositiveInt(argv[4], "warmup", 10000) : 20;

        cv::Mat image;
        std::string input_kind;
        if (image_path.empty() || image_path == "-") {
            image = makeSyntheticImage();
            input_kind = "synthetic";
        } else {
            image = cv::imread(image_path, cv::IMREAD_COLOR);
            input_kind = "file";
            if (image.empty()) {
                std::cerr << "FAIL: could not read image: " << image_path << '\n';
                return 2;
            }
        }

        yolo11::PoseConfig config;
        config.engine_path = engine_path;
        config.gpu_id = 0;
        config.use_gpu_postprocess = false;

        yolo11::Yolo11PoseDetector detector;
        const auto init_start = std::chrono::steady_clock::now();
        if (!detector.init(config)) {
            std::cerr << "FAIL: TensorRT pose engine initialization failed\n";
            return 1;
        }
        const double initialization_ms = elapsedMs(init_start);

        for (int i = 0; i < warmup; ++i) {
            (void)detector.infer(image);
        }

        std::vector<Sample> samples;
        samples.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto detections = detector.infer(image);
            Sample sample;
            sample.wall_ms = elapsedMs(start);
            sample.perf = detector.lastPerf();
            sample.detections = detections.size();
            samples.push_back(sample);
        }
        detector.release();

        std::vector<double> wall;
        std::vector<double> pipeline;
        std::vector<double> preprocess;
        std::vector<double> enqueue;
        std::vector<double> d2h;
        std::vector<double> postprocess;
        std::vector<double> detection_counts;
        wall.reserve(samples.size());
        pipeline.reserve(samples.size());
        preprocess.reserve(samples.size());
        enqueue.reserve(samples.size());
        d2h.reserve(samples.size());
        postprocess.reserve(samples.size());
        detection_counts.reserve(samples.size());
        for (const auto& sample : samples) {
            wall.push_back(sample.wall_ms);
            pipeline.push_back(sample.perf.model_pipeline_ms);
            preprocess.push_back(sample.perf.preprocess_ms);
            enqueue.push_back(sample.perf.trt_enqueue_ms);
            d2h.push_back(sample.perf.d2h_ms);
            postprocess.push_back(sample.perf.postprocess_ms);
            detection_counts.push_back(static_cast<double>(sample.detections));
        }

        const double wall_mean = mean(wall);
        std::cout << std::fixed << std::setprecision(3)
                  << "PASS: pose TensorRT single-image benchmark\n"
                  << "input_kind=" << input_kind << '\n'
                  << "input_width=" << image.cols << '\n'
                  << "input_height=" << image.rows << '\n'
                  << "iterations=" << iterations << '\n'
                  << "warmup_iterations=" << warmup << '\n'
                  << "initialization_ms=" << initialization_ms << '\n'
                  << "wall_mean_ms=" << wall_mean << '\n'
                  << "wall_min_ms=" << *std::min_element(wall.begin(), wall.end()) << '\n'
                  << "wall_p50_ms=" << percentile(wall, 0.50) << '\n'
                  << "wall_p95_ms=" << percentile(wall, 0.95) << '\n'
                  << "wall_p99_ms=" << percentile(wall, 0.99) << '\n'
                  << "wall_max_ms=" << *std::max_element(wall.begin(), wall.end()) << '\n'
                  << "throughput_fps=" << (wall_mean > 0.0 ? 1000.0 / wall_mean : 0.0) << '\n'
                  << "pipeline_mean_ms=" << mean(pipeline) << '\n'
                  << "preprocess_mean_ms=" << mean(preprocess) << '\n'
                  << "trt_enqueue_mean_ms=" << mean(enqueue) << '\n'
                  << "d2h_sync_mean_ms=" << mean(d2h) << '\n'
                  << "postprocess_mean_ms=" << mean(postprocess) << '\n'
                  << "detections_mean=" << mean(detection_counts) << '\n';
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 2;
    }
}
