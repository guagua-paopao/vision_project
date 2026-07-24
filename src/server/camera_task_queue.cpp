#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include "server/camera_task_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include <hiredis/hiredis.h>

namespace yolo11_server {

namespace {

using json = nlohmann::json;

struct ReplyCloser {
    void operator()(redisReply* reply) const { if (reply) freeReplyObject(reply); }
};
using ReplyPtr = std::unique_ptr<redisReply, ReplyCloser>;

std::string replyString(const redisReply* reply) {
    return reply && reply->str ? std::string(reply->str, reply->len) : std::string{};
}

long long parseLongLong(const std::string& value, long long fallback = 0) {
    try { return value.empty() ? fallback : std::stoll(value); }
    catch (...) { return fallback; }
}

double parseDouble(const std::string& value, double fallback = 0.0) {
    try { return value.empty() ? fallback : std::stod(value); }
    catch (...) { return fallback; }
}

std::string redisError(redisContext* context, const std::string& fallback) {
    if (context && context->errstr && std::strlen(context->errstr) > 0) return context->errstr;
    return fallback;
}

bool replyError(redisReply* reply, redisContext* context, std::string& error) {
    if (!reply) {
        error = redisError(context, "empty Redis reply");
        return true;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        error = replyString(reply);
        return true;
    }
    return false;
}

bool safeKeyPart(const std::string& value) {
    if (value.empty() || value.size() > 160) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

std::map<std::string, std::string> fields(redisReply* reply) {
    std::map<std::string, std::string> result;
    if (!reply || reply->type != REDIS_REPLY_ARRAY) return result;
    for (std::size_t index = 0; index + 1 < reply->elements; index += 2) {
        result[replyString(reply->element[index])] = replyString(reply->element[index + 1]);
    }
    return result;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string makeLeaseOwnerToken(const std::string& consumer_name) {
    std::random_device random;
    std::ostringstream value;
    value << consumer_name << ':' << std::hex
          << static_cast<unsigned long long>(random()) << ':'
          << static_cast<unsigned long long>(random()) << ':'
          << static_cast<unsigned long long>(nowMs());
    return value.str();
}

std::string sanitizedCameraError(std::string value) {
    if (value.find("://") != std::string::npos || value.find('@') != std::string::npos ||
        value.find("password") != std::string::npos || value.find("Password") != std::string::npos) {
        return "camera capture error";
    }
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > 256) value.resize(256);
    return value;
}

}  // namespace

CameraTaskQueue::CameraTaskQueue(
    const RedisSection& redis_config,
    const CameraTasksSection& camera_config,
    std::string consumer_name
) : redis_config_(redis_config),
    camera_config_(camera_config),
    consumer_name_(std::move(consumer_name)),
    lease_owner_token_(makeLeaseOwnerToken(consumer_name_)) {
}

CameraTaskQueue::~CameraTaskQueue() noexcept {
    interrupt();
    std::lock_guard<std::mutex> lock(mutex_);
    disconnectLocked();
}

bool CameraTaskQueue::connectLocked(std::string& error) {
    disconnectLocked();
    timeval timeout{};
    timeout.tv_sec = 5;
    context_ = redisConnectWithTimeout(redis_config_.host.c_str(), redis_config_.port, timeout);
    if (!context_ || context_->err) {
        error = redisError(context_, "Redis connection failed");
        disconnectLocked();
        return false;
    }
    timeval command_timeout{};
    const int timeout_ms = std::max(1000, redis_config_.block_ms + 2000);
    command_timeout.tv_sec = timeout_ms / 1000;
    command_timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (redisSetTimeout(context_, command_timeout) != REDIS_OK) {
        error = redisError(context_, "Redis command timeout setup failed");
        disconnectLocked();
        return false;
    }
    if (!redis_config_.password.empty()) {
        ReplyPtr auth(static_cast<redisReply*>(redisCommand(
            context_, "AUTH %s", redis_config_.password.c_str())));
        if (replyError(auth.get(), context_, error)) {
            disconnectLocked();
            return false;
        }
    }
    ReplyPtr select(static_cast<redisReply*>(redisCommand(context_, "SELECT %d", redis_config_.db)));
    if (replyError(select.get(), context_, error)) {
        disconnectLocked();
        return false;
    }
    return true;
}

void CameraTaskQueue::disconnectLocked() noexcept {
    if (context_) redisFree(context_);
    context_ = nullptr;
}

bool CameraTaskQueue::ensureGroupLocked(std::string& error) {
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "XGROUP CREATE %s %s 0 MKSTREAM",
        camera_config_.command_stream_key.c_str(), camera_config_.consumer_group.c_str())));
    if (!reply) {
        error = redisError(context_, "camera command group creation failed");
        return false;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        const std::string message = replyString(reply.get());
        if (message.find("BUSYGROUP") != std::string::npos) return true;
        error = message;
        return false;
    }
    return true;
}

