#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "server/redis_task_queue.h"
#include "server/uri_masker.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <hiredis/hiredis.h>

namespace yolo11_server {

    namespace {

        struct RedisContextDeleter {
            void operator()(redisContext* ctx) const {
                if (ctx != nullptr) {
                    redisFree(ctx);
                }
            }
        };

        struct RedisReplyDeleter {
            void operator()(redisReply* reply) const {
                if (reply != nullptr) {
                    freeReplyObject(reply);
                }
            }
        };

        using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
        using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

        std::string replyString(const redisReply* reply) {
            if (reply == nullptr || reply->str == nullptr || reply->len == 0) {
                return {};
            }
            return std::string(reply->str, reply->len);
        }

        std::string contextError(const redisContext* context, const std::string& fallback) {
            if (context != nullptr && context->errstr != nullptr && std::strlen(context->errstr) > 0) {
                return context->errstr;
            }
            return fallback;
        }

        bool replyIsError(const redisReply* reply, std::string& error, const redisContext* context) {
            if (reply == nullptr) {
                error = contextError(context, "empty redis reply");
                return true;
            }
            if (reply->type == REDIS_REPLY_ERROR) {
                error = replyString(reply);
                if (error.empty()) {
                    error = "redis command error";
                }
                return true;
            }
            return false;
        }

        bool containsText(const std::string& text, const std::string& token) {
            return text.find(token) != std::string::npos;
        }

        long long parseLongLong(const std::string& value, long long default_value = 0) {
            if (value.empty()) {
                return default_value;
            }
            try {
                return std::stoll(value);
            }
            catch (...) {
                return default_value;
            }
        }

        double parseDouble(const std::string& value, double default_value = 0.0) {
            if (value.empty()) {
                return default_value;
            }
            try {
                return std::stod(value);
            }
            catch (...) {
                return default_value;
            }
        }

        long long nowMs() {
            auto now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        }

        std::map<std::string, std::string> parseInfoText(const std::string& text) {
            std::map<std::string, std::string> values;
            std::istringstream iss(text);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                const auto pos = line.find(':');
                if (pos == std::string::npos) {
                    continue;
                }
                values[line.substr(0, pos)] = line.substr(pos + 1);
            }
            return values;
        }

