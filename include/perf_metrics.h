#pragma once

#include <chrono>

// Lightweight per-task performance breakdown shared by model APIs and service workers.
// Values use milliseconds. A value of 0.0 means either the stage was not executed
// for this model or the stage is not split yet.
struct PerfMetrics {
    double decode_ms = 0.0;
    double redis_read_ms = 0.0;
    double preprocess_ms = 0.0;
    double trt_enqueue_ms = 0.0;
    double d2h_ms = 0.0;
    double postprocess_ms = 0.0;
    double model_pipeline_ms = 0.0;
    double draw_ms = 0.0;
    double encode_ms = 0.0;
    double redis_write_ms = 0.0;
    double result_local_write_ms = 0.0;
    double ack_ms = 0.0;
    double worker_total_ms = 0.0;
};

using PerfClock = std::chrono::steady_clock;

inline double elapsedMs(PerfClock::time_point start, PerfClock::time_point end = PerfClock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