bool CameraTaskQueue::start(std::string& error) {
    interrupted_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    return connectLocked(error) && ensureGroupLocked(error);
}

bool CameraTaskQueue::fillCommand(void* opaque, CameraTaskCommand& command, std::string& error) const {
    auto* entry = static_cast<redisReply*>(opaque);
    if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->elements < 2 ||
        !entry->element[1] || entry->element[1]->type != REDIS_REPLY_ARRAY) {
        error = "invalid camera command stream entry";
        return false;
    }
    const auto values = fields(entry->element[1]);
    auto get = [&values](const std::string& key) {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };
    command = CameraTaskCommand{};
    command.message_id = replyString(entry->element[0]);
    command.kind = get("command_kind") == "camera_frame_stop"
        ? CameraTaskCommandKind::stop : CameraTaskCommandKind::start;
    command.task_id = get("task_id");
    command.run_id = get("run_id");
    command.camera_profile = get("camera_profile");
    command.definition_version = static_cast<int>(parseLongLong(get("definition_version")));
    command.frame_interval_ms = static_cast<int>(parseLongLong(get("frame_interval_ms"), 1000));
    command.output_mode = get("output_mode");
    command.jpeg_quality = static_cast<int>(parseLongLong(get("jpeg_quality"), 90));
    command.max_width = static_cast<int>(parseLongLong(get("max_width")));
    command.max_height = static_cast<int>(parseLongLong(get("max_height")));
    command.retention_days = static_cast<int>(parseLongLong(get("retention_days"), 7));
    command.max_saved_frames = static_cast<int>(parseLongLong(get("max_saved_frames"), 100000));
    command.analysis_enabled = parseLongLong(get("analysis_enabled")) != 0;
    command.target_infer_fps = parseDouble(get("target_infer_fps"), 5.0);
    command.algorithm_profile = get("algorithm_profile");
    const auto algorithms = json::parse(get("algorithms_json"), nullptr, false);
    if (algorithms.is_array()) {
        for (const auto& item : algorithms) {
            if (item.is_string()) command.algorithms.push_back(item.get<std::string>());
        }
    }
    command.callback_profile = get("callback_profile");
    command.create_time_ms = parseLongLong(get("create_time_ms"));
    return !command.message_id.empty();
}

bool CameraTaskQueue::claimPendingLocked(
    CameraTaskCommand& command,
    bool& claimed,
    std::string& error
) {
    claimed = false;
    const long long minimum_idle_ms = std::max<long long>(
        redis_config_.pending_min_idle_ms,
        static_cast<long long>(camera_config_.lease_ttl_seconds) * 1000LL);
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "XAUTOCLAIM %s %s %s %lld %s COUNT 1",
        camera_config_.command_stream_key.c_str(), camera_config_.consumer_group.c_str(),
        consumer_name_.c_str(), minimum_idle_ms, claim_cursor_.c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
        error = "invalid XAUTOCLAIM reply";
        return false;
    }
    claim_cursor_ = replyString(reply->element[0]);
    redisReply* entries = reply->element[1];
    if (!entries || entries->type != REDIS_REPLY_ARRAY || entries->elements == 0) return true;
    claimed = fillCommand(entries->element[0], command, error);
    return claimed;
}

bool CameraTaskQueue::readNewLocked(
    CameraTaskCommand& command,
    bool& received,
    std::string& error
) {
    received = false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "XREADGROUP GROUP %s %s COUNT 1 BLOCK %d STREAMS %s >",
        camera_config_.consumer_group.c_str(), consumer_name_.c_str(),
        std::max(100, redis_config_.block_ms), camera_config_.command_stream_key.c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    if (reply->type == REDIS_REPLY_NIL) return true;
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) return true;
    redisReply* stream = reply->element[0];
    if (!stream || stream->type != REDIS_REPLY_ARRAY || stream->elements < 2) {
        error = "invalid XREADGROUP stream reply";
        return false;
    }
    redisReply* entries = stream->element[1];
    if (!entries || entries->type != REDIS_REPLY_ARRAY || entries->elements == 0) return true;
    received = fillCommand(entries->element[0], command, error);
    return received;
}

