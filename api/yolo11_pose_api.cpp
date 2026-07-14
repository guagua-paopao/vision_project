#include "yolo11_pose_api.h"

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <cuda_runtime_api.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <iostream>
#include <vector>

#include "config.h"
#include "cuda_utils.h"
#include "logging.h"
#include "postprocess.h"
#include "preprocess.h"

namespace yolo11 {

    namespace {
        static Logger gLogger;

#ifdef _WIN32
        static HMODULE gYoloPluginHandle = nullptr;
#endif

        constexpr int kPoseOutputSize =
            kMaxNumOutputBbox * sizeof(Detection) / sizeof(float) + 1;

        bool loadTensorRtPlugins() {
#ifdef _WIN32
            if (gYoloPluginHandle == nullptr) {
                gYoloPluginHandle = LoadLibraryA("myplugins.dll");
                if (!gYoloPluginHandle) {
                    std::cerr << "Failed to load myplugins.dll, Windows error code: "
                        << GetLastError() << std::endl;
                    return false;
                }
            }
#endif
            initLibNvInferPlugins(&gLogger, "");
            return true;
        }
    }  // namespace

    Yolo11PoseDetector::Yolo11PoseDetector() = default;

    Yolo11PoseDetector::~Yolo11PoseDetector() {
        release();
    }

    bool Yolo11PoseDetector::init(const PoseConfig& config) {
        if (initialized_) {
            release();
        }

        config_ = config;
        if (config_.use_gpu_postprocess) {
            // The original demo explicitly keeps pose GPU postprocess disabled.
            // Keep the service on CPU postprocess for correctness and stability.
            std::cerr << "Pose GPU postprocess is not supported yet; fallback to CPU postprocess." << std::endl;
            config_.use_gpu_postprocess = false;
        }

        cudaError_t cuda_status = cudaSetDevice(config_.gpu_id);
        if (cuda_status != cudaSuccess) {
            std::cerr << "Failed to set CUDA device for POSE: " << cudaGetErrorString(cuda_status) << std::endl;
            return false;
        }

        if (!loadTensorRtPlugins()) {
            std::cerr << "Failed to initialize TensorRT plugins for POSE." << std::endl;
            return false;
        }

        if (!loadEngine(config_.engine_path)) {
            std::cerr << "Failed to load POSE TensorRT engine: " << config_.engine_path << std::endl;
            release();
            return false;
        }

        auto* engine = reinterpret_cast<nvinfer1::ICudaEngine*>(engine_);
        if (!engine) {
            std::cerr << "POSE engine pointer is null after loading engine." << std::endl;
            release();
            return false;
        }

        const auto out_dims = engine->getTensorShape(kOutputTensorName);
        if (out_dims.nbDims < 2) {
            std::cerr << "Invalid POSE output tensor shape." << std::endl;
            release();
            return false;
        }
        model_bboxes_ = static_cast<int>(out_dims.d[1]);

        cudaStream_t stream = nullptr;
        cuda_status = cudaStreamCreate(&stream);
        if (cuda_status != cudaSuccess) {
            std::cerr << "Failed to create CUDA stream for POSE: " << cudaGetErrorString(cuda_status) << std::endl;
            release();
            return false;
        }
        stream_ = reinterpret_cast<void*>(stream);

        cuda_preprocess_init(kMaxInputImageSize);

        if (!prepareBuffers()) {
            std::cerr << "Failed to prepare POSE buffers." << std::endl;
            release();
            return false;
        }

        initialized_ = true;
        std::cout << "Yolo11PoseDetector initialized successfully." << std::endl;
        std::cout << "kInputW = " << kInputW << ", kInputH = " << kInputH << std::endl;
        std::cout << "kPoseNumClass = " << kPoseNumClass << ", kNumberOfPoints = " << kNumberOfPoints << std::endl;
        std::cout << "model_bboxes = " << model_bboxes_ << std::endl;
        return true;
    }

    bool Yolo11PoseDetector::loadEngine(const std::string& engine_path) {
        std::ifstream file(engine_path, std::ios::binary);
        if (!file.good()) {
            std::cerr << "Cannot open POSE engine file: " << engine_path << std::endl;
            return false;
        }

        file.seekg(0, std::ifstream::end);
        const size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ifstream::beg);
        if (size == 0) {
            std::cerr << "POSE engine file is empty: " << engine_path << std::endl;
            return false;
        }

        std::vector<char> serialized_engine(size);
        file.read(serialized_engine.data(), static_cast<std::streamsize>(size));
        file.close();

        auto* runtime = nvinfer1::createInferRuntime(gLogger);
        if (!runtime) {
            std::cerr << "createInferRuntime failed for POSE." << std::endl;
            return false;
        }

        auto* engine = runtime->deserializeCudaEngine(serialized_engine.data(), size);
        if (!engine) {
            std::cerr << "deserializeCudaEngine failed for POSE." << std::endl;
            delete runtime;
            return false;
        }

        auto* context = engine->createExecutionContext();
        if (!context) {
            std::cerr << "createExecutionContext failed for POSE." << std::endl;
            delete engine;
            delete runtime;
            return false;
        }

