#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/camera_run_spec.h"

namespace {

using namespace yolo11_server;
using nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    static_assert(std::is_copy_constructible_v<CameraRunSpec>);
    static_assert(!std::is_copy_assignable_v<CameraRunSpec>);
    static_assert(!std::is_move_assignable_v<CameraRunSpec>);

    CameraTaskDefinition definition;
    definition.task_id = "entrance_01";
    definition.camera_profile = "entry_camera_01";
    definition.frame_interval_ms = 1250;
    definition.output_mode = "both";
    definition.jpeg_quality = 88;
    definition.max_width = 1280;
    definition.max_height = 720;
    definition.retention_days = 14;
    definition.max_saved_frames = 5000;
    definition.analysis_enabled = true;
    definition.target_infer_fps = 6.5;
    definition.algorithm_profile = "security_default";
    definition.algorithms = { "people_flow", "electronic_fence" };
    definition.callback_profile = "backend_primary";
    definition.version = 7;

    CameraRunSpecOptions options;
    options.analysis_config_version = "entry-line-v3";
    options.initial_occupancy = 12;
    options.snapshot_fps = 2;
    options.algorithm_parameters_json = R"({"line_id":"main_entry"})";

    const auto spec =
        makeCameraRunSpec(definition, "cr_immutable_01", 1774412345000LL, options);
    definition.frame_interval_ms = 9999;
    definition.algorithms.clear();
    options.initial_occupancy = 999;

    require(spec.runId() == "cr_immutable_01" &&
            spec.taskId() == "entrance_01" &&
            spec.origin() == kCameraRunOriginCameraApi &&
            spec.definitionVersion() == 7,
        "RunSpec identity must be captured at construction");
    require(spec.frameOutput().interval_ms == 1250 &&
            spec.analysis().algorithms ==
                std::vector<std::string>({ "people_flow", "electronic_fence" }) &&
            spec.analysis().initial_occupancy == 12,
        "RunSpec must not observe later Definition or options changes");

    const auto document = json::parse(spec.toDefinitionJson());
    require(document["camera_profile"] == "entry_camera_01" &&
            document["frame_interval_ms"] == 1250 &&
            document["frame_output"]["retention"]["days"] == 14 &&
            document["analysis"]["config_version"] == "entry-line-v3" &&
            document["analysis"]["initial_occupancy"] == 12 &&
            document["analysis"]["algorithm_parameters"]["line_id"] == "main_entry" &&
            document["compatibility"]["legacy_session_id"] == "",
        "serialized RunSpec must contain legacy flat fields and canonical immutable fields");

    const auto run = spec.toRunRecord();
    require(run.origin == "camera_api" &&
            run.analysis_config_version == "entry-line-v3" &&
            run.definition_json == spec.toDefinitionJson(),
        "Run persistence projection must retain immutable metadata");

    const auto command = spec.toStartCommand();
    require(command.origin == "camera_api" &&
            command.analysis_config_version == "entry-line-v3" &&
            command.initial_occupancy == 12 &&
            command.snapshot_fps == 2 &&
            command.algorithm_parameters_json == R"({"line_id":"main_entry"})",
        "command projection must carry the complete additive RunSpec contract");

    std::cout << "CameraRunSpec immutable mapping tests passed\n";
    return 0;
}
