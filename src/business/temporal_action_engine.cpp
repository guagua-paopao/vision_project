#include "business/temporal_action_engine.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace yolo11_server {

    FeatureThresholdTemporalClassifier::FeatureThresholdTemporalClassifier(
        std::string label,
        std::size_t feature_index,
        double confidence_scale
    ) : label_(std::move(label)),
        feature_index_(feature_index),
        confidence_scale_(confidence_scale) {
        if (label_.empty()) throw std::invalid_argument("demo temporal classifier label must not be empty");
    }

    std::string FeatureThresholdTemporalClassifier::name() const {
        return "feature-threshold-demo";
    }

    bool FeatureThresholdTemporalClassifier::ready() const {
        return true;
    }

    bool FeatureThresholdTemporalClassifier::isDemoClassifier() const {
        return true;
    }

    std::vector<TemporalActionPrediction> FeatureThresholdTemporalClassifier::classify(
        std::int64_t,
        const std::deque<TemporalFeatureFrame>& window
    ) {
        if (window.empty()) return {};
        double sum = 0.0;
        std::size_t count = 0;
        for (const TemporalFeatureFrame& frame : window) {
            if (feature_index_ >= frame.features.size()) continue;
            sum += frame.features[feature_index_];
            ++count;
        }
        if (count == 0) return {};
        return { { label_, std::clamp(sum / static_cast<double>(count) * confidence_scale_, 0.0, 1.0) } };
    }

    TemporalActionEngine::TemporalActionEngine(
        TemporalActionConfig config,
        std::unique_ptr<ITemporalActionClassifier> classifier,
        std::string session_id,
        std::string camera_id
    ) : config_(config),
        classifier_(std::move(classifier)),
        session_id_(std::move(session_id)),
        camera_id_(std::move(camera_id)) {
        if (config_.window_size == 0 || config_.min_samples == 0 ||
            config_.min_samples > config_.window_size) {
            throw std::invalid_argument("invalid temporal action window configuration");
        }
        if (config_.confirm_windows < 1 || config_.release_windows < 1) {
            throw std::invalid_argument("temporal confirmation counts must be positive");
        }
    }

    bool TemporalActionEngine::ready() const {
        return classifier_ != nullptr && classifier_->ready();
    }

    std::string TemporalActionEngine::classifierName() const {
        return classifier_ == nullptr ? "none" : classifier_->name();
    }

    SecurityEvent TemporalActionEngine::makeEvent(
        std::int64_t track_id,
        const std::string& event_type,
        long long event_time_ms,
        long long start_time_ms,
        double confidence
    ) {
        SecurityEvent event;
        event.event_id = "temporal_" + std::to_string(next_event_sequence_++);
        event.session_id = session_id_;
        event.camera_id = camera_id_;
        event.track_id = track_id;
        event.category = SecurityEventCategory::TemporalAction;
        event.event_type = event_type;
        event.start_time_ms = start_time_ms;
        event.end_time_ms = event_time_ms;
        event.event_time_ms = event_time_ms;
        event.confidence = std::clamp(confidence, 0.0, 1.0);
        event.severity = config_.severity;
        event.demo_classifier = classifier_ != nullptr && classifier_->isDemoClassifier();
        return event;
    }

    std::vector<SecurityEvent> TemporalActionEngine::update(
        std::int64_t track_id,
        TemporalFeatureFrame frame
    ) {
        std::vector<SecurityEvent> events;
        if (!ready() || track_id <= 0) return events;
        const long long timestamp_ms = frame.timestamp_ms;
        auto& window = windows_[track_id];
        if (!window.empty() && timestamp_ms <= window.back().timestamp_ms) return events;
        window.push_back(std::move(frame));
        while (window.size() > config_.window_size) window.pop_front();
        if (window.size() < config_.min_samples) return events;

        const std::vector<TemporalActionPrediction> predictions = classifier_->classify(track_id, window);
        std::map<std::string, double> confidence_by_label;
        for (const TemporalActionPrediction& prediction : predictions) {
            if (!prediction.label.empty()) {
                confidence_by_label[prediction.label] = std::clamp(prediction.confidence, 0.0, 1.0);
            }
        }
        for (const auto& [key, state] : states_) {
            if (key.first == track_id && !confidence_by_label.count(key.second)) {
                confidence_by_label[key.second] = 0.0;
            }
        }

        for (const auto& [label, confidence] : confidence_by_label) {
            LabelState& state = states_[{ track_id, label }];
            if (!state.active) {
                state.release_windows = 0;
                if (confidence >= config_.start_threshold &&
                    (state.last_end_ms == 0 || timestamp_ms - state.last_end_ms >= config_.cooldown_ms)) {
                    ++state.candidate_windows;
                    state.last_confidence = confidence;
                    if (state.candidate_windows >= config_.confirm_windows) {
                        state.active = true;
                        state.start_time_ms = timestamp_ms;
                        state.candidate_windows = 0;
                        events.push_back(makeEvent(
                            track_id, label + "_START", timestamp_ms, timestamp_ms, confidence));
                    }
                } else {
                    state.candidate_windows = 0;
                }
            } else {
                state.last_confidence = std::max(state.last_confidence, confidence);
                if (confidence <= config_.end_threshold) {
                    ++state.release_windows;
                    if (state.release_windows >= config_.release_windows) {
                        events.push_back(makeEvent(
                            track_id, label + "_END", timestamp_ms,
                            state.start_time_ms, state.last_confidence));
                        state.active = false;
                        state.release_windows = 0;
                        state.last_end_ms = timestamp_ms;
                    }
                } else {
                    state.release_windows = 0;
                }
            }
        }
        return events;
    }

    void TemporalActionEngine::reset() {
        windows_.clear();
        states_.clear();
    }

}  // namespace yolo11_server