        runtime_ = reinterpret_cast<void*>(runtime);
        engine_ = reinterpret_cast<void*>(engine);
        context_ = reinterpret_cast<void*>(context);
        return true;
    }

    bool Yolo11PoseDetector::prepareBuffers() {
        auto* engine = reinterpret_cast<nvinfer1::ICudaEngine*>(engine_);
        if (!engine) {
            std::cerr << "POSE engine is null in prepareBuffers." << std::endl;
            return false;
        }

        if (engine->getNbIOTensors() != 2) {
            std::cerr << "POSE model should have 1 input and 1 output." << std::endl;
            return false;
        }

        const auto input_mode = engine->getTensorIOMode(kInputTensorName);
        if (input_mode != nvinfer1::TensorIOMode::kINPUT) {
            std::cerr << kInputTensorName << " is not POSE input tensor." << std::endl;
            return false;
        }

        const auto output_mode = engine->getTensorIOMode(kOutputTensorName);
        if (output_mode != nvinfer1::TensorIOMode::kOUTPUT) {
            std::cerr << kOutputTensorName << " is not POSE output tensor." << std::endl;
            return false;
        }

        CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void**>(&input_device_),
            kBatchSize * 3 * kInputH * kInputW * sizeof(float)
        ));

        CUDA_CHECK(cudaMalloc(
            reinterpret_cast<void**>(&output_device_),
            kBatchSize * kPoseOutputSize * sizeof(float)
        ));

        CUDA_CHECK(cudaMallocHost(reinterpret_cast<void**>(&output_host_),
            kBatchSize * kPoseOutputSize * sizeof(float)));
        return true;
    }

    std::vector<Detection> Yolo11PoseDetector::infer(const cv::Mat& image) {
        std::vector<cv::Mat> batch;
        batch.push_back(image);
        auto batch_results = inferBatch(batch);
        if (batch_results.empty()) {
            return {};
        }
        return batch_results[0];
    }

    PerfMetrics Yolo11PoseDetector::lastPerf() const {
        return last_perf_;
    }

    std::vector<std::vector<Detection>> Yolo11PoseDetector::inferBatch(const std::vector<cv::Mat>& images) {
        std::vector<std::vector<Detection>> results;
        last_perf_ = PerfMetrics{};
        const auto pipeline_start = PerfClock::now();
        if (!initialized_) {
            std::cerr << "POSE detector is not initialized." << std::endl;
            return results;
        }
        if (images.empty()) {
            return results;
        }
        if (static_cast<int>(images.size()) > kBatchSize) {
            std::cerr << "POSE inferBatch images.size() exceeds kBatchSize." << std::endl;
            return results;
        }
        for (const auto& image : images) {
            if (image.empty()) {
                std::cerr << "POSE inferBatch received empty image." << std::endl;
                return results;
            }
        }

        auto* context = reinterpret_cast<nvinfer1::IExecutionContext*>(context_);
        auto stream = reinterpret_cast<cudaStream_t>(stream_);
        if (!context || !stream || !input_device_ || !output_device_ || !output_host_) {
            std::cerr << "POSE TensorRT resources are incomplete." << std::endl;
            return results;
        }

        std::vector<cv::Mat> img_batch = images;
        while (img_batch.size() < kBatchSize) {
            img_batch.push_back(cv::Mat::zeros(images[0].size(), images[0].type()));
        }

        auto stage_start = PerfClock::now();
        cuda_batch_preprocess(img_batch, input_device_, kInputW, kInputH, stream);
        last_perf_.preprocess_ms = elapsedMs(stage_start);

        context->setInputTensorAddress(kInputTensorName, input_device_);
        context->setOutputTensorAddress(kOutputTensorName, output_device_);
        stage_start = PerfClock::now();
        const bool enqueue_ok = context->enqueueV3(stream);
        last_perf_.trt_enqueue_ms = elapsedMs(stage_start);
        if (!enqueue_ok) {
            std::cerr << "POSE enqueueV3 failed." << std::endl;
            return results;
        }

        stage_start = PerfClock::now();
        CUDA_CHECK(cudaMemcpyAsync(
            output_host_,
            output_device_,
            static_cast<size_t>(kBatchSize) * kPoseOutputSize * sizeof(float),
            cudaMemcpyDeviceToHost,
            stream
        ));
        CUDA_CHECK(cudaStreamSynchronize(stream));
        last_perf_.d2h_ms = elapsedMs(stage_start);

        stage_start = PerfClock::now();
        batch_nms(results, output_host_, static_cast<int>(images.size()), kPoseOutputSize, kConfThresh, kNmsThresh);
        last_perf_.postprocess_ms = elapsedMs(stage_start);
        last_perf_.model_pipeline_ms = elapsedMs(pipeline_start);
        return results;
    }

    cv::Mat Yolo11PoseDetector::draw(const cv::Mat& image, const std::vector<Detection>& detections) {
        if (image.empty()) {
            return {};
        }
        std::vector<cv::Mat> img_batch;
        img_batch.push_back(image.clone());
        std::vector<std::vector<Detection>> res_batch;
        res_batch.push_back(detections);
        draw_bbox_keypoints_line(img_batch, res_batch);
        return img_batch[0];
    }

    void Yolo11PoseDetector::release() {
        if (stream_) {
            cudaStreamDestroy(reinterpret_cast<cudaStream_t>(stream_));
            stream_ = nullptr;
        }
        if (input_device_) {
            cudaFree(input_device_);
            input_device_ = nullptr;
        }
        if (output_device_) {
            cudaFree(output_device_);
            output_device_ = nullptr;
        }
        if (output_host_) {
            cudaFreeHost(output_host_);
            output_host_ = nullptr;
        }

        if (context_) {
            delete reinterpret_cast<nvinfer1::IExecutionContext*>(context_);
            context_ = nullptr;
        }
        if (engine_) {
            delete reinterpret_cast<nvinfer1::ICudaEngine*>(engine_);
            engine_ = nullptr;
        }
        if (runtime_) {
            delete reinterpret_cast<nvinfer1::IRuntime*>(runtime_);
            runtime_ = nullptr;
        }

        if (initialized_) {
            cuda_preprocess_destroy();
        }
        initialized_ = false;
        model_bboxes_ = 0;
    }

}  // namespace yolo11
