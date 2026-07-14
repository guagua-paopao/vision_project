#include "business/people_flow_renderer.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace yolo11_server {

    namespace {

        // All colors are OpenCV BGR values. Keeping them in one palette makes
        // screenshots and operator documentation use stable semantics.
        struct VisualPalette {
            const cv::Scalar raw_person{ 150, 150, 150 };
            const cv::Scalar accepted_high{ 255, 255, 0 };
            const cv::Scalar accepted_low{ 255, 0, 255 };
            const cv::Scalar rejected{ 0, 0, 150 };
            const cv::Scalar tentative{ 0, 165, 255 };
            const cv::Scalar confirmed{ 0, 220, 0 };
            const cv::Scalar missed{ 255, 80, 0 };
            const cv::Scalar roi{ 255, 180, 0 };
            const cv::Scalar counting_line{ 0, 255, 255 };
            const cv::Scalar hysteresis{ 0, 190, 255 };
            const cv::Scalar side_positive{ 80, 255, 80 };
            const cv::Scalar side_negative{ 255, 150, 80 };
            const cv::Scalar event_in{ 0, 255, 80 };
            const cv::Scalar event_out{ 0, 150, 255 };
            const cv::Scalar panel{ 20, 20, 20 };
            const cv::Scalar text{ 255, 255, 255 };
            const cv::Scalar secondary_text{ 220, 220, 220 };
        };

        const VisualPalette kColors;

        int clampCoordinate(int value, int extent) {
            return std::clamp(value, 0, std::max(0, extent - 1));
        }

        cv::Point toCanvasPoint(const PfPoint& point, const cv::Size& size) {
            const double safe_x = std::isfinite(point.x) ? point.x : 0.0;
            const double safe_y = std::isfinite(point.y) ? point.y : 0.0;
            return {
                clampCoordinate(static_cast<int>(std::lround(safe_x)), size.width),
                clampCoordinate(static_cast<int>(std::lround(safe_y)), size.height)
            };
        }

        cv::Point normalizedPoint(const NormalizedPoint& point, const cv::Size& size) {
            return {
                clampCoordinate(static_cast<int>(std::lround(point.x * size.width)), size.width),
                clampCoordinate(static_cast<int>(std::lround(point.y * size.height)), size.height)
            };
        }

        cv::Rect clippedRect(const PfRect& box, const cv::Size& size) {
            if (size.width <= 0 || size.height <= 0 ||
                !std::isfinite(box.x1) || !std::isfinite(box.y1) ||
                !std::isfinite(box.x2) || !std::isfinite(box.y2)) {
                return {};
            }
            const int x1 = std::clamp(static_cast<int>(std::floor(std::min(box.x1, box.x2))), 0, size.width);
            const int y1 = std::clamp(static_cast<int>(std::floor(std::min(box.y1, box.y2))), 0, size.height);
            const int x2 = std::clamp(static_cast<int>(std::ceil(std::max(box.x1, box.x2))), 0, size.width);
            const int y2 = std::clamp(static_cast<int>(std::ceil(std::max(box.y1, box.y2))), 0, size.height);
            if (x2 <= x1 || y2 <= y1) return {};
            return { x1, y1, x2 - x1, y2 - y1 };
        }

        void drawLabel(
            cv::Mat& canvas,
            const std::string& text,
            cv::Point origin,
            const cv::Scalar& color,
            double font_scale,
            int thickness = 1,
            bool background = true
        ) {
            if (canvas.empty() || text.empty()) return;
            int baseline = 0;
            const cv::Size text_size = cv::getTextSize(
                text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
            const int max_x = std::max(0, canvas.cols - text_size.width - 4);
            const int min_y = text_size.height + 3;
            const int max_y = std::max(min_y, canvas.rows - baseline - 2);
            origin.x = std::clamp(origin.x, 0, max_x);
            origin.y = std::clamp(origin.y, min_y, max_y);
            if (background) {
                const cv::Point top_left(
                    std::max(0, origin.x - 2),
                    std::max(0, origin.y - text_size.height - 2));
                const cv::Point bottom_right(
                    std::min(canvas.cols - 1, origin.x + text_size.width + 2),
                    std::min(canvas.rows - 1, origin.y + baseline + 2));
                if (bottom_right.x >= top_left.x && bottom_right.y >= top_left.y) {
                    cv::rectangle(canvas, top_left, bottom_right, kColors.panel, cv::FILLED);
                }
            }
            cv::putText(canvas, text, origin, cv::FONT_HERSHEY_SIMPLEX,
                font_scale, color, thickness, cv::LINE_AA);
        }

        double resolvedUiScale(const cv::Mat& canvas, const PeopleFlowVisualizationSection& visual) {
            if (visual.ui_scale > 0.0) return std::clamp(visual.ui_scale, 0.25, 4.0);
            return std::clamp(
                std::min(canvas.cols / 1280.0, canvas.rows / 720.0),
                0.35,
                1.0);
        }

        std::string shortDecision(DetectionFilterDecision decision) {
            switch (decision) {
            case DetectionFilterDecision::RejectedLowConfidence: return "CONF";
            case DetectionFilterDecision::RejectedInvalidBBox: return "BBOX";
            case DetectionFilterDecision::RejectedMinimumSize: return "SIZE";
            case DetectionFilterDecision::RejectedAspectRatio: return "ASPECT";
            case DetectionFilterDecision::RejectedRoi: return "ROI";
            case DetectionFilterDecision::AcceptedHigh: return "HIGH";
            case DetectionFilterDecision::AcceptedLow: return "LOW";
            }
            return "BBOX";
        }

        const TrackCounterDebugState* findCounterState(
            const PeopleFlowFrameDebug* debug,
            std::int64_t track_id
        ) {
            if (!debug) return nullptr;
            const auto it = std::find_if(debug->counter_states.begin(), debug->counter_states.end(),
                [track_id](const TrackCounterDebugState& state) { return state.track_id == track_id; });
            return it == debug->counter_states.end() ? nullptr : &*it;
        }

        cv::Scalar trackColor(const PersonTrack& track) {
            if (track.missed > 0 || !track.matched_this_frame) return kColors.missed;
            return track.confirmed ? kColors.confirmed : kColors.tentative;
        }

        bool trackVisible(const PersonTrack& track, const PeopleFlowVisualizationSection& visual) {
            if (track.missed > 0 || !track.matched_this_frame) return visual.draw_missed_tracks;
            if (track.confirmed) return visual.draw_confirmed_tracks;
            return visual.draw_tentative_tracks;
        }

        std::string trackPrefix(const PersonTrack& track) {
            if (track.missed > 0 || !track.matched_this_frame) return "PRED";
            return track.confirmed ? "CONF" : "TENT";
        }

        cv::Mat renderLegacy(
            const cv::Mat& frame,
            const std::vector<PersonTrack>& tracks,
            const PeopleFlowCounts& counts,
            const PeopleFlowRenderMetrics& metrics,
            const PeopleFlowSection& config
        ) {
            cv::Mat canvas = frame.clone();
            if (config.roi.enabled && config.roi.polygon_norm.size() >= 3) {
                std::vector<cv::Point> polygon;
                for (const NormalizedPoint& point : config.roi.polygon_norm) {
                    polygon.emplace_back(
                        static_cast<int>(point.x * canvas.cols),
                        static_cast<int>(point.y * canvas.rows)
                    );
                }
                cv::polylines(canvas, polygon, true, kColors.roi, 2, cv::LINE_AA);
            }
            const cv::Point line_a(
                static_cast<int>(config.counting.line_a_norm.x * canvas.cols),
                static_cast<int>(config.counting.line_a_norm.y * canvas.rows)
            );
            const cv::Point line_b(
                static_cast<int>(config.counting.line_b_norm.x * canvas.cols),
                static_cast<int>(config.counting.line_b_norm.y * canvas.rows)
            );
            cv::arrowedLine(canvas, line_a, line_b, kColors.counting_line, 3, cv::LINE_AA, 0, 0.05);
            cv::putText(canvas, config.counting.transition_positive_to_negative,
                (line_a + line_b) * 0.5, cv::FONT_HERSHEY_SIMPLEX, 0.7,
                kColors.counting_line, 2, cv::LINE_AA);
            for (const PersonTrack& track : tracks) {
                const cv::Scalar color = track.confirmed
                    ? cv::Scalar(0, 220, 0)
                    : cv::Scalar(0, 160, 255);
                cv::Rect box(
                    static_cast<int>(track.bbox.x1),
                    static_cast<int>(track.bbox.y1),
                    static_cast<int>(track.bbox.width()),
                    static_cast<int>(track.bbox.height())
                );
                box &= cv::Rect(0, 0, canvas.cols, canvas.rows);
                if (box.width > 0 && box.height > 0) cv::rectangle(canvas, box, color, 2, cv::LINE_AA);
                std::ostringstream label;
                label << "ID " << track.track_id << ' ' << std::fixed << std::setprecision(2) << track.confidence;
                cv::putText(canvas, label.str(), cv::Point(box.x, std::max(18, box.y - 5)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv::LINE_AA);
                for (std::size_t i = 1; i < track.trail.size(); ++i) {
                    cv::line(canvas,
                        cv::Point(static_cast<int>(track.trail[i - 1].x), static_cast<int>(track.trail[i - 1].y)),
                        cv::Point(static_cast<int>(track.trail[i].x), static_cast<int>(track.trail[i].y)),
                        color, 2, cv::LINE_AA);
                }
                cv::circle(canvas,
                    cv::Point(static_cast<int>(track.anchor_point.x), static_cast<int>(track.anchor_point.y)),
                    4, color, cv::FILLED, cv::LINE_AA);
            }

            cv::rectangle(canvas, cv::Rect(10, 10, std::min(560, canvas.cols - 20), 118),
                kColors.panel, cv::FILLED);
            std::ostringstream line1;
            line1 << "IN " << counts.in_count << "  OUT " << counts.out_count
                  << "  OCC " << counts.occupancy << "  LIVE " << counts.live_persons;
            cv::putText(canvas, line1.str(), cv::Point(24, 45), cv::FONT_HERSHEY_SIMPLEX,
                0.8, kColors.text, 2, cv::LINE_AA);
            std::ostringstream line2;
            line2 << std::fixed << std::setprecision(1)
                  << "capture " << metrics.capture_fps << " fps  infer " << metrics.infer_fps
                  << " fps  age " << metrics.latest_frame_age_ms << " ms";
            cv::putText(canvas, line2.str(), cv::Point(24, 78), cv::FONT_HERSHEY_SIMPLEX,
                0.58, cv::Scalar(230, 230, 230), 1, cv::LINE_AA);
            std::ostringstream line3;
            line3 << "capture=" << metrics.capture_state << " reconnect=" << metrics.reconnect_count
                  << " config=" << config.config_version;
            cv::putText(canvas, line3.str(), cv::Point(24, 108), cv::FONT_HERSHEY_SIMPLEX,
                0.52, cv::Scalar(200, 220, 255), 1, cv::LINE_AA);
            return canvas;
        }

    }  // namespace

    PeopleFlowRenderer::PeopleFlowRenderer(const PeopleFlowSection& config)
        : config_(config) {
    }

    cv::Mat PeopleFlowRenderer::render(
        const cv::Mat& frame,
        const std::vector<PersonTrack>& tracks,
        const PeopleFlowCounts& counts,
        const PeopleFlowRenderMetrics& metrics
    ) const {
        return render(frame, tracks, counts, metrics, nullptr);
    }

    void PeopleFlowRenderer::resetEventMarkers() const {
        event_markers_.clear();
    }

    cv::Mat PeopleFlowRenderer::render(
        const cv::Mat& frame,
        const std::vector<PersonTrack>& confirmed_tracks,
        const PeopleFlowCounts& counts,
        const PeopleFlowRenderMetrics& metrics,
        const PeopleFlowFrameDebug* debug
    ) const {
        if (frame.empty()) return {};
        if (!config_.visualization.enabled) {
            return renderLegacy(frame, confirmed_tracks, counts, metrics, config_);
        }

        cv::Mat canvas = frame.clone();
        const auto& visual = config_.visualization;
        const double ui_scale = resolvedUiScale(canvas, visual);
        const double font_scale = std::clamp(visual.font_scale * ui_scale, 0.25, 2.0);
        const int box_thickness = std::max(1, static_cast<int>(std::lround(visual.box_thickness * ui_scale)));
        const int line_thickness = std::max(1, static_cast<int>(std::lround(visual.line_thickness * ui_scale)));
        const int trail_thickness = std::max(1, static_cast<int>(std::lround(visual.trail_thickness * ui_scale)));
        const bool compact = visual.compact_panel_auto && (canvas.cols <= 640 || canvas.rows <= 480);

        std::vector<cv::Point> roi_polygon;
        if (config_.roi.enabled && config_.roi.polygon_norm.size() >= 3 &&
            (visual.draw_roi || visual.fill_roi)) {
            roi_polygon.reserve(config_.roi.polygon_norm.size());
            for (const NormalizedPoint& point : config_.roi.polygon_norm) {
                roi_polygon.push_back(normalizedPoint(point, canvas.size()));
            }
        }

        // 1-3: base frame, optional ROI fill, then ROI boundary.
        if (visual.fill_roi && roi_polygon.size() >= 3) {
            cv::Mat overlay = canvas.clone();
            cv::fillPoly(overlay, std::vector<std::vector<cv::Point>>{ roi_polygon }, kColors.roi, cv::LINE_AA);
            cv::addWeighted(overlay, 0.12, canvas, 0.88, 0.0, canvas);
        }
        if (visual.draw_roi && roi_polygon.size() >= 3) {
            cv::polylines(canvas, roi_polygon, true, kColors.roi, line_thickness, cv::LINE_AA);
        }

        const cv::Point line_a = normalizedPoint(config_.counting.line_a_norm, canvas.size());
        const cv::Point line_b = normalizedPoint(config_.counting.line_b_norm, canvas.size());
        const double line_dx = static_cast<double>(line_b.x - line_a.x);
        const double line_dy = static_cast<double>(line_b.y - line_a.y);
        const double line_length = std::hypot(line_dx, line_dy);
        if (line_length > 1e-6) {
            const double unit_x = line_dx / line_length;
            const double unit_y = line_dy / line_length;
            const double normal_x = -unit_y;
            const double normal_y = unit_x;

            // 4: semi-transparent finite-line extension guides.
            if (visual.draw_counting_line && config_.counting.finite_segment_extension_norm > 0.0) {
                const double extension = line_length * config_.counting.finite_segment_extension_norm;
                const cv::Point extension_a(
                    static_cast<int>(std::lround(line_a.x - unit_x * extension)),
                    static_cast<int>(std::lround(line_a.y - unit_y * extension)));
                const cv::Point extension_b(
                    static_cast<int>(std::lround(line_b.x + unit_x * extension)),
                    static_cast<int>(std::lround(line_b.y + unit_y * extension)));
                cv::Mat overlay = canvas.clone();
                cv::line(overlay, extension_a, line_a, kColors.counting_line, line_thickness, cv::LINE_AA);
                cv::line(overlay, line_b, extension_b, kColors.counting_line, line_thickness, cv::LINE_AA);
                cv::addWeighted(overlay, 0.35, canvas, 0.65, 0.0, canvas);
            }

            // 5: true parallel hysteresis boundaries for arbitrary line angle.
            if (visual.draw_hysteresis_band && config_.counting.hysteresis_px > 0.0) {
                for (const double sign : { -1.0, 1.0 }) {
                    const double offset = sign * config_.counting.hysteresis_px;
                    const cv::Point offset_a(
                        static_cast<int>(std::lround(line_a.x + normal_x * offset)),
                        static_cast<int>(std::lround(line_a.y + normal_y * offset)));
                    const cv::Point offset_b(
                        static_cast<int>(std::lround(line_b.x + normal_x * offset)),
                        static_cast<int>(std::lround(line_b.y + normal_y * offset)));
                    cv::line(canvas, offset_a, offset_b, kColors.hysteresis, 1, cv::LINE_AA);
                }
            }

            // 6: A->B counting line, endpoints, direction mapping, and side labels.
            if (visual.draw_counting_line) {
                cv::arrowedLine(canvas, line_a, line_b, kColors.counting_line,
                    line_thickness + 1, cv::LINE_AA, 0, 0.06);
                const cv::Point midpoint((line_a.x + line_b.x) / 2, (line_a.y + line_b.y) / 2);
                if (visual.draw_line_endpoints) {
                    cv::circle(canvas, line_a, std::max(3, line_thickness + 2),
                        kColors.counting_line, cv::FILLED, cv::LINE_AA);
                    cv::circle(canvas, line_b, std::max(3, line_thickness + 2),
                        kColors.counting_line, cv::FILLED, cv::LINE_AA);
                    drawLabel(canvas, "A", line_a + cv::Point(5, -5),
                        kColors.counting_line, font_scale, 1);
                    drawLabel(canvas, "B", line_b + cv::Point(5, -5),
                        kColors.counting_line, font_scale, 1);
                }
                std::ostringstream mapping;
                mapping << "A->B  + -> - = " << config_.counting.transition_positive_to_negative;
                drawLabel(canvas, mapping.str(), midpoint + cv::Point(6, -6),
                    kColors.counting_line, font_scale, 1);
                std::ostringstream coordinates;
                coordinates << std::fixed << std::setprecision(3)
                    << "A(" << config_.counting.line_a_norm.x << ',' << config_.counting.line_a_norm.y
                    << ") B(" << config_.counting.line_b_norm.x << ',' << config_.counting.line_b_norm.y << ')';
                drawLabel(canvas, coordinates.str(), midpoint + cv::Point(6, 13),
                    kColors.counting_line, font_scale * 0.85, 1);
                if (visual.draw_side_labels) {
                    const double side_offset = std::max(config_.counting.hysteresis_px + 18.0 * ui_scale, 16.0);
                    const cv::Point positive(
                        static_cast<int>(std::lround(midpoint.x + normal_x * side_offset)),
                        static_cast<int>(std::lround(midpoint.y + normal_y * side_offset)));
                    const cv::Point negative(
                        static_cast<int>(std::lround(midpoint.x - normal_x * side_offset)),
                        static_cast<int>(std::lround(midpoint.y - normal_y * side_offset)));
                    drawLabel(canvas, "SIDE +1", positive, kColors.side_positive, font_scale, 1);
                    drawLabel(canvas, "SIDE -1", negative, kColors.side_negative, font_scale, 1);
                    if (!compact) {
                        drawLabel(canvas, "DEAD BAND 0", midpoint + cv::Point(6, 18),
                            kColors.hysteresis, font_scale * 0.9, 1);
                    }
                }
            }
        }

        // 7: raw person candidates first, then their filter decisions.
        if (debug) {
            for (const DetectionDebugItem& item : debug->detection_items) {
                const PfRect& preferred = item.clipped_bbox.area() > 0.0 ? item.clipped_bbox : item.raw_bbox;
                const cv::Rect box = clippedRect(preferred, canvas.size());
                if (box.area() <= 0) continue;
                if (visual.draw_raw_person_detections) {
                    cv::rectangle(canvas, box, kColors.raw_person, 1, cv::LINE_AA);
                    std::ostringstream raw_label;
                    raw_label << "RAW P" << item.class_id << ' ' << std::fixed << std::setprecision(2)
                              << item.confidence;
                    if (!compact) {
                        raw_label << " [" << static_cast<int>(std::lround(item.raw_bbox.x1))
                                  << ',' << static_cast<int>(std::lround(item.raw_bbox.y1))
                                  << ',' << static_cast<int>(std::lround(item.raw_bbox.x2))
                                  << ',' << static_cast<int>(std::lround(item.raw_bbox.y2)) << ']';
                    }
                    drawLabel(canvas, raw_label.str(), cv::Point(box.x, std::max(12, box.y - 3)),
                        kColors.raw_person, font_scale * 0.85, 1);
                }

                if (item.decision == DetectionFilterDecision::AcceptedHigh &&
                    visual.draw_accepted_high_detections) {
                    cv::rectangle(canvas, box, kColors.accepted_high, box_thickness, cv::LINE_AA);
                    std::ostringstream label;
                    label << "DET-H " << std::fixed << std::setprecision(2) << item.confidence;
                    drawLabel(canvas, label.str(), cv::Point(box.x, box.y + 14),
                        kColors.accepted_high, font_scale, 1);
                }
                else if (item.decision == DetectionFilterDecision::AcceptedLow &&
                    visual.draw_accepted_low_detections) {
                    cv::rectangle(canvas, box, kColors.accepted_low, box_thickness, cv::LINE_AA);
                    std::ostringstream label;
                    label << "DET-L " << std::fixed << std::setprecision(2) << item.confidence;
                    drawLabel(canvas, label.str(), cv::Point(box.x, box.y + 14),
                        kColors.accepted_low, font_scale, 1);
                }
                else if (!item.accepted && visual.draw_rejected_detections) {
                    cv::rectangle(canvas, box, kColors.rejected, box_thickness, cv::LINE_AA);
                    if (visual.draw_rejection_reason) {
                        std::ostringstream label;
                        label << "REJ " << shortDecision(item.decision) << ' '
                              << std::fixed << std::setprecision(2) << item.confidence;
                        drawLabel(canvas, label.str(), cv::Point(box.x, box.y + 14),
                            kColors.rejected, font_scale, 1);
                    }
                }
            }
        }

        const std::vector<PersonTrack>& all_tracks = debug ? debug->all_tracks : confirmed_tracks;
        std::vector<const PersonTrack*> visible_tracks;
        visible_tracks.reserve(all_tracks.size());
        for (const PersonTrack& track : all_tracks) {
            if (trackVisible(track, visual)) visible_tracks.push_back(&track);
        }

        // 8: track boxes and lifecycle labels.
        for (const PersonTrack* track_ptr : visible_tracks) {
            const PersonTrack& track = *track_ptr;
            const cv::Scalar color = trackColor(track);
            const cv::Rect box = clippedRect(track.bbox, canvas.size());
            if (box.area() <= 0) continue;
            const int thickness = track.confirmed && track.missed == 0
                ? box_thickness
                : std::max(1, box_thickness - 1);
            cv::rectangle(canvas, box, color, thickness, cv::LINE_AA);
            if (visual.draw_track_id || visual.draw_track_stats) {
                std::ostringstream label;
                label << trackPrefix(track);
                if (visual.draw_track_id) label << " ID=" << track.track_id;
                if (visual.draw_track_stats) {
                    label << " h=" << track.hits << " m=" << track.missed
                          << " a=" << track.age << " c=" << std::fixed << std::setprecision(2)
                          << track.confidence;
                }
                drawLabel(canvas, label.str(), cv::Point(box.x, std::max(14, box.y - 4)),
                    color, font_scale, 1);
            }
        }

        // 9-10: trails, configured anchor point, velocity, side/stable/count state.
        for (const PersonTrack* track_ptr : visible_tracks) {
            const PersonTrack& track = *track_ptr;
            const cv::Scalar color = trackColor(track);
            if (visual.draw_trails) {
                for (std::size_t i = 1; i < track.trail.size(); ++i) {
                    cv::line(canvas, toCanvasPoint(track.trail[i - 1], canvas.size()),
                        toCanvasPoint(track.trail[i], canvas.size()), color,
                        track.missed > 0 ? std::max(1, trail_thickness - 1) : trail_thickness,
                        cv::LINE_AA);
                }
            }
            const cv::Point anchor = toCanvasPoint(track.anchor_point, canvas.size());
            if (visual.draw_anchor_points) {
                cv::circle(canvas, anchor, std::max(2, static_cast<int>(std::lround(4 * ui_scale))),
                    color, cv::FILLED, cv::LINE_AA);
            }
            const TrackCounterDebugState* counter_state = findCounterState(debug, track.track_id);
            if (visual.draw_counter_state && counter_state) {
                std::ostringstream state_label;
                state_label << "side=" << (counter_state->current_side > 0 ? "+1" :
                    counter_state->current_side < 0 ? "-1" : "0")
                    << " stable=" << (counter_state->stable_side > 0 ? "+1" :
                        counter_state->stable_side < 0 ? "-1" : "0")
                    << " counted=" << (counter_state->counted ? "Y" : "N")
                    << " hits=" << (counter_state->min_hits_ready ? "OK" : "WAIT");
                drawLabel(canvas, state_label.str(), anchor + cv::Point(5, 16),
                    color, font_scale * 0.9, 1);
            }
            if (visual.draw_velocity) {
                std::ostringstream velocity;
                velocity << std::fixed << std::setprecision(1)
                         << "v=(" << track.velocity_x << ',' << track.velocity_y << ')';
                drawLabel(canvas, velocity.str(), anchor + cv::Point(5, 31),
                    color, font_scale * 0.85, 1);
            }
        }

        // 11: finite-lifetime event markers. The debug context carries only
        // events from the current inference frame, so nothing is persisted to Redis.
        if (visual.draw_event_markers && debug) {
            for (const CrossingEvent& event : debug->new_events) {
                const auto duplicate = std::find_if(event_markers_.begin(), event_markers_.end(),
                    [&](const ActiveEventMarker& marker) { return marker.event.event_id == event.event_id; });
                if (duplicate == event_markers_.end()) {
                    event_markers_.push_back({ event, visual.event_marker_hold_frames });
                }
            }
        }
        if (visual.draw_event_markers) {
            for (ActiveEventMarker& marker : event_markers_) {
                const cv::Scalar color = marker.event.direction == "IN" ? kColors.event_in : kColors.event_out;
                const cv::Point point(
                    clampCoordinate(static_cast<int>(std::lround(marker.event.point_x_norm * canvas.cols)), canvas.cols),
                    clampCoordinate(static_cast<int>(std::lround(marker.event.point_y_norm * canvas.rows)), canvas.rows));
                cv::circle(canvas, point, std::max(5, static_cast<int>(std::lround(8 * ui_scale))),
                    color, cv::FILLED, cv::LINE_AA);
                std::ostringstream label;
                label << marker.event.direction << " T" << marker.event.track_id
                      << ' ' << marker.event.event_time_ms << "ms";
                drawLabel(canvas, label.str(), point + cv::Point(8, -8), color,
                    std::max(font_scale, 0.4), 2);
                --marker.remaining_frames;
            }
            event_markers_.erase(
                std::remove_if(event_markers_.begin(), event_markers_.end(),
                    [](const ActiveEventMarker& marker) { return marker.remaining_frames <= 0; }),
                event_markers_.end());
        }
        else {
            event_markers_.clear();
        }

        // 12: adaptive status panel. Compact mode always stays at two lines.
        if (visual.draw_status_panel) {
            const int margin = std::max(4, static_cast<int>(std::lround(10 * ui_scale)));
            const int panel_width = std::clamp(
                compact ? canvas.cols - margin * 2 : static_cast<int>(std::lround(590 * ui_scale)),
                1,
                std::max(1, canvas.cols - margin * 2));
            const int panel_height = std::clamp(
                compact ? static_cast<int>(std::lround(58 * ui_scale + 18))
                        : static_cast<int>(std::lround(112 * ui_scale)),
                1,
                std::max(1, canvas.rows - margin * 2));
            cv::Mat panel_overlay = canvas.clone();
            cv::rectangle(panel_overlay, cv::Rect(margin, margin, panel_width, panel_height),
                kColors.panel, cv::FILLED);
            cv::addWeighted(panel_overlay, 0.82, canvas, 0.18, 0.0, canvas);

            const int text_x = margin + std::max(5, static_cast<int>(std::lround(12 * ui_scale)));
            const int first_y = margin + std::max(14, static_cast<int>(std::lround(29 * ui_scale)));
            std::ostringstream first;
            first << "IN " << counts.in_count << " OUT " << counts.out_count
                  << " OCC " << counts.occupancy << " LIVE " << counts.live_persons;
            drawLabel(canvas, first.str(), cv::Point(text_x, first_y), kColors.text,
                compact ? std::max(0.34, font_scale) : std::max(0.45, font_scale * 1.2),
                compact ? 1 : 2, false);

            std::ostringstream second;
            second << std::fixed << std::setprecision(1)
                   << "CAP " << metrics.capture_fps << " INF " << metrics.infer_fps;
            if (debug) {
                second << " FRAME " << debug->frame_index << ' ';
                if (debug->warmup_active) second << "WARMUP " << debug->warmup_frames_remaining;
                else second << (debug->inference_frame ? "INF FRAME" : "REUSED FRAME");
            }
            else {
                second << " AGE " << metrics.latest_frame_age_ms << "ms";
            }
            const int second_y = std::min(canvas.rows - 3,
                first_y + std::max(14, static_cast<int>(std::lround(27 * ui_scale))));
            drawLabel(canvas, second.str(), cv::Point(text_x, second_y),
                kColors.secondary_text, std::max(0.3, font_scale), 1, false);

            if (!compact) {
                std::ostringstream third;
                third << "capture=" << metrics.capture_state
                      << " reconnect=" << metrics.reconnect_count
                      << " config=" << config_.config_version;
                const int third_y = std::min(canvas.rows - 3,
                    second_y + std::max(14, static_cast<int>(std::lround(25 * ui_scale))));
                drawLabel(canvas, third.str(), cv::Point(text_x, third_y),
                    cv::Scalar(200, 220, 255), std::max(0.28, font_scale * 0.9), 1, false);
            }
        }

        // Filter and track statistics use a separate small top-right panel.
        if (visual.draw_filter_statistics && debug) {
            int tentative_count = 0;
            int confirmed_count = 0;
            int predicted_count = 0;
            for (const PersonTrack& track : debug->all_tracks) {
                if (track.missed > 0 || !track.matched_this_frame) ++predicted_count;
                else if (track.confirmed) ++confirmed_count;
                else ++tentative_count;
            }
            const auto& stats = debug->filter_statistics;
            std::ostringstream first;
            first << "RAW-P " << stats.raw_person << " H " << stats.accepted_high
                  << " L " << stats.accepted_low;
            std::ostringstream second;
            second << "REJ conf " << stats.rejected_low_confidence
                   << " size " << stats.rejected_minimum_size
                   << " roi " << stats.rejected_roi;
            std::ostringstream third;
            third << "TRACK T " << tentative_count << " C " << confirmed_count
                  << " P " << predicted_count;
            const double stats_scale = std::max(0.28, font_scale * 0.86);
            int baseline = 0;
            const int text_width = std::max({
                cv::getTextSize(first.str(), cv::FONT_HERSHEY_SIMPLEX, stats_scale, 1, &baseline).width,
                cv::getTextSize(second.str(), cv::FONT_HERSHEY_SIMPLEX, stats_scale, 1, &baseline).width,
                cv::getTextSize(third.str(), cv::FONT_HERSHEY_SIMPLEX, stats_scale, 1, &baseline).width
            });
            const int margin = 5;
            const int panel_width = std::clamp(text_width + 12, 1, std::max(1, canvas.cols - margin * 2));
            const int row_height = std::max(13, static_cast<int>(std::lround(22 * ui_scale)));
            const int panel_height = std::clamp(row_height * 3 + 8, 1, std::max(1, canvas.rows - margin * 2));
            const int panel_x = std::max(margin, canvas.cols - panel_width - margin);
            const int panel_y = margin;
            cv::Mat overlay = canvas.clone();
            cv::rectangle(overlay, cv::Rect(panel_x, panel_y, panel_width, panel_height),
                kColors.panel, cv::FILLED);
            cv::addWeighted(overlay, 0.82, canvas, 0.18, 0.0, canvas);
            drawLabel(canvas, first.str(), cv::Point(panel_x + 5, panel_y + row_height),
                kColors.secondary_text, stats_scale, 1, false);
            drawLabel(canvas, second.str(), cv::Point(panel_x + 5, panel_y + row_height * 2),
                kColors.secondary_text, stats_scale, 1, false);
            drawLabel(canvas, third.str(), cv::Point(panel_x + 5, panel_y + row_height * 3),
                kColors.secondary_text, stats_scale, 1, false);
        }

        // 13: compact two-row legend at the bottom. It intentionally uses
        // short labels so 384x288 output remains readable.
        if (visual.draw_legend) {
            const std::vector<std::pair<std::string, cv::Scalar>> items{
                { "RAW", kColors.raw_person }, { "DET-H", kColors.accepted_high },
                { "DET-L", kColors.accepted_low }, { "REJ", kColors.rejected },
                { "TENT", kColors.tentative }, { "CONF", kColors.confirmed },
                { "PRED", kColors.missed }
            };
            const double legend_scale = std::max(0.26, font_scale * 0.78);
            const int row_height = std::max(13, static_cast<int>(std::lround(21 * ui_scale)));
            const int legend_height = std::min(canvas.rows, row_height * (compact ? 2 : 1) + 6);
            const int legend_y = std::max(0, canvas.rows - legend_height);
            cv::Mat overlay = canvas.clone();
            cv::rectangle(overlay, cv::Rect(0, legend_y, canvas.cols, legend_height),
                kColors.panel, cv::FILLED);
            cv::addWeighted(overlay, 0.78, canvas, 0.22, 0.0, canvas);
            int x = 7;
            int y = legend_y + row_height;
            for (const auto& item : items) {
                int baseline = 0;
                const cv::Size text_size = cv::getTextSize(
                    item.first, cv::FONT_HERSHEY_SIMPLEX, legend_scale, 1, &baseline);
                const int item_width = text_size.width + 22;
                if (x + item_width >= canvas.cols && y + row_height < canvas.rows) {
                    x = 7;
                    y += row_height;
                }
                if (x + item_width >= canvas.cols || y >= canvas.rows) break;
                cv::circle(canvas, cv::Point(x + 4, y - 4), 3, item.second, cv::FILLED, cv::LINE_AA);
                cv::putText(canvas, item.first, cv::Point(x + 10, y),
                    cv::FONT_HERSHEY_SIMPLEX, legend_scale, item.second, 1, cv::LINE_AA);
                x += item_width;
            }
        }

        return canvas;
    }

}  // namespace yolo11_server
