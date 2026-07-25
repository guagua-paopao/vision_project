#include "server/camera_run_spec.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace yolo11_server {

namespace {

using json = nlohmann::json;

json parsedObject(const std::string& value) {
    const auto parsed = json::parse(value, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

}  // namespace

CameraRunSpec::CameraRunSpec(
    std::string run_id,
    std::string task_id,
    std::string origin,
    std::string camera_profile,
    int definition_version,
    CameraRunFrameOutputSpec frame_output,
    CameraRunAnalysisSpec analysis,
    std::string callback_profile,
    long long create_time_ms,
    CameraRunCompatibilitySpec compatibility
) : run_id_(std::move(run_id)),
    task_id_(std::move(task_id)),
    origin_(std::move(origin)),
    camera_profile_(std::move(camera_profile)),
    definition_version_(definition_version),
    frame_output_(std::move(frame_output)),
    analysis_(std::move(analysis)),
    callback_profile_(std::move(callback_profile)),
    create_time_ms_(create_time_ms),
    compatibility_(std::move(compatibility)) {
}

std::string CameraRunSpec::toDefinitionJson() const {
    // The original flat keys remain present for rollback readers. The new
    // canonical metadata and nested frame_output section are additive.
    return json({
        {"run_id", run_id_},
        {"camera_id", task_id_},
        {"origin", origin_},
        {"camera_profile", camera_profile_},
        {"definition_version", definition_version_},
        {"frame_interval_ms", frame_output_.interval_ms},
        {"output_mode", frame_output_.mode},
        {"jpeg_quality", frame_output_.jpeg_quality},
        {"max_width", frame_output_.max_width},
        {"max_height", frame_output_.max_height},
        {"retention_days", frame_output_.retention_days},
        {"max_saved_frames", frame_output_.max_saved_frames},
        {"desired_state", "running"},
        {"frame_output", {
            {"enabled", frame_output_.enabled},
            {"interval_ms", frame_output_.interval_ms},
            {"mode", frame_output_.mode},
            {"jpeg_quality", frame_output_.jpeg_quality},
            {"max_width", frame_output_.max_width},
            {"max_height", frame_output_.max_height},
            {"retention", {
                {"days", frame_output_.retention_days},
                {"max_saved_frames", frame_output_.max_saved_frames}
            }}
        }},
        {"analysis", {
            {"enabled", analysis_.enabled},
            {"target_infer_fps", analysis_.target_infer_fps},
            {"algorithm_profile", analysis_.algorithm_profile},
            {"algorithms", analysis_.algorithms},
            {"config_version", analysis_.config_version},
            {"initial_occupancy", analysis_.initial_occupancy},
            {"snapshot_fps", analysis_.snapshot_fps},
            {"algorithm_parameters", parsedObject(analysis_.algorithm_parameters_json)}
        }},
        {"callback_profile", callback_profile_},
        {"create_time_ms", create_time_ms_},
        {"compatibility", {
            {"legacy_session_id", compatibility_.legacy_session_id},
            {"preserve_pf_projection", compatibility_.preserve_pf_projection},
            {"legacy_response_version", compatibility_.legacy_response_version}
        }}
    }).dump();
}

CameraTaskRunRecord CameraRunSpec::toRunRecord() const {
    CameraTaskRunRecord run;
    run.run_id = run_id_;
    run.task_id = task_id_;
    run.definition_version = definition_version_;
    run.definition_json = toDefinitionJson();
    run.status = "queued";
    run.camera_profile = camera_profile_;
    run.create_time_ms = create_time_ms_;
    run.last_update_ms = create_time_ms_;
    run.origin = origin_;
    run.legacy_session_id = compatibility_.legacy_session_id;
    run.analysis_config_version = analysis_.config_version;
    return run;
}

CameraTaskCommand CameraRunSpec::toStartCommand() const {
    CameraTaskCommand command;
    command.task_id = task_id_;
    command.run_id = run_id_;
    command.camera_profile = camera_profile_;
    command.definition_version = definition_version_;
    command.frame_interval_ms = frame_output_.interval_ms;
    command.output_mode = frame_output_.mode;
    command.jpeg_quality = frame_output_.jpeg_quality;
    command.max_width = frame_output_.max_width;
    command.max_height = frame_output_.max_height;
    command.retention_days = frame_output_.retention_days;
    command.max_saved_frames = frame_output_.max_saved_frames;
    command.analysis_enabled = analysis_.enabled;
    command.target_infer_fps = analysis_.target_infer_fps;
    command.algorithm_profile = analysis_.algorithm_profile;
    command.algorithms = analysis_.algorithms;
    command.callback_profile = callback_profile_;
    command.create_time_ms = create_time_ms_;
    command.origin = origin_;
    command.analysis_config_version = analysis_.config_version;
    command.initial_occupancy = analysis_.initial_occupancy;
    command.snapshot_fps = analysis_.snapshot_fps;
    command.algorithm_parameters_json = analysis_.algorithm_parameters_json;
    command.legacy_session_id = compatibility_.legacy_session_id;
    command.preserve_pf_projection = compatibility_.preserve_pf_projection;
    command.legacy_response_version = compatibility_.legacy_response_version;
    return command;
}

CameraRunSpec makeCameraRunSpec(
    const CameraTaskDefinition& definition,
    std::string run_id,
    long long create_time_ms,
    CameraRunSpecOptions options
) {
    CameraRunFrameOutputSpec frame_output;
    frame_output.enabled = true;
    frame_output.interval_ms = definition.frame_interval_ms;
    frame_output.mode = definition.output_mode;
    frame_output.jpeg_quality = definition.jpeg_quality;
    frame_output.max_width = definition.max_width;
    frame_output.max_height = definition.max_height;
    frame_output.retention_days = definition.retention_days;
    frame_output.max_saved_frames = definition.max_saved_frames;

    CameraRunAnalysisSpec analysis;
    analysis.enabled = definition.analysis_enabled;
    analysis.target_infer_fps = definition.target_infer_fps;
    analysis.algorithm_profile = definition.algorithm_profile;
    analysis.algorithms = definition.algorithms;
    analysis.config_version = std::move(options.analysis_config_version);
    analysis.initial_occupancy = std::max(0LL, options.initial_occupancy);
    analysis.snapshot_fps = std::max(0, options.snapshot_fps);
    analysis.algorithm_parameters_json =
        parsedObject(options.algorithm_parameters_json).dump();

    return CameraRunSpec(
        std::move(run_id),
        definition.task_id,
        std::move(options.origin),
        definition.camera_profile,
        definition.version,
        std::move(frame_output),
        std::move(analysis),
        definition.callback_profile,
        create_time_ms,
        std::move(options.compatibility));
}

}  // namespace yolo11_server