bool CameraTaskQueue::poll(CameraTaskCommand& command, std::string& error) {
    error.clear();
    if (interrupted_.load()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && (!connectLocked(error) || !ensureGroupLocked(error))) return false;
    bool claimed = false;
    if (!claimPendingLocked(command, claimed, error)) {
        disconnectLocked();
        return false;
    }
    if (claimed) return true;
    bool received = false;
    if (!readNewLocked(command, received, error)) {
        disconnectLocked();
        return false;
    }
    return received;
}

bool CameraTaskQueue::acknowledge(const std::string& message_id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "XACK %s %s %s", camera_config_.command_stream_key.c_str(),
        camera_config_.consumer_group.c_str(), message_id.c_str())));
    return !replyError(reply.get(), context_, error);
}

void CameraTaskQueue::interrupt() noexcept {
    interrupted_.store(true);
}

bool CameraTaskQueue::submitStart(const CameraTaskCommand& command, std::string& error) {
    if (!safeKeyPart(command.task_id) || !safeKeyPart(command.run_id) ||
        !safeKeyPart(command.camera_profile)) {
        error = "camera command contains an unsafe identifier";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && (!connectLocked(error) || !ensureGroupLocked(error))) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_,
        "XADD %s * command_kind camera_frame_start command_version 2 task_id %s run_id %s "
        "definition_version %d camera_profile %s frame_interval_ms %d output_mode %s jpeg_quality %d "
        "max_width %d max_height %d retention_days %d max_saved_frames %d analysis_enabled %d "
        "target_infer_fps %.8g algorithm_profile %s algorithms_json %s callback_profile %s create_time_ms %lld",
        camera_config_.command_stream_key.c_str(), command.task_id.c_str(), command.run_id.c_str(),
        command.definition_version, command.camera_profile.c_str(), command.frame_interval_ms,
        command.output_mode.c_str(), command.jpeg_quality, command.max_width, command.max_height,
        command.retention_days, command.max_saved_frames, command.analysis_enabled ? 1 : 0,
        command.target_infer_fps, command.algorithm_profile.c_str(),
        json(command.algorithms).dump().c_str(), command.callback_profile.c_str(),
        command.create_time_ms > 0 ? command.create_time_ms : nowMs())));
    return !replyError(reply.get(), context_, error);
}

std::string CameraTaskQueue::activeKey(const std::string& task_id) const {
    return "yolo:camera-task:" + task_id + ":active";
}

std::string CameraTaskQueue::stopKey(const std::string& run_id) const {
    return "yolo:camera-task:run:" + run_id + ":stop";
}

std::string CameraTaskQueue::statusKey(const std::string& run_id) const {
    return "yolo:camera-task:run:" + run_id + ":status";
}

std::string CameraTaskQueue::hubStatusKey(const std::string& camera_profile) const {
    return "yolo:camera-hub:" + camera_profile + ":status";
}

std::string CameraTaskQueue::leaseValue(const std::string& run_id) const {
    return run_id + "|" + lease_owner_token_;
}

bool CameraTaskQueue::acquireRunLease(
    const std::string& task_id,
    const std::string& run_id,
    std::string& error
) {
    if (!safeKeyPart(task_id) || !safeKeyPart(run_id)) { error = "unsafe lease identifier"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    static const char* script =
        "local current=redis.call('GET',KEYS[1]); "
        "if not current then redis.call('SET',KEYS[1],ARGV[1],'EX',ARGV[2]); return 1; "
        "elseif current==ARGV[1] then redis.call('EXPIRE',KEYS[1],ARGV[2]); return 2; else return 0 end";
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "EVAL %s 1 %s %s %d", script, activeKey(task_id).c_str(),
        leaseValue(run_id).c_str(),
        camera_config_.lease_ttl_seconds)));
    if (replyError(reply.get(), context_, error)) return false;
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer == 0) {
        error = "camera run lease is already held";
        return false;
    }
    return true;
}

bool CameraTaskQueue::refreshRunLease(
    const std::string& task_id,
    const std::string& run_id,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    static const char* script =
        "if redis.call('GET',KEYS[1])==ARGV[1] then return redis.call('EXPIRE',KEYS[1],ARGV[2]) else return 0 end";
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "EVAL %s 1 %s %s %d", script, activeKey(task_id).c_str(),
        leaseValue(run_id).c_str(), camera_config_.lease_ttl_seconds)));
    if (replyError(reply.get(), context_, error)) return false;
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer != 1) {
        error = "camera run lease lost";
        return false;
    }
    return true;
}

