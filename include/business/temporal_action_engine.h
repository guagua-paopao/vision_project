#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "business/security_analytics_types.h"

namespace yolo11_server {

    class ITemporalActionClassifier {
    public:
        virtual ~ITemporalActionClassifier() = default;
        virtual std::string name() const = 0;
        virtual bool ready() const = 0;
        virtual bool isDemoClassifier() const { return false; }
        virtual std::vector<TemporalActionPrediction> classify(
            std::int64_t track_id,
            const std::deque<TemporalFeatureFrame>& window
        ) = 0;
    };

    // Deterministic classifier used only to exercise the Phase 4 integration
    // contract without pretending that a trained semantic action model exists.
    class FeatureThresholdTemporalClassifier final : public ITemporalActionClassifier {
    public:
        FeatureThresholdTemporalClassifier(
            std::string label,
            std::size_t feature_index = 0,
            double confidence_scale = 1.0
        );

        std::string name() const override;
        bool ready() const override;
        bool isDemoClassifier() const override;
        std::vector<TemporalActionPrediction> classify(
            std::int64_t track_id,
            const std::deque<TemporalFeatureFrame>& window
        ) override;

    private:
        std::string label_;
        std::size_t feature_index_ = 0;
        double confidence_scale_ = 1.0;
    };

    class TemporalActionEngine {
    public:
        TemporalActionEngine(
            TemporalActionConfig config,
            std::unique_ptr<ITemporalActionClassifier> classifier,
            std::string session_id,
            std::string camera_id
        );

        std::vector<SecurityEvent> update(
            std::int64_t track_id,
            TemporalFeatureFrame frame
        );

        bool ready() const;
        std::string classifierName() const;
        void reset();

    private:
        struct LabelState {
            bool active = false;
            int candidate_windows = 0;
            int release_windows = 0;
            long long start_time_ms = 0;
            long long last_end_ms = 0;
            double last_confidence = 0.0;
        };

        SecurityEvent makeEvent(
            std::int64_t track_id,
            const std::string& event_type,
            long long event_time_ms,
            long long start_time_ms,
            double confidence
        );

        TemporalActionConfig config_;
        std::unique_ptr<ITemporalActionClassifier> classifier_;
        std::string session_id_;
        std::string camera_id_;
        std::map<std::int64_t, std::deque<TemporalFeatureFrame>> windows_;
        std::map<std::pair<std::int64_t, std::string>, LabelState> states_;
        std::uint64_t next_event_sequence_ = 1;
    };

}  // namespace yolo11_server
