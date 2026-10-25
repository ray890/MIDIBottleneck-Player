// The Analysis window's workload graph without any drawing (v1 WorkloadGraph.cs): viewport,
// zoom, pan, follow, hover, pin and seek rules, the plot rectangle, the timeline ticks, the
// per-pixel series, the legend and the inspection tip. A frontend feeds pointer and size
// events in and draws the two frames this class builds:
//   * GraphStaticFrame - everything that changes only with the data, the view, the size or
//     the scale (v1's cached "static layer");
//   * GraphDynamicFrame - live markers, the Playback statistics panel, the pin, the hover
//     line and the inspection tip (v1 draws these on every paint).
//
// Text sizes come from a callback (the frontend's font): the plot rectangle and the tick
// labels depend on them exactly as in v1. Events (viewport changed, inspection changed,
// seek requested) are queued and fetched with take_events(). No timers here: the frontend
// calls process_dynamic_updates() when needs_dynamic_update() says so (v1's 16 ms timer).
#pragma once

#include "midibn/app/analysis_view.h"
#include "midibn/format/net_format.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace midibn {
namespace app {

struct GraphRect {
    int x = 0, y = 0, width = 0, height = 0;
    int right() const { return x + width; }
    int bottom() const { return y + height; }
    bool contains(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    bool intersects(const GraphRect& o) const { return o.x < x + width && x < o.x + o.width && o.y < y + height && y < o.y + o.height; }
    bool operator==(const GraphRect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

struct GraphSize {
    int width = 0, height = 0;
};

enum class FollowTarget { Off = 0, PlaybackTimeline = 1, MidiOutput = 2 };
enum class GraphCursor { Cross, SizeWE };

// The Playback statistics shown over the plot (v1 PlaybackOverlayData). `active` is "Playing or Paused".
struct PlaybackOverlay {
    bool active = false;
    std::string state, timeline_and_output, queue, events, events_caption, output_rate, effective_speed, lag;
    bool operator==(const PlaybackOverlay& o) const
    {
        return active == o.active && state == o.state && timeline_and_output == o.timeline_and_output && queue == o.queue &&
            events == o.events && events_caption == o.events_caption && output_rate == o.output_rate &&
            effective_speed == o.effective_speed && lag == o.lag;
    }
};

struct GraphEvent {
    enum Kind { ViewportChanged, InspectionChanged, SeekRequested } kind = ViewportChanged;
    int64_t time_us = 0;
    bool pinned = false;
};

// Text measurement in the graph's label font: `padded` is a normal text measurement, otherwise
// without the extra side padding (TextFormatFlags.NoPadding).
using GraphTextMeasure = std::function<GraphSize(const std::string& text, bool padded)>;
// Canonical (100%) pixels to device pixels. Empty: v1's ScaleMetric at the model's scale percent.
using GraphScale = std::function<int(int canonicalPixels)>;

struct GraphTextItem {
    GraphRect bounds;
    std::string text;
};

struct GraphAxisTick {
    int64_t time_us = 0;
    int x = 0;
    std::string label;
    GraphRect label_bounds;
    int mark_top = 0, mark_bottom = 0;
};

enum class PressureLevel { Low = 0, Medium = 1, High = 2, OverLimit = 3 };
struct GraphPressureStrip {
    int x;
    PressureLevel level;
};

struct GraphStaticFrame {
    int client_width = 0, client_height = 0;
    bool empty = true;             // no analysis: only `message` is shown, centred
    std::string message;
    GraphRect area;
    GraphTextItem scope;           // "WHOLE FILE   mm:ss – mm:ss"
    GraphTextItem title;           // panel title (left of the heading line)
    GraphTextItem scale;           // "peak … maximum rate …" (right-aligned)
    bool bytes_model = false;      // amber (bytes) or blue (events) series
    bool event_counts = false;
    std::vector<int> grid_y;       // the three inner grid lines
    std::vector<int> overflow_x;   // full-height predicted-overflow columns (absolute x)
    std::vector<int> source_y;     // one y per plot pixel column (absolute), polyline from area.x
    std::vector<int> gate_y;       // projected gate output series (empty without a gate projection)
    int capacity_y = -1;           // dashed maximum-rate line, -1 = none
    std::vector<GraphAxisTick> ticks;
    int cluster_y = 0, cluster_radius = 0;
    std::vector<int> cluster_x;    // magenta diamonds (absolute x)
    int pressure_y = 0, pressure_height = 0;
    std::vector<GraphPressureStrip> pressure;
    std::vector<GraphTextItem> legend;
};

struct GraphLiveMarker {
    int x;
    bool midi_output;              // green dash-dot, else white dotted playback timeline
};

struct GraphDynamicFrame {
    std::vector<GraphLiveMarker> markers;
    bool overlay_visible = false;
    GraphRect overlay_bounds;
    std::vector<GraphTextItem> overlay_lines;   // already fitted to the panel width
    bool pin_visible = false;
    int pin_x = 0;
    bool hover_visible = false;
    int hover_x = 0;
    bool tip_visible = false;
    GraphRect tip_bounds;
    std::string tip_text;
};

class WorkloadGraphModel {
public:
    WorkloadGraphModel();

    // ---- environment
    void set_culture(const format::NumberCulture& culture) { culture_ = culture; invalidate_static(); }
    void set_text_measure(GraphTextMeasure measure);
    // v1 ApplicationScalePercent (50..200): the default scale function.
    void set_scale_percent(int percent);
    int scale_percent() const { return scale_percent_; }
    // A frontend scale function replaces v1's percent formula (it also folds in the system DPI).
    void set_scale(GraphScale scale);
    // v1 OnResize: raises ViewportChanged when the plot width changed.
    void set_client_size(int width, int height);
    int client_width() const { return client_width_; }
    int client_height() const { return client_height_; }

    // ---- data
    // v1 Analysis setter: keeps the view when the duration is unchanged, otherwise resets it.
    void set_analysis(const AnalysisResultView& view);
    // v1 DetachAnalysis(message).
    void detach(const std::string& emptyMessage);
    void set_empty_message(const std::string& message);   // v1 EmptyMessage setter
    const std::string& empty_message() const { return empty_message_; }
    bool has_analysis() const { return has_analysis_; }
    const AnalysisResultView& analysis() const { return analysis_; }
    void set_show_event_counts(bool value);
    bool show_event_counts() const { return show_event_counts_; }
    void set_playback_statistics_visible(bool value);
    bool playback_statistics_visible() const { return playback_statistics_visible_; }
    void set_follow(FollowTarget target);
    FollowTarget follow() const { return follow_; }
    // v1 SetPlaybackPositions / SetPlaybackOverlay.
    void set_playback_positions(int64_t playbackTimeline, int64_t midiOutput);
    void set_playback_overlay(const PlaybackOverlay& overlay);
    bool has_playback_positions() const { return has_positions_; }
    int64_t playback_timeline() const { return playback_timeline_; }
    int64_t midi_output_position() const { return midi_output_; }

    // ---- view
    int64_t view_start() const { return view_start_; }
    int64_t view_end() const { return view_end_; }
    bool has_pinned() const { return has_pinned_; }
    int64_t pinned_time() const { return pinned_; }
    bool has_hover() const { return has_hover_; }
    int64_t hover_time() const { return hover_; }
    bool has_inspected() const { return has_hover_ || has_pinned_; }
    int64_t inspected_time() const { return has_hover_ ? hover_ : pinned_; }
    bool uses_byte_rate() const { return has_analysis_ && analysis_.uses_byte_rate(); }
    void reset_zoom();
    void zoom_at(int x, int wheelDelta);
    void pan_by_pixels(int pixels);
    // v1 GraphArea (the plot rectangle in client coordinates).
    GraphRect graph_area() const;
    int64_t time_at_client_x(int x) const;
    int client_x_at_time(int64_t time) const;
    bool try_get_pin_time(int x, int y, int64_t& time) const;
    // v1 BucketRangeAtPixel: the bucket indices [first, last) behind plot pixel column `x`.
    void bucket_range_at_pixel(int x, const GraphRect& area, int& first, int& last) const;
    std::vector<GraphAxisTick> timeline_ticks() const;
    std::string event_panel_title() const;
    double event_panel_peak() const;

    // ---- pointer input (client coordinates)
    void mouse_move(int x, int y);
    void mouse_leave();
    void mouse_down(int x, int y);       // left button
    void mouse_up(int x, int y);         // left button
    void mouse_double_click(int x, int y);
    void mouse_wheel(int x, int y, int delta);
    bool mouse_captured() const { return mouse_down_; }
    GraphCursor cursor() const { return cursor_; }

    // ---- repaint and timers
    // True while a hover update or a repaint waits for process_dynamic_updates (v1's 16 ms timer runs).
    bool needs_dynamic_update() const { return hover_update_pending_ || dynamic_repaint_pending_; }
    // v1 ProcessDynamicUpdates. `nowMicroseconds` is a monotonic clock. Returns true when the graph must repaint.
    bool process_dynamic_updates(int64_t nowMicroseconds);
    // True (once) when anything visible changed since the last call: repaint the window.
    bool take_repaint_request();
    uint64_t static_revision() const { return static_revision_; }
    int static_build_count() const { return static_build_count_; }

    std::vector<GraphEvent> take_events();

    // ---- frames and text
    const GraphStaticFrame& static_frame();
    GraphDynamicFrame dynamic_frame() const;
    // The graph's hover text for the pointer position (the Playback statistics values, or the usage hint).
    std::string tooltip_text(int x, int y) const;
    bool playback_statistics_drawn() const;
    GraphRect playback_statistics_bounds(const GraphRect& area) const;
    // v1 FitPlaybackLine: the caption + value that fits `width`, shortening the value, then the caption.
    std::string fit_playback_line(const std::string& caption, const std::string& shortCaption, const std::string& value, int width) const;

private:
    int S(int canonical) const;
    GraphSize measure(const std::string& text, bool padded) const;
    void invalidate_static();
    void request_dynamic_repaint();
    void raise_viewport_changed();
    void raise_inspection(int64_t time, bool pinned);
    void set_viewport(int64_t start, int64_t end);
    void apply_follow();
    double peak_event_count() const;
    double panel_peak(bool byteModel, bool eventCounts) const;
    std::vector<GraphAxisTick> build_ticks(const GraphRect& area) const;
    void build_series(GraphStaticFrame& frame, const GraphRect& area, bool byteModel, bool eventCounts, double peak) const;
    void build_lanes(GraphStaticFrame& frame, const GraphRect& area) const;
    void build_legend(GraphStaticFrame& frame, const GraphRect& area) const;

    format::NumberCulture culture_;
    GraphTextMeasure measure_;
    GraphScale scale_;
    int scale_percent_ = 100;
    int client_width_ = 0, client_height_ = 0;

    AnalysisResultView analysis_;
    bool has_analysis_ = false;
    std::string empty_message_ = "No analysis available.";
    bool show_event_counts_ = false;
    bool playback_statistics_visible_ = false;
    FollowTarget follow_ = FollowTarget::Off;

    int64_t view_start_ = 0, view_end_ = 0;
    bool has_hover_ = false, has_pinned_ = false;
    int64_t hover_ = 0, pinned_ = 0;
    bool has_positions_ = false;
    int64_t playback_timeline_ = 0, midi_output_ = 0;
    PlaybackOverlay overlay_;
    bool has_overlay_ = false;

    bool mouse_down_ = false, mouse_down_at_edge_ = false, dragging_ = false;
    int drag_origin_x_ = 0, drag_origin_y_ = 0;
    int64_t drag_view_start_ = 0;
    GraphCursor cursor_ = GraphCursor::Cross;
    int pending_pointer_x_ = 0, pending_pointer_y_ = 0;
    bool hover_update_pending_ = false;
    bool dynamic_repaint_pending_ = false;
    int64_t last_inspection_notification_us_ = -1;
    int last_resolution_plot_width_ = 0;
    bool repaint_requested_ = false;

    std::vector<GraphEvent> events_;

    mutable bool area_valid_ = false;
    mutable GraphRect area_cache_;
    mutable bool peak_valid_ = false;
    mutable double peak_event_count_ = 0;
    GraphStaticFrame static_frame_;
    bool static_valid_ = false;
    uint64_t static_revision_ = 1;
    int static_build_count_ = 0;
};

} // namespace app
} // namespace midibn