bool CameraTaskQueue::releaseRunLease(
    const std::string& task_id,
    const std::string& run_id,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    static const char* script =
        "if redis.call('GET',KEYS[1])==ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end";
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "EVAL %s 1 %s %s", script, activeKey(task_id).c_str(),
        leaseValue(run_id).c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer != 1) {
        error = "camera run lease is not owned by this Worker instance";
        return false;
    }
    return true;
}

bool CameraTaskQueue::requestStop(const std::string& run_id, std::string& error) {
    if (!safeKeyPart(run_id)) { error = "unsafe run identifier"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "SET %s 1 EX %d", stopKey(run_id).c_str(), camera_config_.status_ttl_seconds)));
    return !replyError(reply.get(), context_, error);
}

bool CameraTaskQueue::isStopRequested(
    const std::string& run_id,
    bool& requested,
    std::string& error
) {
    requested = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "EXISTS %s", stopKey(run_id).c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    requested = reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
    return true;
}

bool CameraTaskQueue::updateRunStatus(const CameraTaskRunHotStatus& status, std::string& error) {
    if (!safeKeyPart(status.run_id)) { error = "unsafe run identifier"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    const std::string key = statusKey(status.run_id);
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_,
        "HSET %s run_id %s task_id %s status %s camera_profile %s hub_instance_id %s hub_state %s "
        "capture_backend %s error_code %s error_message %s last_update_ms %lld latest_frame_age_ms %lld "
        "pipeline_thread_running %d pipeline_started_at_ms %lld sample_fps %.6f sampled_frames %lld "
        "inference_submit_drops %lld "
        "save_fps %.6f consumed_frames %lld saved_frames %lld skipped_frames %lld dropped_frames %lld "
        "last_source_sequence %llu last_frame_time_ms %lld writer_queue_depth %d",
        key.c_str(), status.run_id.c_str(), status.task_id.c_str(), status.status.c_str(),
        status.camera_profile.c_str(), status.hub_instance_id.c_str(), status.hub_state.c_str(),
        status.capture_backend.c_str(), status.error_code.c_str(), status.error_message.c_str(),
        status.last_update_ms, status.latest_frame_age_ms,
        status.pipeline_thread_running ? 1 : 0, status.pipeline_started_at_ms,
        status.sample_fps, status.sampled_frames, status.inference_submit_drops,
        status.save_fps, status.consumed_frames, status.saved_frames,
        status.skipped_frames, status.dropped_frames, status.last_source_sequence,
        status.last_frame_time_ms, status.writer_queue_depth)));
    if (replyError(reply.get(), context_, error)) return false;
    ReplyPtr expire(static_cast<redisReply*>(redisCommand(
        context_, "EXPIRE %s %d", key.c_str(), camera_config_.status_ttl_seconds)));
    return !replyError(expire.get(), context_, error);
}

bool CameraTaskQueue::getRunStatus(
    const std::string& run_id,
    CameraTaskRunHotStatus& status,
    std::string& error
) {
    status = CameraTaskRunHotStatus{};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "HGETALL %s", statusKey(run_id).c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    const auto values = fields(reply.get());
    if (values.empty()) return true;
    auto get = [&values](const std::string& key) {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };
    status.found = true;
    status.run_id = get("run_id");
    status.task_id = get("task_id");
    status.status = get("status");
    status.camera_profile = get("camera_profile");
    status.hub_instance_id = get("hub_instance_id");
    status.hub_state = get("hub_state");
    status.capture_backend = get("capture_backend");
    status.error_code = get("error_code");
    status.error_message = get("error_message");
    status.last_update_ms = parseLongLong(get("last_update_ms"));
    status.latest_frame_age_ms = parseLongLong(get("latest_frame_age_ms"), -1);
    status.pipeline_thread_running = parseLongLong(get("pipeline_thread_running")) != 0;
    status.pipeline_started_at_ms = parseLongLong(get("pipeline_started_at_ms"));
    status.sample_fps = parseDouble(get("sample_fps"));
    status.sampled_frames = parseLongLong(get("sampled_frames"));
    status.inference_submit_drops = parseLongLong(get("inference_submit_drops"));
    try { status.save_fps = std::stod(get("save_fps")); } catch (...) {}
    status.consumed_frames = parseLongLong(get("consumed_frames"));
    status.saved_frames = parseLongLong(get("saved_frames"));
    status.skipped_frames = parseLongLong(get("skipped_frames"));
    status.dropped_frames = parseLongLong(get("dropped_frames"));
    status.last_source_sequence = static_cast<unsigned long long>(parseLongLong(get("last_source_sequence")));
    status.last_frame_time_ms = parseLongLong(get("last_frame_time_ms"));
    status.writer_queue_depth = static_cast<int>(parseLongLong(get("writer_queue_depth")));
    return true;
}