        std::map<std::string, std::string> parseHashReply(const redisReply* reply) {
            std::map<std::string, std::string> values;
            if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
                return values;
            }
            for (size_t i = 0; i + 1 < reply->elements; i += 2) {
                const std::string key = replyString(reply->element[i]);
                const std::string value = replyString(reply->element[i + 1]);
                values[key] = value;
            }
            return values;
        }

        bool setCommandTimeout(redisContext* context, int command_timeout_ms, std::string& error) {
            if (context == nullptr) {
                error = "redis context is null";
                return false;
            }

            command_timeout_ms = std::max(command_timeout_ms, 1000);

            struct timeval command_timeout;
            command_timeout.tv_sec = command_timeout_ms / 1000;
            command_timeout.tv_usec = (command_timeout_ms % 1000) * 1000;

            if (redisSetTimeout(context, command_timeout) != REDIS_OK) {
                error = contextError(context, "redisSetTimeout failed");
                return false;
            }
            return true;
        }

        bool connectContextOnce(
            const RedisSection& config,
            RedisContextPtr& context,
            std::string& error,
            int command_timeout_ms
        ) {
            struct timeval connect_timeout;
            connect_timeout.tv_sec = 5;
            connect_timeout.tv_usec = 0;

            redisContext* raw_context = redisConnectWithTimeout(
                config.host.c_str(),
                config.port,
                connect_timeout
            );

            if (raw_context == nullptr) {
                error = "redisConnectWithTimeout returned nullptr";
                return false;
            }

            context.reset(raw_context);

            if (context->err) {
                error = contextError(context.get(), "redis connection error");
                return false;
            }

            if (!setCommandTimeout(context.get(), command_timeout_ms, error)) {
                return false;
            }

            if (!config.password.empty()) {
                RedisReplyPtr auth_reply(static_cast<redisReply*>(
                    redisCommand(context.get(), "AUTH %s", config.password.c_str())
                    ));
                if (replyIsError(auth_reply.get(), error, context.get())) {
                    error = "AUTH failed: " + error;
                    return false;
                }
            }

            RedisReplyPtr select_reply(static_cast<redisReply*>(
                redisCommand(context.get(), "SELECT %d", config.db)
                ));
            if (replyIsError(select_reply.get(), error, context.get())) {
                error = "SELECT failed: " + error;
                return false;
            }

            return true;
        }

        bool connectContext(
            const RedisSection& config,
            RedisContextPtr& context,
            std::string& error,
            int command_timeout_ms = 10000
        ) {
            std::string last_error;

            for (int attempt = 1; attempt <= 3; ++attempt) {
                context.reset();
                error.clear();

                if (connectContextOnce(config, context, error, command_timeout_ms)) {
                    return true;
                }

                last_error = error;
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
            }

            error = last_error.empty() ? "failed to connect redis after retries" : last_error;
            return false;
        }

        bool ensureConsumerGroup(redisContext* context, const RedisSection& config, std::string& error) {
            RedisReplyPtr reply(static_cast<redisReply*>(
                redisCommand(
                    context,
                    "XGROUP CREATE %s %s 0 MKSTREAM",
                    config.stream_key.c_str(),
                    config.consumer_group.c_str()
                )
                ));

            if (reply == nullptr) {
                error = contextError(context, "empty redis reply when creating consumer group");
                return false;
            }

            if (reply->type == REDIS_REPLY_ERROR) {
                const std::string message = replyString(reply.get());
                if (containsText(message, "BUSYGROUP")) {
                    return true;
                }
                error = message;
                return false;
            }

            return true;
        }

        bool expireKey(redisContext* context, const std::string& key, int ttl_seconds, std::string& error) {
            RedisReplyPtr reply(static_cast<redisReply*>(
                redisCommand(context, "EXPIRE %s %d", key.c_str(), ttl_seconds)
                ));
            return !replyIsError(reply.get(), error, context);
        }

        int taskTtlSeconds(const RedisSection& config) {
            return config.task_ttl_seconds > 0 ? config.task_ttl_seconds : config.ttl_seconds;
        }

        bool hashHasAnyField(const std::map<std::string, std::string>& values) {
            return !values.empty();
        }

        std::string sanitizeRedisKeyPart(std::string text) {
            for (char& ch : text) {
                const bool ok = (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-';
                if (!ok) {
                    ch = '_';
                }
            }
            if (text.empty()) {
                return "default";
            }
            return text;
        }

        RedisReplyPtr commandWithReconnectLocked(
            const RedisSection& config,
            redisContext*& context,
            std::string& error,
            int command_timeout_ms,
            const char* format,
            ...
        );

        bool isTerminalStreamStatus(const std::string& status) {
            return status.empty() || status == "stopped" || status == "failed";
        }

        bool releaseActiveStreamIfMatchedLocked(
            const RedisSection& config,
            redisContext*& context,
            const std::string& active_key,
            const std::string& stream_id,
            std::string& error
        ) {
            RedisReplyPtr current_reply = commandWithReconnectLocked(
                config, context, error, 10000,
                "GET %s",
                active_key.c_str()
            );
            if (replyIsError(current_reply.get(), error, context)) {
                return false;
            }
            if (current_reply->type == REDIS_REPLY_NIL) {
                return true;
            }

            const std::string active_stream_id = replyString(current_reply.get());
            if (active_stream_id != stream_id) {
                return true;
            }

            RedisReplyPtr del_reply = commandWithReconnectLocked(
                config, context, error, 10000,
                "DEL %s",
                active_key.c_str()
            );
            return !replyIsError(del_reply.get(), error, context);
        }

        bool fillTaskFromStreamEntry(redisReply* entry_reply, RedisTask& task, std::string& error) {
            task = RedisTask{};

            if (entry_reply == nullptr || entry_reply->type != REDIS_REPLY_ARRAY || entry_reply->elements < 2) {
                error = "invalid redis stream entry reply";
                return false;
            }

            task.stream_id = replyString(entry_reply->element[0]);

            redisReply* fields_reply = entry_reply->element[1];
            if (fields_reply == nullptr || fields_reply->type != REDIS_REPLY_ARRAY) {
                error = "invalid redis stream fields reply";
                return false;
            }

            std::map<std::string, std::string> fields;
            for (size_t i = 0; i + 1 < fields_reply->elements; i += 2) {
                fields[replyString(fields_reply->element[i])] = replyString(fields_reply->element[i + 1]);
            }

            task.task_id = fields["task_id"];
            task.task_kind = fields["task_kind"];
            task.model_type = fields["model_type"];
            task.input_image_path = fields["input_image_path"];
            task.input_image_key = fields["input_image_key"];
            task.result_image_key = fields["result_image_key"];
            task.input_video_path = fields["input_video_path"];
            task.output_video_path = fields["output_video_path"];
            task.output_video_filename = fields["output_video_filename"];
            task.stream_task_id = fields["stream_id"];
            task.source_type = fields["source_type"];
            task.source_ref = fields["source_ref"];
            task.masked_uri = fields["masked_uri"];
            task.camera_profile = fields["camera_profile"];
            task.legacy_source_uri = fields["source_uri"];
            task.camera_id = static_cast<int>(parseLongLong(fields["camera_id"]));
            task.snapshot_path = fields["snapshot_path"];
            task.snapshot_interval_frames = static_cast<int>(parseLongLong(fields["snapshot_interval_frames"], 5));
            task.target_fps = static_cast<int>(parseLongLong(fields["target_fps"], 10));
            task.people_flow_session_id = fields["session_id"];
            task.people_flow_camera_id = fields["people_flow_camera_id"];
            task.people_flow_config_version = fields["config_version"];
            task.initial_occupancy = parseLongLong(fields["initial_occupancy"]);
            task.create_time_ms = parseLongLong(fields["create_time_ms"]);
            if (task.task_kind.empty()) {
                if (!task.stream_task_id.empty()) {
                    task.task_kind = "stream";
                }
                else {
                    task.task_kind = task.input_video_path.empty() ? "image" : "video";
                }
            }
            if (task.model_type.empty()) {
                task.model_type = "detect";
            }

            if (task.stream_id.empty() || task.task_id.empty()) {
                error = "invalid redis stream message: missing stream_id/task_id";
                return false;
            }
            if (task.task_kind == "video") {
                if (task.input_video_path.empty() || task.output_video_path.empty()) {
                    error = "invalid redis stream message: missing input_video_path/output_video_path";
                    return false;
                }
            }
            else if (task.task_kind == "stream") {
                if (task.stream_task_id.empty() || task.source_type.empty() || task.snapshot_path.empty()) {
                    error = "invalid redis stream message: missing stream_id/source_type/snapshot_path";
                    return false;
                }
            }
            else if (task.task_kind == "people_flow") {
                if (task.people_flow_session_id.empty() || task.people_flow_camera_id.empty() ||
                    task.camera_profile.empty() || task.source_ref.rfind("env:", 0) != 0) {
                    error = "invalid people-flow message: missing session/camera/profile/secret reference";
                    return false;
                }
            }
            else if (task.input_image_key.empty() && task.input_image_path.empty()) {
                error = "invalid redis stream message: missing input_image_key/input_image_path";
                return false;
            }

            return true;
        }

        void freeRawContext(redisContext*& context) {
            if (context != nullptr) {
                redisFree(context);
                context = nullptr;
            }
        }

        bool ensureRawContextLocked(
            const RedisSection& config,
            redisContext*& context,
            std::string& error,
            int command_timeout_ms
        ) {
            if (context != nullptr && !context->err) {
                return setCommandTimeout(context, command_timeout_ms, error);
            }

            freeRawContext(context);

            RedisContextPtr new_context;
            if (!connectContext(config, new_context, error, command_timeout_ms)) {
                return false;
            }

            context = new_context.release();
            return true;
        }

        RedisReplyPtr commandWithReconnectLocked(
            const RedisSection& config,
            redisContext*& context,
            std::string& error,
            int command_timeout_ms,
            const char* format,
            ...
        ) {
            std::string last_error;

            for (int attempt = 1; attempt <= 2; ++attempt) {
                error.clear();

                if (!ensureRawContextLocked(config, context, error, command_timeout_ms)) {
                    last_error = error;
                    freeRawContext(context);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
                    continue;
                }

                va_list args;
                va_start(args, format);
                redisReply* raw_reply = static_cast<redisReply*>(redisvCommand(context, format, args));
                va_end(args);

                if (raw_reply != nullptr) {
                    return RedisReplyPtr(raw_reply);
                }

                last_error = contextError(context, "empty redis reply");
                freeRawContext(context);
                std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
            }

            error = last_error.empty() ? "redis command failed after reconnect" : last_error;
            return RedisReplyPtr(nullptr);
        }

    }  // namespace

    RedisTaskQueue::RedisTaskQueue(const RedisSection& config)
        : config_(config) {
    }

    RedisTaskQueue::~RedisTaskQueue() {
        disconnect();
    }

    void RedisTaskQueue::disconnect() const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        freeRawContext(context_);
    }

    bool RedisTaskQueue::connect(std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        if (!ensureRawContextLocked(config_, context_, error, 10000)) {
            return false;
        }

        RedisReplyPtr ping_reply = commandWithReconnectLocked(config_, context_, error, 10000, "PING");
        if (replyIsError(ping_reply.get(), error, context_)) {
            error = "PING failed: " + error;
            return false;
        }

        if (!ensureConsumerGroup(context_, config_, error)) {
            error = "XGROUP CREATE failed: " + error;
            return false;
        }

        return true;
    }

    bool RedisTaskQueue::ping(std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000, "PING");
        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::setBinaryValue(const std::string& key, const std::string& value, std::string& error) const {
        return setBinaryValueWithTtl(key, value, config_.ttl_seconds, error);
    }

    bool RedisTaskQueue::setBinaryValueWithTtl(const std::string& key, const std::string& value, int ttl_seconds, std::string& error) const {
        if (key.empty()) {
            error = "empty redis binary key";
            return false;
        }
        if (ttl_seconds <= 0) {
            ttl_seconds = config_.ttl_seconds;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %b",
            key.c_str(),
            ttl_seconds,
            value.data(),
            value.size()
        );
        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::getBinaryValue(const std::string& key, std::string& value, std::string& error) const {
        value.clear();
        if (key.empty()) {
            error = "empty redis binary key";
            return false;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "GET %s",
            key.c_str()
        );
        if (replyIsError(reply.get(), error, context_)) {
            return false;
        }
        if (reply->type == REDIS_REPLY_NIL) {
            error = "redis key not found: " + key;
            return false;
        }
        if (reply->type != REDIS_REPLY_STRING) {
            error = "redis key is not a binary/string value: " + key;
            return false;
        }
        value.assign(reply->str, reply->len);
        return true;
    }

    bool RedisTaskQueue::submitTask(const RedisTask& task, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task.task_id);
        const std::string meta_key = metaKey(task.task_id);
        const char* empty = "";

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "queued"
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s task_id %s task_kind %s model_type %s input_image_path %s input_image_key %s result_image_key %s input_video_path %s output_video_path %s output_video_filename %s create_time_ms %lld start_time_ms %lld finish_time_ms %lld queue_wait_ms %lld infer_ms %s process_ms %lld total_ms %lld worker_id %s consumer_name %s error %s result_image_path %s result_image_filename %s progress %s processed_frames %lld total_frames %lld current_frame_index %lld cancel_requested %s fps %.6f video_width %d video_height %d duration_ms %lld",
            meta_key.c_str(),
            task.task_id.c_str(),
            task.task_kind.empty() ? "image" : task.task_kind.c_str(),
            task.model_type.empty() ? "detect" : task.model_type.c_str(),
            task.input_image_path.c_str(),
            task.input_image_key.c_str(),
            task.result_image_key.c_str(),
            task.input_video_path.c_str(),
            task.output_video_path.c_str(),
            task.output_video_filename.c_str(),
            task.create_time_ms,
            0LL,
            0LL,
            0LL,
            "0",
            0LL,
            0LL,
            empty,
            empty,
            empty,
            empty,
            empty,
            "0",
            0LL,
            task.video_total_frames,
            0LL,
            "0",
            task.video_fps,
            task.video_width,
            task.video_height,
            task.video_duration_ms
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }

        if (!expireKey(context_, meta_key, taskTtlSeconds(config_), error)) {
            return false;
        }

        RedisReplyPtr stream_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "XADD %s * task_id %s task_kind %s model_type %s input_image_key %s result_image_key %s input_image_path %s input_video_path %s output_video_path %s output_video_filename %s create_time_ms %lld",
            config_.stream_key.c_str(),
            task.task_id.c_str(),
            task.task_kind.empty() ? "image" : task.task_kind.c_str(),
            task.model_type.empty() ? "detect" : task.model_type.c_str(),
            task.input_image_key.c_str(),
            task.result_image_key.c_str(),
            task.input_image_path.c_str(),
            task.input_video_path.c_str(),
            task.output_video_path.c_str(),
            task.output_video_filename.c_str(),
            task.create_time_ms
        );
        if (replyIsError(stream_reply.get(), error, context_)) {
            return false;
        }

        if (config_.stream_max_len > 0) {
            RedisReplyPtr trim_reply = commandWithReconnectLocked(
                config_,
                context_,
                error,
                10000,
                "XTRIM %s MAXLEN ~ %lld",
                config_.stream_key.c_str(),
                config_.stream_max_len
            );
            if (replyIsError(trim_reply.get(), error, context_)) {
                error = "XTRIM failed: " + error;
                return false;
            }
        }

        return true;
    }

    bool RedisTaskQueue::markRunning(
        const std::string& task_id,
        long long start_time_ms,
        int worker_id,
        const std::string& consumer_name,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string meta_key = metaKey(task_id);

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "running"
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s start_time_ms %lld worker_id %d consumer_name %s",
            meta_key.c_str(),
            start_time_ms,
            worker_id,
            consumer_name.c_str()
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }

        return expireKey(context_, meta_key, taskTtlSeconds(config_), error);
    }

    bool RedisTaskQueue::markDone(
        const std::string& task_id,
        const std::string& result_json_text,
        const std::string& result_image_key,
        const std::string& result_image_path,
        const std::string& result_image_filename,
        long long finish_time_ms,
        long long queue_wait_ms,
        double infer_ms,
        long long total_ms,
        int worker_id,
        const std::string& consumer_name,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string result_key = resultKey(task_id);
        const std::string meta_key = metaKey(task_id);
        const char* empty = "";

        RedisReplyPtr result_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            result_key.c_str(),
            taskTtlSeconds(config_),
            result_json_text.c_str()
        );
        if (replyIsError(result_reply.get(), error, context_)) {
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s finish_time_ms %lld result_image_key %s result_image_path %s result_image_filename %s queue_wait_ms %lld infer_ms %.6f total_ms %lld worker_id %d consumer_name %s error %s",
            meta_key.c_str(),
            finish_time_ms,
            result_image_key.c_str(),
            result_image_path.c_str(),
            result_image_filename.c_str(),
            queue_wait_ms,
            infer_ms,
            total_ms,
            worker_id,
            consumer_name.c_str(),
            empty
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }

        if (!expireKey(context_, meta_key, taskTtlSeconds(config_), error)) {
            return false;
        }

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "done"
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }

        if (config_.metrics_enabled) {
            std::string metrics_error;
            const std::string global_key = metricsGlobalKey();
            const std::string worker_done_key = metricsWorkerDoneKey();
            const std::string recent_key = metricsRecentDoneKey();
            const std::string recent_member = std::to_string(finish_time_ms) + ":" + task_id;

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s done_count 1", global_key.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_queue_wait_ms %.6f", global_key.c_str(), static_cast<double>(queue_wait_ms));
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_inference_ms %.6f", global_key.c_str(), infer_ms);
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_total_ms %.6f", global_key.c_str(), static_cast<double>(total_ms));
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HSET %s last_finish_time_ms %lld last_task_id %s",
                global_key.c_str(), finish_time_ms, task_id.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s %s 1", worker_done_key.c_str(), consumer_name.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "ZADD %s %lld %s", recent_key.c_str(), finish_time_ms, recent_member.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d", recent_key.c_str(), config_.metrics_ttl_seconds);
        }

        return true;
    }

    bool RedisTaskQueue::markFailed(
        const std::string& task_id,
        const std::string& error_message,
        long long finish_time_ms,
        long long queue_wait_ms,
        long long total_ms,
        int worker_id,
        const std::string& consumer_name,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string meta_key = metaKey(task_id);

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s finish_time_ms %lld queue_wait_ms %lld total_ms %lld worker_id %d consumer_name %s error %s",
            meta_key.c_str(),
            finish_time_ms,
            queue_wait_ms,
            total_ms,
            worker_id,
            consumer_name.c_str(),
            error_message.c_str()
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }

        if (!expireKey(context_, meta_key, taskTtlSeconds(config_), error)) {
            return false;
        }

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "failed"
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }

        if (config_.metrics_enabled) {
            std::string metrics_error;
            const std::string global_key = metricsGlobalKey();
            const std::string worker_failed_key = metricsWorkerFailedKey();
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s failed_count 1", global_key.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HSET %s last_finish_time_ms %lld last_task_id %s",
                global_key.c_str(), finish_time_ms, task_id.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s %s 1", worker_failed_key.c_str(), consumer_name.c_str());
        }

        return true;
    }

    bool RedisTaskQueue::updateVideoProgress(
        const std::string& task_id,
        long long processed_frames,
        long long total_frames,
        long long current_frame_index,
        double progress,
        double fps,
        int width,
        int height,
        long long process_ms,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string meta_key = metaKey(task_id);
        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s processed_frames %lld total_frames %lld current_frame_index %lld progress %.6f fps %.6f video_width %d video_height %d process_ms %lld",
            meta_key.c_str(),
            processed_frames,
            total_frames,
            current_frame_index,
            progress,
            fps,
            width,
            height,
            process_ms
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }
        return expireKey(context_, meta_key, taskTtlSeconds(config_), error);
    }

    bool RedisTaskQueue::markVideoDone(
        const std::string& task_id,
        const std::string& result_json_text,
        const std::string& output_video_path,
        const std::string& output_video_filename,
        long long finish_time_ms,
        long long queue_wait_ms,
        long long process_ms,
        long long total_ms,
        long long processed_frames,
        long long total_frames,
        double fps,
        int width,
        int height,
        long long duration_ms,
        int worker_id,
        const std::string& consumer_name,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string result_key = resultKey(task_id);
        const std::string meta_key = metaKey(task_id);
        const char* empty = "";

        RedisReplyPtr result_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            result_key.c_str(),
            taskTtlSeconds(config_),
            result_json_text.c_str()
        );
        if (replyIsError(result_reply.get(), error, context_)) {
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s finish_time_ms %lld output_video_path %s output_video_filename %s queue_wait_ms %lld infer_ms %.6f process_ms %lld total_ms %lld processed_frames %lld total_frames %lld current_frame_index %lld progress %.6f fps %.6f video_width %d video_height %d duration_ms %lld worker_id %d consumer_name %s error %s",
            meta_key.c_str(),
            finish_time_ms,
            output_video_path.c_str(),
            output_video_filename.c_str(),
            queue_wait_ms,
            static_cast<double>(process_ms),
            process_ms,
            total_ms,
            processed_frames,
            total_frames,
            processed_frames,
            1.0,
            fps,
            width,
            height,
            duration_ms,
            worker_id,
            consumer_name.c_str(),
            empty
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }

        if (!expireKey(context_, meta_key, taskTtlSeconds(config_), error)) {
            return false;
        }

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "done"
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }

        if (config_.metrics_enabled) {
            std::string metrics_error;
            const std::string global_key = metricsGlobalKey();
            const std::string worker_done_key = metricsWorkerDoneKey();
            const std::string recent_key = metricsRecentDoneKey();
            const std::string recent_member = std::to_string(finish_time_ms) + ":" + task_id;

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s done_count 1", global_key.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_queue_wait_ms %.6f", global_key.c_str(), static_cast<double>(queue_wait_ms));
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_inference_ms %.6f", global_key.c_str(), static_cast<double>(process_ms));
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBYFLOAT %s total_total_ms %.6f", global_key.c_str(), static_cast<double>(total_ms));
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HSET %s last_finish_time_ms %lld last_task_id %s",
                global_key.c_str(), finish_time_ms, task_id.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s %s 1", worker_done_key.c_str(), consumer_name.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "ZADD %s %lld %s", recent_key.c_str(), finish_time_ms, recent_member.c_str());
            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d", recent_key.c_str(), config_.metrics_ttl_seconds);
        }

        return true;
    }

    bool RedisTaskQueue::markVideoCanceled(
        const std::string& task_id,
        const std::string& result_json_text,
        long long finish_time_ms,
        long long queue_wait_ms,
        long long process_ms,
        long long total_ms,
        long long processed_frames,
        long long total_frames,
        int worker_id,
        const std::string& consumer_name,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string result_key = resultKey(task_id);
        const std::string meta_key = metaKey(task_id);

        RedisReplyPtr result_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            result_key.c_str(),
            taskTtlSeconds(config_),
            result_json_text.c_str()
        );
        if (replyIsError(result_reply.get(), error, context_)) {
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s finish_time_ms %lld queue_wait_ms %lld process_ms %lld total_ms %lld processed_frames %lld total_frames %lld progress %.6f worker_id %d consumer_name %s error %s cancel_requested %s",
            meta_key.c_str(),
            finish_time_ms,
            queue_wait_ms,
            process_ms,
            total_ms,
            processed_frames,
            total_frames,
            total_frames > 0 ? static_cast<double>(processed_frames) / static_cast<double>(total_frames) : 0.0,
            worker_id,
            consumer_name.c_str(),
            "canceled by user",
            "1"
        );
        if (replyIsError(meta_reply.get(), error, context_)) {
            return false;
        }
        if (!expireKey(context_, meta_key, taskTtlSeconds(config_), error)) {
            return false;
        }

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "SETEX %s %d %s",
            status_key.c_str(),
            taskTtlSeconds(config_),
            "canceled"
        );
        return !replyIsError(status_reply.get(), error, context_);
    }

    bool RedisTaskQueue::requestCancelTask(const std::string& task_id, std::string& error) const {
        if (task_id.empty()) {
            error = "empty task_id";
            return false;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        const std::string meta_key = metaKey(task_id);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %s cancel_requested 1",
            meta_key.c_str()
        );
        if (replyIsError(reply.get(), error, context_)) {
            return false;
        }
        return expireKey(context_, meta_key, taskTtlSeconds(config_), error);
    }

    bool RedisTaskQueue::isCancelRequested(const std::string& task_id, bool& cancel_requested, std::string& error) const {
        cancel_requested = false;
        if (task_id.empty()) {
            error = "empty task_id";
            return false;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        const std::string meta_key = metaKey(task_id);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HGET %s cancel_requested",
            meta_key.c_str()
        );
        if (replyIsError(reply.get(), error, context_)) {
            return false;
        }
        if (reply->type == REDIS_REPLY_NIL) {
            return true;
        }
        cancel_requested = parseLongLong(replyString(reply.get())) != 0;
        return true;
    }

    bool RedisTaskQueue::submitStreamTask(const StreamStartRequest& request, std::string& error) const {
        if (request.stream_id.empty()) {
            error = "empty stream_id";
            return false;
        }
        if (request.source_type.empty()) {
            error = "empty stream source_type";
            return false;
        }
        if (request.snapshot_path.empty()) {
            error = "empty stream snapshot_path";
            return false;
        }
        if (request.source_type == "rtsp" && request.source_ref.rfind("env:", 0) != 0) {
            error = "RTSP stream requires an environment secret reference";
            return false;
        }
        if (request.source_ref.find("rtsp://") != std::string::npos ||
            request.source_ref.find("rtsps://") != std::string::npos) {
            error = "plaintext RTSP URI is forbidden in Redis source_ref";
            return false;
        }

        const long long create_time_ms = request.create_time_ms > 0 ? request.create_time_ms : nowMs();
        const int ttl = taskTtlSeconds(config_);
        const std::string status_key = streamTaskStatusKey(request.stream_id);
        const std::string meta_key = streamTaskMetaKey(request.stream_id);
        const std::string latest_key = streamTaskLatestKey(request.stream_id);
        const std::string active_key = activeStreamTaskKey();

        std::lock_guard<std::mutex> lock(context_mutex_);

        auto reserve_active_stream = [&]() -> bool {
            RedisReplyPtr set_active_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "SET %s %s NX EX %d",
                active_key.c_str(), request.stream_id.c_str(), ttl
            );
            if (replyIsError(set_active_reply.get(), error, context_)) {
                return false;
            }
            if (set_active_reply->type != REDIS_REPLY_NIL) {
                return true;
            }

            RedisReplyPtr current_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "GET %s",
                active_key.c_str()
            );
            if (replyIsError(current_reply.get(), error, context_)) {
                return false;
            }

            const std::string active_stream_id = current_reply->type == REDIS_REPLY_NIL
                ? std::string{}
                : replyString(current_reply.get());

            std::string active_status;
            if (!active_stream_id.empty()) {
                RedisReplyPtr active_status_reply = commandWithReconnectLocked(
                    config_, context_, error, 10000,
                    "GET %s",
                    streamTaskStatusKey(active_stream_id).c_str()
                );
                if (replyIsError(active_status_reply.get(), error, context_)) {
                    return false;
                }
                if (active_status_reply->type != REDIS_REPLY_NIL) {
                    active_status = replyString(active_status_reply.get());
                }
            }

            if (!active_stream_id.empty() && !isTerminalStreamStatus(active_status)) {
                error = "ACTIVE_STREAM_RUNNING:" + active_stream_id;
                return false;
            }

            RedisReplyPtr delete_stale_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "DEL %s",
                active_key.c_str()
            );
            if (replyIsError(delete_stale_reply.get(), error, context_)) {
                return false;
            }

            RedisReplyPtr retry_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "SET %s %s NX EX %d",
                active_key.c_str(), request.stream_id.c_str(), ttl
            );
            if (replyIsError(retry_reply.get(), error, context_)) {
                return false;
            }
            if (retry_reply->type == REDIS_REPLY_NIL) {
                error = active_stream_id.empty()
                    ? "ACTIVE_STREAM_RUNNING"
                    : "ACTIVE_STREAM_RUNNING:" + active_stream_id;
                return false;
            }
            return true;
        };

        if (!reserve_active_stream()) {
            return false;
        }

        auto rollback_active_stream = [&]() {
            std::string ignored;
            (void)releaseActiveStreamIfMatchedLocked(config_, context_, active_key, request.stream_id, ignored);
        };

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_, context_, error, 10000,
            "SETEX %s %d %s",
            status_key.c_str(), ttl, "created");
        if (replyIsError(status_reply.get(), error, context_)) {
            rollback_active_stream();
            return false;
        }

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_, context_, error, 10000,
            "HSET %s stream_id %s task_kind %s model_type %s source_type %s source_ref %s masked_uri %s camera_profile %s camera_id %d snapshot_path %s snapshot_interval_frames %d target_fps %d create_time_ms %lld start_time_ms %lld stop_time_ms %lld frame_count %lld fps %.6f width %d height %d reconnect_count %d no_frame_count %d stop_requested %s worker_id %d consumer_name %s error %s last_error %s",
            meta_key.c_str(),
            request.stream_id.c_str(),
            "stream",
            request.model_type.empty() ? "detect" : request.model_type.c_str(),
            request.source_type.c_str(),
            request.source_ref.c_str(),
            request.masked_uri.c_str(),
            request.camera_profile.c_str(),
            request.camera_id,
            request.snapshot_path.c_str(),
            request.snapshot_interval_frames,
            request.target_fps,
            create_time_ms,
            0LL,
            0LL,
            0LL,
            0.0,
            0,
            0,
            0,
            0,
            "0",
            0,
            "",
            "",
            "");
        if (replyIsError(meta_reply.get(), error, context_)) {
            rollback_active_stream();
            return false;
        }
        if (!expireKey(context_, meta_key, ttl, error)) {
            rollback_active_stream();
            return false;
        }

        RedisReplyPtr latest_reply = commandWithReconnectLocked(
            config_, context_, error, 10000,
            "HSET %s stream_id %s latest_snapshot_path %s frame_count %lld fps %.6f width %d height %d last_num_detections %d last_update_ms %lld",
            latest_key.c_str(),
            request.stream_id.c_str(),
            request.snapshot_path.c_str(),
            0LL,
            0.0,
            0,
            0,
            0,
            0LL);
        if (replyIsError(latest_reply.get(), error, context_)) {
            rollback_active_stream();
            return false;
        }
        if (!expireKey(context_, latest_key, ttl, error)) {
            rollback_active_stream();
            return false;
        }

        RedisReplyPtr stream_reply = commandWithReconnectLocked(
            config_, context_, error, 10000,
            "XADD %s * task_id %s task_kind %s model_type %s stream_id %s source_type %s source_ref %s masked_uri %s camera_profile %s camera_id %d snapshot_path %s snapshot_interval_frames %d target_fps %d create_time_ms %lld",
            config_.stream_key.c_str(),
            request.stream_id.c_str(),
            "stream",
            request.model_type.empty() ? "detect" : request.model_type.c_str(),
            request.stream_id.c_str(),
            request.source_type.c_str(),
            request.source_ref.c_str(),
            request.masked_uri.c_str(),
            request.camera_profile.c_str(),
            request.camera_id,
            request.snapshot_path.c_str(),
            request.snapshot_interval_frames,
            request.target_fps,
            create_time_ms);
        if (replyIsError(stream_reply.get(), error, context_)) {
            rollback_active_stream();
            return false;
        }

        if (config_.stream_max_len > 0) {
            RedisReplyPtr trim_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "XTRIM %s MAXLEN ~ %lld",
                config_.stream_key.c_str(),
                config_.stream_max_len);
            if (replyIsError(trim_reply.get(), error, context_)) {
                error = "XTRIM stream task failed: " + error;
                rollback_active_stream();
                return false;
            }
        }

        return true;
    }

    bool RedisTaskQueue::markStreamStarting(const std::string& stream_id, long long start_time_ms, int worker_id, const std::string& consumer_name, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s", streamTaskStatusKey(stream_id).c_str(), ttl, "starting");
        if (replyIsError(status_reply.get(), error, context_)) return false;
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s start_time_ms %lld worker_id %d consumer_name %s status %s",
            streamTaskMetaKey(stream_id).c_str(), start_time_ms, worker_id, consumer_name.c_str(), "starting");
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        return expireKey(context_, streamTaskMetaKey(stream_id), ttl, error);
    }

    bool RedisTaskQueue::markStreamRunning(const std::string& stream_id, int width, int height, double fps, long long start_time_ms, int worker_id, const std::string& consumer_name, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s", streamTaskStatusKey(stream_id).c_str(), ttl, "running");
        if (replyIsError(status_reply.get(), error, context_)) return false;
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s status %s start_time_ms %lld worker_id %d consumer_name %s width %d height %d fps %.6f no_frame_count %d error %s last_error %s",
            streamTaskMetaKey(stream_id).c_str(), "running", start_time_ms, worker_id, consumer_name.c_str(), width, height, fps, 0, "", "");
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        return expireKey(context_, streamTaskMetaKey(stream_id), ttl, error);
    }

    bool RedisTaskQueue::markStreamReconnecting(const std::string& stream_id, const std::string& reason, int reconnect_count, int no_frame_count, long long update_time_ms, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s", streamTaskStatusKey(stream_id).c_str(), ttl, "reconnecting");
        if (replyIsError(status_reply.get(), error, context_)) return false;
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s status %s last_error %s error %s reconnect_count %d no_frame_count %d last_update_ms %lld",
            streamTaskMetaKey(stream_id).c_str(), "reconnecting", reason.c_str(), reason.c_str(), reconnect_count, no_frame_count, update_time_ms);
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        return expireKey(context_, streamTaskMetaKey(stream_id), ttl, error);
    }

    bool RedisTaskQueue::updateStreamCaptureMetrics(
        const std::string& stream_id,
        const StreamCaptureMetrics& metrics,
        std::string& error
    ) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        const long long update_time_ms = metrics.update_time_ms > 0 ? metrics.update_time_ms : nowMs();
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s capture_state %s capture_backend %s backend_fallback %d capture_fps %.6f infer_fps %.6f source_fps %.6f latest_frame_age_ms %lld dropped_frames %lld reconnect_count %d last_frame_time_ms %lld width %d height %d resolution_changed %d resolution_change_count %lld last_update_ms %lld",
            streamTaskMetaKey(stream_id).c_str(),
            metrics.capture_state.c_str(),
            metrics.capture_backend.c_str(),
            metrics.backend_fallback ? 1 : 0,
            metrics.capture_fps,
            metrics.infer_fps,
            metrics.source_fps,
            metrics.latest_frame_age_ms,
            metrics.dropped_frames,
            metrics.reconnect_count,
            metrics.last_frame_time_ms,
            metrics.width,
            metrics.height,
            metrics.resolution_changed ? 1 : 0,
            metrics.resolution_change_count,
            update_time_ms);
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        return expireKey(context_, streamTaskMetaKey(stream_id), ttl, error);
    }

    bool RedisTaskQueue::updateStreamLatest(const std::string& stream_id, const std::string& latest_snapshot_path, long long frame_count, double fps, int width, int height, int last_num_detections, long long update_time_ms, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr latest_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s stream_id %s latest_snapshot_path %s frame_count %lld fps %.6f width %d height %d last_num_detections %d last_update_ms %lld",
            streamTaskLatestKey(stream_id).c_str(), stream_id.c_str(), latest_snapshot_path.c_str(), frame_count, fps, width, height, last_num_detections, update_time_ms);
        if (replyIsError(latest_reply.get(), error, context_)) return false;
        if (!expireKey(context_, streamTaskLatestKey(stream_id), ttl, error)) return false;
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s latest_snapshot_path %s frame_count %lld fps %.6f width %d height %d last_num_detections %d last_update_ms %lld no_frame_count %d",
            streamTaskMetaKey(stream_id).c_str(), latest_snapshot_path.c_str(), frame_count, fps, width, height, last_num_detections, update_time_ms, 0);
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        return expireKey(context_, streamTaskMetaKey(stream_id), ttl, error);
    }

    bool RedisTaskQueue::markStreamStopped(const std::string& stream_id,
        long long stop_time_ms,
        long long frame_count,
        std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);

        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s status %s stop_time_ms %lld frame_count %lld last_update_ms %lld",
            streamTaskMetaKey(stream_id).c_str(),
            "stopped",
            stop_time_ms,
            frame_count,
            stop_time_ms);
        if (replyIsError(meta_reply.get(), error, context_)) return false;

        if (!expireKey(context_, streamTaskMetaKey(stream_id), ttl, error)) return false;

        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s",
            streamTaskStatusKey(stream_id).c_str(),
            ttl,
            "stopped");
        if (replyIsError(status_reply.get(), error, context_)) return false;

        // Phase 14.5 stream metrics hotfix:
        // stream 任务是长生命周期任务，进入 running 后已 XACK，不能依赖普通队列 done 统计。
        // 所以在 stopped 终态这里累计 done_count。
        if (config_.metrics_enabled) {
            std::string metrics_error;
            std::string consumer_name;

            RedisReplyPtr consumer_reply = commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HGET %s consumer_name",
                streamTaskMetaKey(stream_id).c_str());

            if (consumer_reply &&
                consumer_reply->type != REDIS_REPLY_NIL &&
                consumer_reply->type != REDIS_REPLY_ERROR) {
                consumer_name = replyString(consumer_reply.get());
            }

            const std::string global_key = metricsGlobalKey();
            const std::string worker_done_key = metricsWorkerDoneKey();
            const std::string recent_key = metricsRecentDoneKey();

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s done_count 1",
                global_key.c_str());

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HSET %s last_task_id %s last_finish_time_ms %lld",
                global_key.c_str(),
                stream_id.c_str(),
                stop_time_ms);

            if (!consumer_name.empty()) {
                commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                    "HINCRBY %s %s 1",
                    worker_done_key.c_str(),
                    consumer_name.c_str());
            }

            const std::string recent_member = stream_id + ":" + std::to_string(stop_time_ms);

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "ZADD %s %lld %s",
                recent_key.c_str(),
                stop_time_ms,
                recent_member.c_str());

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d",
                global_key.c_str(),
                config_.metrics_ttl_seconds);

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d",
                worker_done_key.c_str(),
                config_.metrics_ttl_seconds);

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d",
                recent_key.c_str(),
                config_.metrics_ttl_seconds);
        }

        return releaseActiveStreamIfMatchedLocked(config_, context_, activeStreamTaskKey(), stream_id, error);
    }

    bool RedisTaskQueue::markStreamFailed(const std::string& stream_id, const std::string& error_message, long long stop_time_ms, long long frame_count, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s status %s stop_time_ms %lld frame_count %lld error %s last_error %s last_update_ms %lld",
            streamTaskMetaKey(stream_id).c_str(), "failed", stop_time_ms, frame_count, error_message.c_str(), error_message.c_str(), stop_time_ms);
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        if (!expireKey(context_, streamTaskMetaKey(stream_id), ttl, error)) return false;
        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s", streamTaskStatusKey(stream_id).c_str(), ttl, "failed");
        if (replyIsError(status_reply.get(), error, context_)) return false;

        // Phase 14.5 stream metrics hotfix:
        // failed 终态也要累计，包括无效 camera、RTSP 打不开、重连耗尽、stale cleanup。
        if (config_.metrics_enabled) {
            std::string metrics_error;
            std::string consumer_name;

            RedisReplyPtr consumer_reply = commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HGET %s consumer_name", streamTaskMetaKey(stream_id).c_str());

            if (consumer_reply && consumer_reply->type != REDIS_REPLY_NIL && consumer_reply->type != REDIS_REPLY_ERROR) {
                consumer_name = replyString(consumer_reply.get());
            }

            const std::string global_key = metricsGlobalKey();
            const std::string worker_failed_key = metricsWorkerFailedKey();

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HINCRBY %s failed_count 1", global_key.c_str());

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "HSET %s last_task_id %s last_finish_time_ms %lld",
                global_key.c_str(), stream_id.c_str(), stop_time_ms);

            if (!consumer_name.empty()) {
                commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                    "HINCRBY %s %s 1", worker_failed_key.c_str(), consumer_name.c_str());
            }

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d", global_key.c_str(), config_.metrics_ttl_seconds);

            commandWithReconnectLocked(config_, context_, metrics_error, 10000,
                "EXPIRE %s %d", worker_failed_key.c_str(), config_.metrics_ttl_seconds);
        }

        return releaseActiveStreamIfMatchedLocked(config_, context_, activeStreamTaskKey(), stream_id, error);
    }

    bool RedisTaskQueue::requestStopStreamTask(const std::string& stream_id, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        const int ttl = taskTtlSeconds(config_);
        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s stop_requested 1 status %s",
            streamTaskMetaKey(stream_id).c_str(), "stopping");
        if (replyIsError(meta_reply.get(), error, context_)) return false;
        if (!expireKey(context_, streamTaskMetaKey(stream_id), ttl, error)) return false;
        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SETEX %s %d %s", streamTaskStatusKey(stream_id).c_str(), ttl, "stopping");
        return !replyIsError(status_reply.get(), error, context_);
    }

    bool RedisTaskQueue::isStreamStopRequested(const std::string& stream_id, bool& stop_requested, std::string& error) const {
        stop_requested = false;
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGET %s stop_requested", streamTaskMetaKey(stream_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type == REDIS_REPLY_NIL) return true;
        stop_requested = parseLongLong(replyString(reply.get())) != 0;
        return true;
    }

    bool RedisTaskQueue::getStreamTaskStatus(const std::string& stream_id, StreamTaskStatus& status, std::string& error) const {
        status = StreamTaskStatus{};
        status.stream_id = stream_id;
        std::lock_guard<std::mutex> lock(context_mutex_);

        RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "GET %s", streamTaskStatusKey(stream_id).c_str());
        if (replyIsError(status_reply.get(), error, context_)) return false;
        if (status_reply->type == REDIS_REPLY_NIL) {
            status.found = false;
            return true;
        }
        status.found = true;
        status.status = replyString(status_reply.get());

        RedisReplyPtr meta_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGETALL %s", streamTaskMetaKey(stream_id).c_str());
        if (!replyIsError(meta_reply.get(), error, context_)) {
            const auto values = parseHashReply(meta_reply.get());
            auto getValue = [&values](const std::string& key) -> std::string {
                auto it = values.find(key);
                return it == values.end() ? std::string{} : it->second;
            };
            status.model_type = getValue("model_type");
            status.source_type = getValue("source_type");
            status.source_ref = getValue("source_ref");
            status.masked_uri = getValue("masked_uri");
            status.camera_profile = getValue("camera_profile");
            if (status.masked_uri.empty()) {
                // Migration path for Phase 18.5 records that may remain until
                // their TTL expires. Never expose the legacy value directly.
                status.masked_uri = maskStreamSource(status.source_type, getValue("source_uri"));
            }
            status.camera_id = static_cast<int>(parseLongLong(getValue("camera_id")));
            status.snapshot_path = getValue("snapshot_path");
            status.latest_snapshot_path = getValue("latest_snapshot_path");
            status.error = getValue("error");
            status.last_error = getValue("last_error");
            status.consumer_name = getValue("consumer_name");
            status.worker_id = static_cast<int>(parseLongLong(getValue("worker_id")));
            status.stop_requested = parseLongLong(getValue("stop_requested")) != 0;
            status.create_time_ms = parseLongLong(getValue("create_time_ms"));
            status.start_time_ms = parseLongLong(getValue("start_time_ms"));
            status.stop_time_ms = parseLongLong(getValue("stop_time_ms"));
            status.last_update_ms = parseLongLong(getValue("last_update_ms"));
            status.frame_count = parseLongLong(getValue("frame_count"));
            status.fps = parseDouble(getValue("fps"));
            status.width = static_cast<int>(parseLongLong(getValue("width")));
            status.height = static_cast<int>(parseLongLong(getValue("height")));
            status.last_num_detections = static_cast<int>(parseLongLong(getValue("last_num_detections")));
            status.reconnect_count = static_cast<int>(parseLongLong(getValue("reconnect_count")));
            status.no_frame_count = static_cast<int>(parseLongLong(getValue("no_frame_count")));
            status.capture_state = getValue("capture_state");
            status.capture_backend = getValue("capture_backend");
            status.backend_fallback = parseLongLong(getValue("backend_fallback")) != 0;
            status.capture_fps = parseDouble(getValue("capture_fps"));
            status.infer_fps = parseDouble(getValue("infer_fps"));
            status.source_fps = parseDouble(getValue("source_fps"));
            status.latest_frame_age_ms = parseLongLong(getValue("latest_frame_age_ms"), -1);
            status.dropped_frames = parseLongLong(getValue("dropped_frames"));
            status.last_frame_time_ms = parseLongLong(getValue("last_frame_time_ms"));
            status.resolution_changed = parseLongLong(getValue("resolution_changed")) != 0;
            status.resolution_change_count = parseLongLong(getValue("resolution_change_count"));
        }

        RedisReplyPtr latest_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGETALL %s", streamTaskLatestKey(stream_id).c_str());
        if (!replyIsError(latest_reply.get(), error, context_)) {
            const auto latest = parseHashReply(latest_reply.get());
            auto getLatest = [&latest](const std::string& key) -> std::string {
                auto it = latest.find(key);
                return it == latest.end() ? std::string{} : it->second;
            };
            const std::string latest_snapshot_path = getLatest("latest_snapshot_path");
            if (!latest_snapshot_path.empty()) status.latest_snapshot_path = latest_snapshot_path;
            const long long frame_count = parseLongLong(getLatest("frame_count"), -1);
            if (frame_count >= 0) status.frame_count = frame_count;
            const double fps = parseDouble(getLatest("fps"), -1.0);
            if (fps >= 0.0) status.fps = fps;
            const long long last_update_ms = parseLongLong(getLatest("last_update_ms"), -1);
            if (last_update_ms >= 0) status.last_update_ms = last_update_ms;
            const long long width = parseLongLong(getLatest("width"), -1);
            if (width >= 0) status.width = static_cast<int>(width);
            const long long height = parseLongLong(getLatest("height"), -1);
            if (height >= 0) status.height = static_cast<int>(height);
            const long long dets = parseLongLong(getLatest("last_num_detections"), -1);
            if (dets >= 0) status.last_num_detections = static_cast<int>(dets);
        }

        return true;
    }

    bool RedisTaskQueue::getActiveStreamTask(StreamTaskStatus& status, std::string& error) const {
        status = StreamTaskStatus{};
        std::string active_stream_id;
        {
            std::lock_guard<std::mutex> lock(context_mutex_);
            RedisReplyPtr active_reply = commandWithReconnectLocked(
                config_, context_, error, 10000,
                "GET %s",
                activeStreamTaskKey().c_str()
            );
            if (replyIsError(active_reply.get(), error, context_)) {
                return false;
            }
            if (active_reply->type == REDIS_REPLY_NIL) {
                status.found = false;
                return true;
            }
            active_stream_id = replyString(active_reply.get());
        }

        if (active_stream_id.empty()) {
            status.found = false;
            return true;
        }
        return getStreamTaskStatus(active_stream_id, status, error);
    }

    bool RedisTaskQueue::submitPeopleFlowTask(const PeopleFlowStartRequest& request, std::string& error) const {
        if (request.session_id.empty() || request.camera_id.empty() || request.camera_profile.empty()) {
            error = "people-flow start requires session_id, camera_id and camera_profile";
            return false;
        }
        if (request.source_ref.rfind("env:", 0) != 0 || request.source_ref.find("rtsp://") != std::string::npos) {
            error = "people-flow source_ref must be an environment reference, never a plaintext URI";
            return false;
        }
        const int active_ttl = std::max(10, request.active_ttl_seconds);
        const int session_ttl = std::max(60, request.session_ttl_seconds);
        const long long create_time_ms = request.create_time_ms > 0 ? request.create_time_ms : nowMs();
        const std::string active_key = peopleFlowActiveKey(request.camera_id);
        const std::string session_key = peopleFlowSessionKey(request.session_id);
        const std::string realtime_key = peopleFlowRealtimeKey(request.camera_id);

        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reserve_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "SET %s %s NX EX %d", active_key.c_str(), request.session_id.c_str(), active_ttl);
        if (replyIsError(reserve_reply.get(), error, context_)) return false;
        if (reserve_reply->type == REDIS_REPLY_NIL) {
            RedisReplyPtr current_reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "GET %s", active_key.c_str());
            if (replyIsError(current_reply.get(), error, context_)) return false;
            const std::string active_session = current_reply->type == REDIS_REPLY_NIL
                ? std::string{}
                : replyString(current_reply.get());
            std::string active_status;
            if (!active_session.empty()) {
                RedisReplyPtr status_reply = commandWithReconnectLocked(config_, context_, error, 10000,
                    "HGET %s status", peopleFlowSessionKey(active_session).c_str());
                if (replyIsError(status_reply.get(), error, context_)) return false;
                if (status_reply->type != REDIS_REPLY_NIL) active_status = replyString(status_reply.get());
            }
            if (!active_session.empty() && !isTerminalStreamStatus(active_status)) {
                error = "CAMERA_ALREADY_ACTIVE:" + active_session;
                return false;
            }
            RedisReplyPtr delete_reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "DEL %s", active_key.c_str());
            if (replyIsError(delete_reply.get(), error, context_)) return false;
            reserve_reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "SET %s %s NX EX %d", active_key.c_str(), request.session_id.c_str(), active_ttl);
            if (replyIsError(reserve_reply.get(), error, context_)) return false;
            if (reserve_reply->type == REDIS_REPLY_NIL) {
                error = "CAMERA_ALREADY_ACTIVE";
                return false;
            }
        }

        auto rollback = [&]() {
            std::string ignored;
            (void)releaseActiveStreamIfMatchedLocked(config_, context_, active_key, request.session_id, ignored);
        };

        RedisReplyPtr session_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s session_id %s camera_id %s camera_profile %s source_ref %s masked_uri %s config_version %s status %s snapshot_path %s initial_occupancy %lld in_count %lld out_count %lld occupancy %lld applied_calibration_version %lld live_persons %d create_time_ms %lld start_time_ms %lld stop_time_ms %lld last_update_ms %lld frame_count %lld stop_requested %d storage_degraded %d snapshot_degraded %d reconnect_count %d error %s last_error %s",
            session_key.c_str(), request.session_id.c_str(), request.camera_id.c_str(),
            request.camera_profile.c_str(), request.source_ref.c_str(), request.masked_uri.c_str(),
            request.config_version.c_str(), "queued", request.snapshot_path.c_str(),
            request.initial_occupancy, 0LL, 0LL, request.initial_occupancy, 0LL, 0,
            create_time_ms, 0LL, 0LL, create_time_ms, 0LL, 0, 0, 0, 0, "", "");
        if (replyIsError(session_reply.get(), error, context_) ||
            !expireKey(context_, session_key, session_ttl, error)) {
            rollback();
            return false;
        }

        RedisReplyPtr realtime_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s session_id %s camera_id %s status %s in_count %lld out_count %lld occupancy %lld applied_calibration_version %lld requested_calibration_version %lld requested_occupancy %lld live_persons %d frame_count %lld last_update_ms %lld config_version %s",
            realtime_key.c_str(), request.session_id.c_str(), request.camera_id.c_str(), "queued",
            0LL, 0LL, request.initial_occupancy, 0LL, 0LL, request.initial_occupancy,
            0, 0LL, create_time_ms, request.config_version.c_str());
        if (replyIsError(realtime_reply.get(), error, context_) ||
            !expireKey(context_, realtime_key, active_ttl, error)) {
            rollback();
            return false;
        }

        RedisReplyPtr stream_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "XADD %s * task_id %s task_kind %s model_type %s session_id %s people_flow_camera_id %s camera_profile %s source_type %s source_ref %s masked_uri %s config_version %s snapshot_path %s initial_occupancy %lld create_time_ms %lld",
            config_.stream_key.c_str(), request.session_id.c_str(), "people_flow", "detect",
            request.session_id.c_str(), request.camera_id.c_str(), request.camera_profile.c_str(),
            "rtsp", request.source_ref.c_str(), request.masked_uri.c_str(),
            request.config_version.c_str(), request.snapshot_path.c_str(), request.initial_occupancy,
            create_time_ms);
        if (replyIsError(stream_reply.get(), error, context_)) {
            rollback();
            return false;
        }
        if (config_.stream_max_len > 0) {
            RedisReplyPtr trim_reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "XTRIM %s MAXLEN ~ %lld", config_.stream_key.c_str(), config_.stream_max_len);
            if (replyIsError(trim_reply.get(), error, context_)) {
                rollback();
                return false;
            }
        }
        std::string index_error;
        RedisReplyPtr index_reply = commandWithReconnectLocked(config_, context_, index_error, 10000,
            "SET %s %s EX %d", peopleFlowLastKey(request.camera_id).c_str(),
            request.session_id.c_str(), session_ttl);
        (void)index_reply;
        return true;
    }

    bool RedisTaskQueue::updatePeopleFlowSession(
        const PeopleFlowSessionStatus& status,
        int session_ttl_seconds,
        int realtime_ttl_seconds,
        std::string& error
    ) const {
        if (status.session_id.empty() || status.camera_id.empty()) {
            error = "people-flow status requires session_id and camera_id";
            return false;
        }
        const long long update_ms = status.last_update_ms > 0 ? status.last_update_ms : nowMs();
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr session_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s status %s camera_id %s camera_profile %s masked_uri %s config_version %s snapshot_path %s capture_state %s capture_backend %s worker_id %d consumer_name %s stop_requested %d storage_degraded %d snapshot_degraded %d resolution_changed %d create_time_ms %lld start_time_ms %lld stop_time_ms %lld last_update_ms %lld last_frame_time_ms %lld latest_frame_age_ms %lld frame_count %lld dropped_frames %lld event_queue_depth %lld in_count %lld out_count %lld occupancy %lld applied_calibration_version %lld live_persons %d reconnect_count %d width %d height %d capture_fps %.6f infer_fps %.6f source_fps %.6f last_inference_ms %.6f error %s last_error %s",
            peopleFlowSessionKey(status.session_id).c_str(), status.status.c_str(), status.camera_id.c_str(),
            status.camera_profile.c_str(), status.masked_uri.c_str(), status.config_version.c_str(),
            status.snapshot_path.c_str(), status.capture_state.c_str(), status.capture_backend.c_str(),
            status.worker_id, status.consumer_name.c_str(), status.stop_requested ? 1 : 0,
            status.storage_degraded ? 1 : 0, status.snapshot_degraded ? 1 : 0,
            status.resolution_changed ? 1 : 0, status.create_time_ms, status.start_time_ms,
            status.stop_time_ms, update_ms, status.last_frame_time_ms, status.latest_frame_age_ms,
            status.frame_count, status.dropped_frames, status.event_queue_depth, status.in_count,
            status.out_count, status.occupancy, status.applied_calibration_version,
            status.live_persons, status.reconnect_count,
            status.width, status.height, status.capture_fps, status.infer_fps, status.source_fps,
            status.last_inference_ms, status.error.c_str(), status.last_error.c_str());
        if (replyIsError(session_reply.get(), error, context_)) return false;
        if (!expireKey(context_, peopleFlowSessionKey(status.session_id), std::max(60, session_ttl_seconds), error)) return false;

        RedisReplyPtr realtime_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s session_id %s camera_id %s status %s in_count %lld out_count %lld occupancy %lld applied_calibration_version %lld live_persons %d frame_count %lld capture_state %s capture_backend %s capture_fps %.6f infer_fps %.6f source_fps %.6f latest_frame_age_ms %lld reconnect_count %d dropped_frames %lld storage_degraded %d event_queue_depth %lld last_update_ms %lld config_version %s",
            peopleFlowRealtimeKey(status.camera_id).c_str(), status.session_id.c_str(), status.camera_id.c_str(),
            status.status.c_str(), status.in_count, status.out_count, status.occupancy,
            status.applied_calibration_version, status.live_persons, status.frame_count,
            status.capture_state.c_str(),
            status.capture_backend.c_str(), status.capture_fps, status.infer_fps, status.source_fps,
            status.latest_frame_age_ms, status.reconnect_count, status.dropped_frames,
            status.storage_degraded ? 1 : 0, status.event_queue_depth, update_ms,
            status.config_version.c_str());
        if (replyIsError(realtime_reply.get(), error, context_)) return false;
        return expireKey(context_, peopleFlowRealtimeKey(status.camera_id), std::max(3, realtime_ttl_seconds), error);
    }

    namespace {
        void fillPeopleFlowStatusFromValues(
            const std::map<std::string, std::string>& values,
            PeopleFlowSessionStatus& status
        ) {
            auto get = [&values](const std::string& key) -> std::string {
                const auto it = values.find(key);
                return it == values.end() ? std::string{} : it->second;
            };
            status.session_id = get("session_id");
            status.camera_id = get("camera_id");
            status.camera_profile = get("camera_profile");
            status.masked_uri = get("masked_uri");
            status.config_version = get("config_version");
            status.status = get("status");
            status.snapshot_path = get("snapshot_path");
            status.capture_state = get("capture_state");
            status.capture_backend = get("capture_backend");
            status.consumer_name = get("consumer_name");
            status.error = get("error");
            status.last_error = get("last_error");
            status.worker_id = static_cast<int>(parseLongLong(get("worker_id")));
            status.stop_requested = parseLongLong(get("stop_requested")) != 0;
            status.storage_degraded = parseLongLong(get("storage_degraded")) != 0;
            status.snapshot_degraded = parseLongLong(get("snapshot_degraded")) != 0;
            status.resolution_changed = parseLongLong(get("resolution_changed")) != 0;
            status.create_time_ms = parseLongLong(get("create_time_ms"));
            status.start_time_ms = parseLongLong(get("start_time_ms"));
            status.stop_time_ms = parseLongLong(get("stop_time_ms"));
            status.last_update_ms = parseLongLong(get("last_update_ms"));
            status.last_frame_time_ms = parseLongLong(get("last_frame_time_ms"));
            status.latest_frame_age_ms = parseLongLong(get("latest_frame_age_ms"), -1);
            status.frame_count = parseLongLong(get("frame_count"));
            status.dropped_frames = parseLongLong(get("dropped_frames"));
            status.event_queue_depth = parseLongLong(get("event_queue_depth"));
            status.in_count = parseLongLong(get("in_count"));
            status.out_count = parseLongLong(get("out_count"));
            status.initial_occupancy = parseLongLong(get("initial_occupancy"));
            status.occupancy = parseLongLong(get("occupancy"));
            status.applied_calibration_version = parseLongLong(get("applied_calibration_version"));
            status.live_persons = static_cast<int>(parseLongLong(get("live_persons")));
            status.reconnect_count = static_cast<int>(parseLongLong(get("reconnect_count")));
            status.width = static_cast<int>(parseLongLong(get("width")));
            status.height = static_cast<int>(parseLongLong(get("height")));
            status.capture_fps = parseDouble(get("capture_fps"));
            status.infer_fps = parseDouble(get("infer_fps"));
            status.source_fps = parseDouble(get("source_fps"));
            status.last_inference_ms = parseDouble(get("last_inference_ms"));
        }
    }

    bool RedisTaskQueue::getPeopleFlowSession(const std::string& session_id, PeopleFlowSessionStatus& status, std::string& error) const {
        status = PeopleFlowSessionStatus{};
        status.session_id = session_id;
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGETALL %s", peopleFlowSessionKey(session_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        const auto values = parseHashReply(reply.get());
        if (values.empty()) return true;
        status.found = true;
        fillPeopleFlowStatusFromValues(values, status);
        if (status.session_id.empty()) status.session_id = session_id;
        return true;
    }

    bool RedisTaskQueue::getPeopleFlowRealtime(const std::string& camera_id, PeopleFlowSessionStatus& status, std::string& error) const {
        status = PeopleFlowSessionStatus{};
        status.camera_id = camera_id;
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGETALL %s", peopleFlowRealtimeKey(camera_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        const auto values = parseHashReply(reply.get());
        if (values.empty()) return true;
        status.found = true;
        fillPeopleFlowStatusFromValues(values, status);
        if (status.camera_id.empty()) status.camera_id = camera_id;
        return true;
    }

    bool RedisTaskQueue::getActivePeopleFlowSession(const std::string& camera_id, std::string& session_id, std::string& error) const {
        session_id.clear();
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "GET %s", peopleFlowActiveKey(camera_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type != REDIS_REPLY_NIL) session_id = replyString(reply.get());
        return true;
    }

    bool RedisTaskQueue::getLastPeopleFlowSession(const std::string& camera_id, std::string& session_id, std::string& error) const {
        session_id.clear();
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "GET %s", peopleFlowLastKey(camera_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type != REDIS_REPLY_NIL) session_id = replyString(reply.get());
        return true;
    }

    bool RedisTaskQueue::refreshPeopleFlowLease(const std::string& camera_id, const std::string& session_id, int ttl_seconds, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "EVAL %s 1 %s %s %d",
            "if redis.call('GET',KEYS[1])==ARGV[1] then return redis.call('EXPIRE',KEYS[1],ARGV[2]) else return 0 end",
            peopleFlowActiveKey(camera_id).c_str(), session_id.c_str(), std::max(10, ttl_seconds));
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 0) {
            error = "people-flow active lease is missing or owned by another session";
            return false;
        }
        return true;
    }

    bool RedisTaskQueue::releasePeopleFlowLease(const std::string& camera_id, const std::string& session_id, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        return releaseActiveStreamIfMatchedLocked(config_, context_, peopleFlowActiveKey(camera_id), session_id, error);
    }

    bool RedisTaskQueue::requestStopPeopleFlow(const std::string& session_id, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s stop_requested %d status %s last_update_ms %lld",
            peopleFlowSessionKey(session_id).c_str(), 1, "stopping", nowMs());
        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::isPeopleFlowStopRequested(const std::string& session_id, bool& stop_requested, std::string& error) const {
        stop_requested = false;
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HGET %s stop_requested", peopleFlowSessionKey(session_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type != REDIS_REPLY_NIL) stop_requested = parseLongLong(replyString(reply.get())) != 0;
        return true;
    }

    bool RedisTaskQueue::appendPeopleFlowEvent(const CrossingEvent& event, int max_len, std::string& error) const {
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply;
        if (max_len > 0) {
            reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "XADD %s MAXLEN ~ %d * event_id %s session_id %s camera_id %s line_id %s track_id %lld direction %s event_time_ms %lld confidence %.6f point_x_norm %.8f point_y_norm %.8f config_version %s",
                peopleFlowEventsKey(event.camera_id).c_str(), max_len, event.event_id.c_str(),
                event.session_id.c_str(), event.camera_id.c_str(), event.line_id.c_str(),
                static_cast<long long>(event.track_id), event.direction.c_str(), event.event_time_ms,
                event.confidence, event.point_x_norm, event.point_y_norm, event.config_version.c_str());
        }
        else {
            reply = commandWithReconnectLocked(config_, context_, error, 10000,
                "XADD %s * event_id %s session_id %s camera_id %s line_id %s track_id %lld direction %s event_time_ms %lld confidence %.6f point_x_norm %.8f point_y_norm %.8f config_version %s",
                peopleFlowEventsKey(event.camera_id).c_str(), event.event_id.c_str(),
                event.session_id.c_str(), event.camera_id.c_str(), event.line_id.c_str(),
                static_cast<long long>(event.track_id), event.direction.c_str(), event.event_time_ms,
                event.confidence, event.point_x_norm, event.point_y_norm, event.config_version.c_str());
        }
        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::requestPeopleFlowCalibration(
        const std::string& camera_id,
        const std::string& session_id,
        long long occupancy,
        long long& calibration_version,
        std::string& error
    ) const {
        calibration_version = 0;
        if (camera_id.empty() || session_id.empty() || occupancy < 0) {
            error = "invalid people-flow calibration request";
            return false;
        }
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "EVAL %s 1 %s %s %lld",
            "if redis.call('HGET',KEYS[1],'session_id')~=ARGV[1] then return -1 end "
            "local v=redis.call('HINCRBY',KEYS[1],'requested_calibration_version',1) "
            "redis.call('HSET',KEYS[1],'requested_occupancy',ARGV[2]) return v",
            peopleFlowRealtimeKey(camera_id).c_str(), session_id.c_str(), occupancy);
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type != REDIS_REPLY_INTEGER || reply->integer < 0) {
            error = "people-flow calibration session is no longer active";
            return false;
        }
        calibration_version = static_cast<long long>(reply->integer);
        RedisReplyPtr session_reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HSET %s requested_calibration_version %lld requested_occupancy %lld",
            peopleFlowSessionKey(session_id).c_str(), calibration_version, occupancy);
        return !replyIsError(session_reply.get(), error, context_);
    }

    bool RedisTaskQueue::getPeopleFlowCalibrationRequest(
        const std::string& camera_id,
        long long& calibration_version,
        long long& occupancy,
        std::string& error
    ) const {
        calibration_version = 0;
        occupancy = 0;
        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(config_, context_, error, 10000,
            "HMGET %s requested_calibration_version requested_occupancy",
            peopleFlowRealtimeKey(camera_id).c_str());
        if (replyIsError(reply.get(), error, context_)) return false;
        if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            error = "invalid people-flow calibration reply";
            return false;
        }
        calibration_version = parseLongLong(replyString(reply->element[0]));
        occupancy = parseLongLong(replyString(reply->element[1]));
        return true;
    }

    bool RedisTaskQueue::getTaskStatus(const std::string& task_id, RedisTaskStatus& status, std::string& error) const {
        status = RedisTaskStatus{};
        status.task_id = task_id;

        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string status_key = statusKey(task_id);
        const std::string result_key = resultKey(task_id);
        const std::string meta_key = metaKey(task_id);

        RedisReplyPtr status_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "GET %s",
            status_key.c_str()
        );
        if (replyIsError(status_reply.get(), error, context_)) {
            return false;
        }
        if (status_reply->type == REDIS_REPLY_NIL) {
            status.found = false;
            return true;
        }

        status.found = true;
        status.status = replyString(status_reply.get());

        RedisReplyPtr meta_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HGETALL %s",
            meta_key.c_str()
        );
        if (!replyIsError(meta_reply.get(), error, context_)) {
            const auto values = parseHashReply(meta_reply.get());
            auto getValue = [&values](const std::string& key) -> std::string {
                auto it = values.find(key);
                return it == values.end() ? std::string{} : it->second;
                };
            status.error = getValue("error");
            status.task_kind = getValue("task_kind");
            status.model_type = getValue("model_type");
            status.input_image_key = getValue("input_image_key");
            status.result_image_key = getValue("result_image_key");
            status.result_image_path = getValue("result_image_path");
            status.result_image_filename = getValue("result_image_filename");
            status.input_video_path = getValue("input_video_path");
            status.output_video_path = getValue("output_video_path");
            status.output_video_filename = getValue("output_video_filename");
            status.worker_id = getValue("worker_id");
            status.consumer_name = getValue("consumer_name");
            status.create_time_ms = parseLongLong(getValue("create_time_ms"));
            status.start_time_ms = parseLongLong(getValue("start_time_ms"));
            status.finish_time_ms = parseLongLong(getValue("finish_time_ms"));
            status.queue_wait_ms = parseLongLong(getValue("queue_wait_ms"));
            status.infer_ms = parseDouble(getValue("infer_ms"));
            status.total_ms = parseLongLong(getValue("total_ms"));
            status.total_frames = parseLongLong(getValue("total_frames"));
            status.processed_frames = parseLongLong(getValue("processed_frames"));
            status.current_frame_index = parseLongLong(getValue("current_frame_index"));
            status.progress = parseDouble(getValue("progress"));
            status.fps = parseDouble(getValue("fps"));
            status.video_width = static_cast<int>(parseLongLong(getValue("video_width")));
            status.video_height = static_cast<int>(parseLongLong(getValue("video_height")));
            status.duration_ms = parseLongLong(getValue("duration_ms"));
            status.process_ms = parseLongLong(getValue("process_ms"));
            status.cancel_requested = parseLongLong(getValue("cancel_requested")) != 0;
        }

        if (status.status == "done" || status.status == "canceled") {
            RedisReplyPtr result_reply = commandWithReconnectLocked(
                config_,
                context_,
                error,
                10000,
                "GET %s",
                result_key.c_str()
            );
            if (replyIsError(result_reply.get(), error, context_)) {
                return false;
            }
            if (result_reply->type != REDIS_REPLY_NIL) {
                status.result_json_text = replyString(result_reply.get());
            }
        }

        return true;
    }

    bool RedisTaskQueue::popTask(RedisTask& task, std::string& error) const {
        task = RedisTask{};
        error.clear();

        std::lock_guard<std::mutex> lock(context_mutex_);

        const int pop_timeout_ms = std::max(config_.block_ms + 5000, 8000);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            pop_timeout_ms,
            "XREADGROUP GROUP %s %s COUNT 1 BLOCK %d STREAMS %s >",
            config_.consumer_group.c_str(),
            config_.consumer_name.c_str(),
            config_.block_ms,
            config_.stream_key.c_str()
        );

        if (reply == nullptr) {
            error = contextError(context_, "empty redis reply when reading stream");
            return false;
        }

        if (reply->type == REDIS_REPLY_NIL) {
            error.clear();
            return false;
        }

        if (reply->type == REDIS_REPLY_ERROR) {
            error = replyString(reply.get());
            if (containsText(error, "NOGROUP")) {
                std::string group_error;
                if (ensureConsumerGroup(context_, config_, group_error)) {
                    error.clear();
                }
            }
            return false;
        }

        if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
            error.clear();
            return false;
        }

        redisReply* stream_reply = reply->element[0];
        if (stream_reply == nullptr || stream_reply->type != REDIS_REPLY_ARRAY || stream_reply->elements < 2) {
            error = "invalid redis stream reply";
            return false;
        }

        redisReply* entries_reply = stream_reply->element[1];
        if (entries_reply == nullptr || entries_reply->type != REDIS_REPLY_ARRAY || entries_reply->elements == 0) {
            error.clear();
            return false;
        }

        return fillTaskFromStreamEntry(entries_reply->element[0], task, error);
    }

    bool RedisTaskQueue::claimPendingTask(RedisTask& task, std::string& error) const {
        task = RedisTask{};
        error.clear();

        if (!config_.enable_pending_reclaim) {
            return false;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);

        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "XAUTOCLAIM %s %s %s %lld 0-0 COUNT 1",
            config_.stream_key.c_str(),
            config_.consumer_group.c_str(),
            config_.consumer_name.c_str(),
            config_.pending_min_idle_ms
        );

        if (reply == nullptr) {
            error = contextError(context_, "empty redis reply when XAUTOCLAIM");
            return false;
        }

        if (reply->type == REDIS_REPLY_ERROR) {
            error = replyString(reply.get());
            if (containsText(error, "NOGROUP")) {
                std::string group_error;
                if (ensureConsumerGroup(context_, config_, group_error)) {
                    error.clear();
                }
            }
            return false;
        }

        if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
            error = "invalid XAUTOCLAIM reply";
            return false;
        }

        redisReply* entries_reply = reply->element[1];
        if (entries_reply == nullptr || entries_reply->type != REDIS_REPLY_ARRAY || entries_reply->elements == 0) {
            error.clear();
            return false;
        }

        return fillTaskFromStreamEntry(entries_reply->element[0], task, error);
    }

    bool RedisTaskQueue::ackTask(const std::string& stream_id, std::string& error) const {
        if (stream_id.empty()) {
            return true;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);

        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "XACK %s %s %s",
            config_.stream_key.c_str(),
            config_.consumer_group.c_str(),
            stream_id.c_str()
        );

        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::getStreamStats(RedisStreamStats& stats, std::string& error) const {
        stats = RedisStreamStats{};

        std::lock_guard<std::mutex> lock(context_mutex_);

        RedisReplyPtr xlen_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "XLEN %s",
            config_.stream_key.c_str()
        );
        if (replyIsError(xlen_reply.get(), error, context_)) {
            error = "XLEN failed: " + error;
            return false;
        }
        if (xlen_reply->type == REDIS_REPLY_INTEGER) {
            stats.stream_len = xlen_reply->integer;
        }

        RedisReplyPtr pending_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "XPENDING %s %s",
            config_.stream_key.c_str(),
            config_.consumer_group.c_str()
        );
        if (replyIsError(pending_reply.get(), error, context_)) {
            error = "XPENDING failed: " + error;
            return false;
        }
        if (pending_reply->type == REDIS_REPLY_ARRAY && pending_reply->elements >= 1) {
            redisReply* total_reply = pending_reply->element[0];
            if (total_reply != nullptr && total_reply->type == REDIS_REPLY_INTEGER) {
                stats.pending = total_reply->integer;
            }
        }

        return true;
    }


    bool RedisTaskQueue::getRuntimeMetrics(RedisRuntimeMetrics& metrics, std::string& error) const {
        metrics = RedisRuntimeMetrics{};
        metrics.recent_window_seconds = config_.metrics_recent_window_seconds;

        std::lock_guard<std::mutex> lock(context_mutex_);

        const std::string global_key = metricsGlobalKey();
        const std::string worker_done_key = metricsWorkerDoneKey();
        const std::string worker_failed_key = metricsWorkerFailedKey();
        const std::string recent_key = metricsRecentDoneKey();

        RedisReplyPtr global_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HGETALL %s",
            global_key.c_str()
        );
        if (replyIsError(global_reply.get(), error, context_)) {
            error = "HGETALL metrics global failed: " + error;
            return false;
        }

        const auto global_values = parseHashReply(global_reply.get());
        metrics.found = hashHasAnyField(global_values);
        auto getGlobal = [&global_values](const std::string& key) -> std::string {
            auto it = global_values.find(key);
            return it == global_values.end() ? std::string{} : it->second;
            };

        metrics.done_count = parseLongLong(getGlobal("done_count"));
        metrics.failed_count = parseLongLong(getGlobal("failed_count"));
        metrics.total_count = metrics.done_count + metrics.failed_count;
        metrics.last_finish_time_ms = parseLongLong(getGlobal("last_finish_time_ms"));
        metrics.last_task_id = getGlobal("last_task_id");

        const double total_queue = parseDouble(getGlobal("total_queue_wait_ms"));
        const double total_infer = parseDouble(getGlobal("total_inference_ms"));
        const double total_total = parseDouble(getGlobal("total_total_ms"));
        if (metrics.done_count > 0) {
            metrics.avg_queue_wait_ms = total_queue / static_cast<double>(metrics.done_count);
            metrics.avg_inference_ms = total_infer / static_cast<double>(metrics.done_count);
            metrics.avg_total_ms = total_total / static_cast<double>(metrics.done_count);
        }

        const long long now_ms = nowMs();
        const long long min_score = now_ms - static_cast<long long>(metrics.recent_window_seconds) * 1000LL;
        RedisReplyPtr trim_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "ZREMRANGEBYSCORE %s 0 %lld",
            recent_key.c_str(),
            min_score
        );
        if (replyIsError(trim_reply.get(), error, context_)) {
            error = "ZREMRANGEBYSCORE metrics failed: " + error;
            return false;
        }

        RedisReplyPtr recent_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "ZCARD %s",
            recent_key.c_str()
        );
        if (replyIsError(recent_reply.get(), error, context_)) {
            error = "ZCARD metrics failed: " + error;
            return false;
        }
        if (recent_reply->type == REDIS_REPLY_INTEGER) {
            metrics.recent_done_count = recent_reply->integer;
        }
        if (metrics.recent_window_seconds > 0) {
            metrics.qps_recent = static_cast<double>(metrics.recent_done_count) /
                static_cast<double>(metrics.recent_window_seconds);
        }

        RedisReplyPtr worker_done_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HGETALL %s",
            worker_done_key.c_str()
        );
        if (replyIsError(worker_done_reply.get(), error, context_)) {
            error = "HGETALL worker done metrics failed: " + error;
            return false;
        }
        const auto worker_done_values = parseHashReply(worker_done_reply.get());
        for (const auto& kv : worker_done_values) {
            metrics.worker_done_count[kv.first] = parseLongLong(kv.second);
        }

        RedisReplyPtr worker_failed_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HGETALL %s",
            worker_failed_key.c_str()
        );
        if (replyIsError(worker_failed_reply.get(), error, context_)) {
            error = "HGETALL worker failed metrics failed: " + error;
            return false;
        }
        const auto worker_failed_values = parseHashReply(worker_failed_reply.get());
        for (const auto& kv : worker_failed_values) {
            metrics.worker_failed_count[kv.first] = parseLongLong(kv.second);
        }

        return true;
    }

    bool RedisTaskQueue::deleteKey(const std::string& key, std::string& error) const {
        if (key.empty()) {
            return true;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "DEL %s",
            key.c_str()
        );
        return !replyIsError(reply.get(), error, context_);
    }

    bool RedisTaskQueue::getRedisMemoryStats(RedisMemoryStats& stats, std::string& error) const {
        stats = RedisMemoryStats{};

        std::lock_guard<std::mutex> lock(context_mutex_);
        RedisReplyPtr reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "INFO memory"
        );
        if (replyIsError(reply.get(), error, context_)) {
            error = "INFO memory failed: " + error;
            return false;
        }
        if (reply->type != REDIS_REPLY_STRING) {
            error = "INFO memory returned non-string reply";
            return false;
        }

        const auto values = parseInfoText(replyString(reply.get()));
        auto getValue = [&values](const std::string& key) -> std::string {
            auto it = values.find(key);
            return it == values.end() ? std::string{} : it->second;
            };

        stats.found = true;
        stats.used_memory_bytes = parseLongLong(getValue("used_memory"));
        stats.used_memory_mb = static_cast<double>(stats.used_memory_bytes) / 1024.0 / 1024.0;
        stats.used_memory_human = getValue("used_memory_human");
        stats.maxmemory_bytes = parseLongLong(getValue("maxmemory"));
        stats.maxmemory_mb = static_cast<double>(stats.maxmemory_bytes) / 1024.0 / 1024.0;
        stats.maxmemory_human = getValue("maxmemory_human");
        return true;
    }

    bool RedisTaskQueue::writeWorkerHeartbeat(const WorkerHeartbeatRecord& heartbeat, int ttl_seconds, std::string& error) const {
        if (heartbeat.consumer_name.empty()) {
            error = "empty heartbeat consumer_name";
            return false;
        }
        if (ttl_seconds <= 0) {
            ttl_seconds = 15;
        }

        const std::string key = workerHeartbeatKey(heartbeat.consumer_name);

        std::lock_guard<std::mutex> lock(context_mutex_);
        // Use %b for binary-safe string arguments. This preserves empty strings
        // such as current_task_id="" and last_error="" and avoids accidental field shifts.
        RedisReplyPtr hset_reply = commandWithReconnectLocked(
            config_,
            context_,
            error,
            10000,
            "HSET %b "
            "consumer_name %b "
            "pid %b "
            "host %b "
            "worker_id %d "
            "gpu_id %d "
            "model_type %b "
            "runner_model_type %b "
            "worker_group %b "
            "worker_kind %b "
            "task_kind %b "
            "stream_type %b "
            "engine_path %b "
            "labels_path %b "
            "max_concurrency %d "
            "status %b "
            "current_task_id %b "
            "processed_count %lld "
            "failed_count %lld "
            "start_time_ms %lld "
            "last_heartbeat_ms %lld "
            "last_error %b",
            key.c_str(), key.size(),
            heartbeat.consumer_name.c_str(), heartbeat.consumer_name.size(),
            heartbeat.pid.c_str(), heartbeat.pid.size(),
            heartbeat.host.c_str(), heartbeat.host.size(),
            heartbeat.worker_id,
            heartbeat.gpu_id,
            heartbeat.model_type.c_str(), heartbeat.model_type.size(),
            heartbeat.runner_model_type.c_str(), heartbeat.runner_model_type.size(),
            heartbeat.worker_group.c_str(), heartbeat.worker_group.size(),
            heartbeat.worker_kind.c_str(), heartbeat.worker_kind.size(),
            heartbeat.task_kind.c_str(), heartbeat.task_kind.size(),
            heartbeat.stream_type.c_str(), heartbeat.stream_type.size(),
            heartbeat.engine_path.c_str(), heartbeat.engine_path.size(),
            heartbeat.labels_path.c_str(), heartbeat.labels_path.size(),
            heartbeat.max_concurrency,
            heartbeat.status.c_str(), heartbeat.status.size(),
            heartbeat.current_task_id.c_str(), heartbeat.current_task_id.size(),
            heartbeat.processed_count,
            heartbeat.failed_count,
            heartbeat.start_time_ms,
            heartbeat.last_heartbeat_ms,
            heartbeat.last_error.c_str(), heartbeat.last_error.size()
        );
        if (replyIsError(hset_reply.get(), error, context_)) {
            return false;
        }

        return expireKey(context_, key, ttl_seconds, error);
    }

    bool RedisTaskQueue::getWorkerHeartbeats(
        const std::string& consumer_name_prefix,
        int expected_worker_num,
        std::vector<WorkerHeartbeatRecord>& workers,
        std::string& error
    ) const {
        workers.clear();
        if (expected_worker_num <= 0) {
            return true;
        }

        std::lock_guard<std::mutex> lock(context_mutex_);
        const long long now_ms = nowMs();

        for (int i = 1; i <= expected_worker_num; ++i) {
            WorkerHeartbeatRecord record;
            record.consumer_name = consumer_name_prefix + std::to_string(i);
            record.heartbeat_key = workerHeartbeatKey(record.consumer_name);

            RedisReplyPtr reply = commandWithReconnectLocked(
                config_,
                context_,
                error,
                10000,
                "HGETALL %s",
                record.heartbeat_key.c_str()
            );
            if (replyIsError(reply.get(), error, context_)) {
                return false;
            }

            const auto values = parseHashReply(reply.get());
            record.found = hashHasAnyField(values);
            record.alive = record.found;

            if (record.found) {
                auto getValue = [&values](const std::string& key) -> std::string {
                    auto it = values.find(key);
                    return it == values.end() ? std::string{} : it->second;
                    };
                const std::string consumer = getValue("consumer_name");
                if (!consumer.empty()) {
                    record.consumer_name = consumer;
                }
                record.pid = getValue("pid");
                record.host = getValue("host");
                record.worker_id = static_cast<int>(parseLongLong(getValue("worker_id")));
                record.gpu_id = static_cast<int>(parseLongLong(getValue("gpu_id")));
                record.model_type = getValue("model_type");
                record.runner_model_type = getValue("runner_model_type");
                record.worker_group = getValue("worker_group");
                record.worker_kind = getValue("worker_kind");
                record.task_kind = getValue("task_kind");
                record.stream_type = getValue("stream_type");
                record.engine_path = getValue("engine_path");
                record.labels_path = getValue("labels_path");
                record.max_concurrency = static_cast<int>(parseLongLong(getValue("max_concurrency")));
                if (record.max_concurrency <= 0) {
                    record.max_concurrency = 1;
                }
                record.status = getValue("status");
                record.current_task_id = getValue("current_task_id");
                record.processed_count = parseLongLong(getValue("processed_count"));
                record.failed_count = parseLongLong(getValue("failed_count"));
                record.start_time_ms = parseLongLong(getValue("start_time_ms"));
                record.last_heartbeat_ms = parseLongLong(getValue("last_heartbeat_ms"));
                record.last_error = getValue("last_error");
                if (record.last_heartbeat_ms > 0) {
                    record.last_heartbeat_age_ms = std::max(0LL, now_ms - record.last_heartbeat_ms);
                }
            }

            workers.push_back(record);
        }

        return true;
    }

    std::string RedisTaskQueue::inputImageKey(const std::string& task_id) const {
        return "yolo:image:" + task_id + ":input";
    }

    std::string RedisTaskQueue::resultImageKey(const std::string& task_id) const {
        return "yolo:image:" + task_id + ":result";
    }

    std::string RedisTaskQueue::workerHeartbeatKey(const std::string& consumer_name) const {
        return "yolo:worker:" + consumer_name + ":heartbeat";
    }


    std::string RedisTaskQueue::metricsGlobalKey() const {
        return "yolo:metrics:" + sanitizeRedisKeyPart(config_.stream_key) + ":global";
    }

    std::string RedisTaskQueue::metricsWorkerDoneKey() const {
        return "yolo:metrics:" + sanitizeRedisKeyPart(config_.stream_key) + ":worker:done";
    }

    std::string RedisTaskQueue::metricsWorkerFailedKey() const {
        return "yolo:metrics:" + sanitizeRedisKeyPart(config_.stream_key) + ":worker:failed";
    }

    std::string RedisTaskQueue::metricsRecentDoneKey() const {
        return "yolo:metrics:" + sanitizeRedisKeyPart(config_.stream_key) + ":recent:done";
    }

    std::string RedisTaskQueue::streamTaskStatusKey(const std::string& stream_id) const {
        return "yolo:streamtask:" + stream_id + ":status";
    }

    std::string RedisTaskQueue::streamTaskMetaKey(const std::string& stream_id) const {
        return "yolo:streamtask:" + stream_id + ":meta";
    }

    std::string RedisTaskQueue::streamTaskLatestKey(const std::string& stream_id) const {
        return "yolo:streamtask:" + stream_id + ":latest";
    }

    std::string RedisTaskQueue::activeStreamTaskKey() const {
        return "yolo:streamtask:" + sanitizeRedisKeyPart(config_.stream_key) + ":active";
    }

    std::string RedisTaskQueue::peopleFlowSessionKey(const std::string& session_id) const {
        return "yolo:pf:session:" + sanitizeRedisKeyPart(session_id);
    }

    std::string RedisTaskQueue::peopleFlowActiveKey(const std::string& camera_id) const {
        return "yolo:pf:active:" + sanitizeRedisKeyPart(camera_id);
    }

    std::string RedisTaskQueue::peopleFlowLastKey(const std::string& camera_id) const {
        return "yolo:pf:last:" + sanitizeRedisKeyPart(camera_id);
    }

    std::string RedisTaskQueue::peopleFlowRealtimeKey(const std::string& camera_id) const {
        return "yolo:pf:realtime:" + sanitizeRedisKeyPart(camera_id);
    }

    std::string RedisTaskQueue::peopleFlowEventsKey(const std::string& camera_id) const {
        return "yolo:pf:events:" + sanitizeRedisKeyPart(camera_id);
    }

    std::string RedisTaskQueue::statusKey(const std::string& task_id) const {
        return "yolo:task:" + task_id + ":status";
    }

    std::string RedisTaskQueue::resultKey(const std::string& task_id) const {
        return "yolo:task:" + task_id + ":result";
    }

    std::string RedisTaskQueue::metaKey(const std::string& task_id) const {
        return "yolo:task:" + task_id + ":meta";
    }

}  // namespace yolo11_server