bool CameraTaskQueue::updateHubStatus(const CameraHubStatus& status, std::string& error) {
    if (!safeKeyPart(status.camera_profile)) { error = "unsafe camera profile identifier"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    const std::string key = hubStatusKey(status.camera_profile);
    const int people_flow_subscribers = status.subscriber_types.count("people_flow")
        ? status.subscriber_types.at("people_flow") : 0;
    const int camera_task_subscribers = status.subscriber_types.count("camera_task")
        ? status.subscriber_types.at("camera_task") : 0;
    const std::string clean_error = sanitizedCameraError(status.last_error);
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_,
        "HSET %s camera_profile %s hub_instance_id %s state %s backend %s subscriber_count %d "
        "people_flow_subscribers %d camera_task_subscribers %d open_count %lld reconnect_count %d "
        "capture_fps %.6f source_fps %.6f latest_frame_age_ms %lld latest_sequence %llu width %d height %d "
        "resolution_changed %d resolution_change_count %lld last_frame_time_ms %lld "
        "last_error %s last_update_ms %lld",
        key.c_str(), status.camera_profile.c_str(), status.hub_instance_id.c_str(), status.state.c_str(),
        status.backend_name.c_str(), status.subscriber_count, people_flow_subscribers,
        camera_task_subscribers, status.open_count, status.reconnect_count, status.capture_fps,
        status.source_fps, status.latest_frame_age_ms, status.latest_sequence, status.width, status.height,
        status.resolution_changed ? 1 : 0, status.resolution_change_count, status.last_frame_time_ms,
        clean_error.c_str(), nowMs())));
    if (replyError(reply.get(), context_, error)) return false;
    ReplyPtr expire(static_cast<redisReply*>(redisCommand(
        context_, "EXPIRE %s %d", key.c_str(), std::max(5, camera_config_.lease_ttl_seconds * 2))));
    return !replyError(expire.get(), context_, error);
}

bool CameraTaskQueue::getHubStatus(
    const std::string& camera_profile,
    CameraHubHotStatus& status,
    std::string& error
) {
    status = CameraHubHotStatus{};
    if (!safeKeyPart(camera_profile)) { error = "unsafe camera profile identifier"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ && !connectLocked(error)) return false;
    ReplyPtr reply(static_cast<redisReply*>(redisCommand(
        context_, "HGETALL %s", hubStatusKey(camera_profile).c_str())));
    if (replyError(reply.get(), context_, error)) return false;
    const auto values = fields(reply.get());
    if (values.empty()) return true;
    auto get = [&values](const std::string& key) {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };
    status.found = true;
    status.snapshot.camera_profile = get("camera_profile");
    status.snapshot.hub_instance_id = get("hub_instance_id");
    status.snapshot.state = get("state");
    status.snapshot.backend_name = get("backend");
    status.snapshot.subscriber_count = static_cast<int>(parseLongLong(get("subscriber_count")));
    status.snapshot.subscriber_types["people_flow"] =
        static_cast<int>(parseLongLong(get("people_flow_subscribers")));
    status.snapshot.subscriber_types["camera_task"] =
        static_cast<int>(parseLongLong(get("camera_task_subscribers")));
    status.snapshot.open_count = parseLongLong(get("open_count"));
    status.snapshot.reconnect_count = static_cast<int>(parseLongLong(get("reconnect_count")));
    try { status.snapshot.capture_fps = std::stod(get("capture_fps")); } catch (...) {}
    try { status.snapshot.source_fps = std::stod(get("source_fps")); } catch (...) {}
    status.snapshot.latest_frame_age_ms = parseLongLong(get("latest_frame_age_ms"), -1);
    status.snapshot.latest_sequence =
        static_cast<unsigned long long>(parseLongLong(get("latest_sequence")));
    status.snapshot.width = static_cast<int>(parseLongLong(get("width")));
    status.snapshot.height = static_cast<int>(parseLongLong(get("height")));
    status.snapshot.resolution_changed = parseLongLong(get("resolution_changed")) != 0;
    status.snapshot.resolution_change_count = parseLongLong(get("resolution_change_count"));
    status.snapshot.last_frame_time_ms = parseLongLong(get("last_frame_time_ms"));
    status.snapshot.last_error = sanitizedCameraError(get("last_error"));
    status.last_update_ms = parseLongLong(get("last_update_ms"));
    return true;
}

}  // namespace yolo11_server
