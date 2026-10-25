#include "midibn/app/analysis_graph.h"

#include "midibn/app/analysis_format.h"
#include "midibn/app/analysis_summary.h"
#include "midibn/format/display_format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace midibn {
namespace app {

using format::format_n;
using format::format_n0;
using format::round_half_even;
using format::round_half_even_double;

namespace {

const char* const kGe = "\xE2\x89\xA5";        // >=
const char* const kDiamond = "\xE2\x97\x86";   // black diamond

const char* const kDefaultTooltip = "Wheel: zoom around cursor. Drag: pan. Click: pin. Double-click: seek.";

int v1_scale_metric(int value, int percent)
{
    if (value == 0) return 0;
    int scaled = (value * percent + 50) / 100;
    return value > 0 ? std::max(1, scaled) : std::min(-1, scaled);
}

int clamp_int(int64_t value, int64_t low, int64_t high) { return static_cast<int>(std::max(low, std::min(high, value))); }

} // namespace

WorkloadGraphModel::WorkloadGraphModel()
{
    measure_ = [](const std::string& text, bool) {
        GraphSize size;
        size.width = static_cast<int>(utf16_length(text)) * 6;
        size.height = 13;
        return size;
    };
}

void WorkloadGraphModel::set_text_measure(GraphTextMeasure measure)
{
    measure_ = std::move(measure);
    invalidate_static();
}

void WorkloadGraphModel::set_scale_percent(int percent)
{
    percent = std::max(50, std::min(200, percent));
    if (scale_percent_ == percent && !scale_) return;
    scale_percent_ = percent;
    invalidate_static();
}

void WorkloadGraphModel::set_scale(GraphScale scale)
{
    scale_ = std::move(scale);
    invalidate_static();
}

int WorkloadGraphModel::S(int canonical) const { return scale_ ? scale_(canonical) : v1_scale_metric(canonical, scale_percent_); }

GraphSize WorkloadGraphModel::measure(const std::string& text, bool padded) const { return measure_(text, padded); }

void WorkloadGraphModel::invalidate_static()
{
    static_valid_ = false;
    area_valid_ = false;
    ++static_revision_;
    repaint_requested_ = true;
}

void WorkloadGraphModel::request_dynamic_repaint() { dynamic_repaint_pending_ = true; }

void WorkloadGraphModel::raise_viewport_changed()
{
    GraphEvent e;
    e.kind = GraphEvent::ViewportChanged;
    events_.push_back(e);
}

void WorkloadGraphModel::raise_inspection(int64_t time, bool pinned)
{
    GraphEvent e;
    e.kind = GraphEvent::InspectionChanged;
    e.time_us = time;
    e.pinned = pinned;
    events_.push_back(e);
}

std::vector<GraphEvent> WorkloadGraphModel::take_events()
{
    std::vector<GraphEvent> out;
    out.swap(events_);
    return out;
}

bool WorkloadGraphModel::take_repaint_request()
{
    bool value = repaint_requested_;
    repaint_requested_ = false;
    return value;
}

void WorkloadGraphModel::set_client_size(int width, int height)
{
    width = std::max(0, width);
    height = std::max(0, height);
    if (width == client_width_ && height == client_height_) return;
    client_width_ = width;
    client_height_ = height;
    invalidate_static();
    // Auto resolution depends on horizontal plot density, not height: a layout-only height change
    // (status controls hiding) must not restart a calculation (v1 OnResize).
    int plotWidth = graph_area().width;
    if (plotWidth != last_resolution_plot_width_) {
        last_resolution_plot_width_ = plotWidth;
        raise_viewport_changed();
    }
}

// ---------------------------------------------------------------- data

void WorkloadGraphModel::set_analysis(const AnalysisResultView& view)
{
    bool first = !has_analysis_;
    int64_t oldDuration = has_analysis_ ? analysis_.duration_us() : 0;
    analysis_ = view;
    has_analysis_ = view.valid();
    peak_valid_ = false;
    if (has_analysis_) {
        int64_t duration = analysis_.duration_us();
        if (first || oldDuration <= 0 || view_end_ <= view_start_) {
            reset_zoom();
        } else {
            view_start_ = std::max<int64_t>(0, std::min(duration, view_start_));
            view_end_ = std::max(view_start_ + 1, std::min(duration, view_end_));
        }
        if (has_pinned_) pinned_ = std::min(duration, pinned_);
    }
    invalidate_static();
}

void WorkloadGraphModel::detach(const std::string& emptyMessage)
{
    analysis_ = AnalysisResultView();
    has_analysis_ = false;
    peak_valid_ = false;
    has_hover_ = false;
    has_pinned_ = false;
    has_positions_ = false;
    has_overlay_ = false;
    overlay_ = PlaybackOverlay();
    view_start_ = 0;
    view_end_ = 0;
    mouse_down_ = false;
    dragging_ = false;
    hover_update_pending_ = false;
    dynamic_repaint_pending_ = false;
    cursor_ = GraphCursor::Cross;
    empty_message_ = emptyMessage.empty() ? kAnalysisNoFileMessage : emptyMessage;
    invalidate_static();
}

void WorkloadGraphModel::set_empty_message(const std::string& message)
{
    empty_message_ = message;
    invalidate_static();
}

void WorkloadGraphModel::set_show_event_counts(bool value)
{
    if (show_event_counts_ == value) return;
    show_event_counts_ = value;
    invalidate_static();
}

void WorkloadGraphModel::set_playback_statistics_visible(bool value)
{
    if (playback_statistics_visible_ == value) return;
    playback_statistics_visible_ = value;
    repaint_requested_ = true;
}

void WorkloadGraphModel::set_follow(FollowTarget target)
{
    follow_ = target;
    apply_follow();
}

void WorkloadGraphModel::set_playback_positions(int64_t playbackTimeline, int64_t midiOutput)
{
    playbackTimeline = std::max<int64_t>(0, playbackTimeline);
    midiOutput = std::max<int64_t>(0, midiOutput);
    if (has_positions_ && playback_timeline_ == playbackTimeline && midi_output_ == midiOutput) return;
    has_positions_ = true;
    playback_timeline_ = playbackTimeline;
    midi_output_ = midiOutput;
    apply_follow();
    request_dynamic_repaint();
    repaint_requested_ = true;
}

void WorkloadGraphModel::set_playback_overlay(const PlaybackOverlay& overlay)
{
    if (has_overlay_ && overlay_ == overlay) return;
    has_overlay_ = true;
    overlay_ = overlay;
    if (playback_statistics_visible_) {
        request_dynamic_repaint();
        repaint_requested_ = true;
    }
}

bool WorkloadGraphModel::playback_statistics_drawn() const
{
    return playback_statistics_visible_ && has_overlay_ && overlay_.active;
}

// ---------------------------------------------------------------- geometry

GraphRect WorkloadGraphModel::graph_area() const
{
    if (area_valid_) return area_cache_;
    int64_t span = std::max<int64_t>(1, view_end_ - view_start_);
    std::string endLabel = analysis_format_axis_clock(std::max(view_start_, view_end_),
        choose_timeline_tick_interval(span, std::max(S(100), client_width_)));
    int horizontalAllowance = std::max(S(42), measure(endLabel, true).width / 2 + S(8));
    int top = S(42);
    bool finite = has_analysis_ && analysis_.has_queue_projection() && analysis_.queue_limit_enabled();
    bool extraLegend = has_analysis_ && !analysis_.has_queue_projection();
    bool narrow = client_width_ - horizontalAllowance * 2 < S(430);
    int bottomAllowance = S(finite ? (narrow ? 121 : 104) : (narrow ? 104 : 87));
    if (extraLegend) bottomAllowance += S(17);
    GraphRect r;
    r.x = horizontalAllowance;
    r.y = top;
    r.width = std::max(S(20), client_width_ - horizontalAllowance * 2);
    r.height = std::max(S(50), client_height_ - top - bottomAllowance);
    area_cache_ = r;
    area_valid_ = true;
    return r;
}

int64_t WorkloadGraphModel::time_at_client_x(int x) const
{
    if (!has_analysis_ || analysis_.duration_us() <= 0) return 0;
    GraphRect area = graph_area();
    double normalized = (x - area.x) / static_cast<double>(std::max(1, area.width));
    normalized = std::max(0.0, std::min(1.0, normalized));
    return view_start_ + round_half_even(normalized * static_cast<double>(std::max<int64_t>(1, view_end_ - view_start_)));
}

int WorkloadGraphModel::client_x_at_time(int64_t time) const
{
    GraphRect area = graph_area();
    return area.x + static_cast<int>(round_half_even(area.width * static_cast<double>(time - view_start_) /
        static_cast<double>(std::max<int64_t>(1, view_end_ - view_start_))));
}

bool WorkloadGraphModel::try_get_pin_time(int x, int y, int64_t& time) const
{
    time = 0;
    if (!has_analysis_ || analysis_.duration_us() <= 0) return false;
    GraphRect area = graph_area();
    const int edgeTolerance = 6;
    if (y < area.y || y >= area.bottom() || x < area.x - edgeTolerance || x > area.right() + edgeTolerance) return false;
    time = time_at_client_x(std::max(area.x, std::min(area.right(), x)));
    return true;
}

void WorkloadGraphModel::bucket_range_at_pixel(int x, const GraphRect& area, int& first, int& last) const
{
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    int64_t span = view_end_ - view_start_;
    int64_t start = view_start_ + static_cast<int64_t>((x / static_cast<double>(std::max(1, area.width))) * static_cast<double>(span));
    int64_t end = view_start_ + static_cast<int64_t>(((x + 1) / static_cast<double>(std::max(1, area.width))) * static_cast<double>(span));
    int64_t bucket = std::max<int64_t>(1, a.bucket_us);
    int64_t count = static_cast<int64_t>(a.buckets.size());
    first = clamp_int(start / bucket, 0, count - 1);
    last = clamp_int(end / bucket + 1, static_cast<int64_t>(first) + 1, count);
}

// ---------------------------------------------------------------- view

void WorkloadGraphModel::reset_zoom()
{
    view_start_ = 0;
    view_end_ = has_analysis_ ? std::max<int64_t>(1, analysis_.duration_us()) : 1;
    invalidate_static();
    raise_viewport_changed();
}

void WorkloadGraphModel::set_viewport(int64_t start, int64_t end)
{
    if (!has_analysis_) return;
    int64_t duration = std::max<int64_t>(1, analysis_.duration_us());
    int64_t span = std::max<int64_t>(1, std::min(duration, end - start));
    if (start < 0) start = 0;
    if (start + span > duration) start = duration - span;
    view_start_ = std::max<int64_t>(0, start);
    view_end_ = view_start_ + span;
    invalidate_static();
    raise_viewport_changed();
}

void WorkloadGraphModel::zoom_at(int x, int wheelDelta)
{
    if (!has_analysis_ || analysis_.duration_us() <= 0 || wheelDelta == 0) return;
    int64_t duration = analysis_.duration_us();
    int64_t oldSpan = std::max<int64_t>(1, view_end_ - view_start_);
    double factor = std::pow(0.8, wheelDelta / 120.0);
    int64_t minimum = std::max<int64_t>(1, duration / 100000);
    int64_t newSpan = std::max(minimum, std::min(duration, round_half_even(static_cast<double>(oldSpan) * factor)));
    int64_t anchor = time_at_client_x(x);
    GraphRect area = graph_area();
    double ratio = std::max(0.0, std::min(1.0, (x - area.x) / static_cast<double>(std::max(1, area.width))));
    int64_t start = anchor - round_half_even(static_cast<double>(newSpan) * ratio);
    set_viewport(start, start + newSpan);
}

void WorkloadGraphModel::pan_by_pixels(int pixels)
{
    int64_t span = std::max<int64_t>(1, view_end_ - view_start_);
    int64_t delta = round_half_even(static_cast<double>(static_cast<int64_t>(-pixels) * span) /
        static_cast<double>(std::max(1, graph_area().width)));
    set_viewport(view_start_ + delta, view_end_ + delta);
}

void WorkloadGraphModel::apply_follow()
{
    if (!has_analysis_ || follow_ == FollowTarget::Off || !has_positions_) return;
    int64_t marker = follow_ == FollowTarget::PlaybackTimeline ? playback_timeline_ : midi_output_;
    if (view_start_ == 0 && view_end_ >= analysis_.duration_us()) return;
    int64_t span = std::max<int64_t>(1, view_end_ - view_start_);
    int64_t left = view_start_ + span * 15 / 100;
    int64_t right = view_start_ + span * 85 / 100;
    if (marker >= left && marker <= right) return;
    int64_t desired = marker < left ? marker - span * 30 / 100 : marker - span * 70 / 100;
    set_viewport(desired, desired + span);
}

// ---------------------------------------------------------------- ticks and text

std::vector<GraphAxisTick> WorkloadGraphModel::build_ticks(const GraphRect& area) const
{
    std::vector<GraphAxisTick> result;
    if (view_end_ <= view_start_ || area.width <= 0) return result;
    int64_t span = view_end_ - view_start_;
    int64_t interval = choose_timeline_tick_interval(span, area.width);
    int64_t remainder = view_start_ % interval;
    int64_t first = remainder == 0 ? view_start_ : view_start_ + (interval - remainder);
    int safetyGap = S(10);
    int previousRight = (std::numeric_limits<int>::min)();
    for (int64_t time = first; time <= view_end_;) {
        std::string label = analysis_format_axis_clock(time, interval);
        GraphSize measured = measure(label, false);
        int x = area.x + static_cast<int>(round_half_even(area.width * static_cast<double>(time - view_start_) / static_cast<double>(span)));
        GraphRect bounds;
        bounds.x = x - measured.width / 2;
        bounds.y = area.bottom() + S(5);
        bounds.width = measured.width;
        bounds.height = std::max(S(16), measured.height);
        if (bounds.x >= S(2) && bounds.right() <= client_width_ - S(2) && bounds.x >= previousRight + safetyGap) {
            GraphAxisTick tick;
            tick.time_us = time;
            tick.x = x;
            tick.label = label;
            tick.label_bounds = bounds;
            tick.mark_top = area.bottom() + S(1);
            tick.mark_bottom = area.bottom() + S(4);
            result.push_back(tick);
            previousRight = bounds.right();
        }
        if (time > (std::numeric_limits<int64_t>::max)() - interval) break;
        time += interval;
    }
    return result;
}

std::vector<GraphAxisTick> WorkloadGraphModel::timeline_ticks() const { return build_ticks(graph_area()); }

std::string WorkloadGraphModel::event_panel_title() const
{
    return show_event_counts_ && has_analysis_ ? "Events per interval " + std::string(format::kEmDash) + " " +
        analysis_format_resolution(analysis_.bucket_us(), culture_) : std::string("Events/sec");
}

double WorkloadGraphModel::peak_event_count() const
{
    if (!has_analysis_) return 0;
    if (peak_valid_) return peak_event_count_;
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    int64_t peak = 0;
    bool gate = analysis_.has_gate_projection();
    for (size_t i = 0; i < a.buckets.size(); ++i) {
        peak = std::max<int64_t>(peak, a.buckets[i].event_count);
        if (gate && i < a.gate_output_buckets.size()) peak = std::max(peak, a.gate_output_buckets[i]);
    }
    peak_event_count_ = static_cast<double>(peak);
    peak_valid_ = true;
    return peak_event_count_;
}

double WorkloadGraphModel::panel_peak(bool byteModel, bool eventCounts) const
{
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    if (byteModel) return a.peak_bytes_per_second;
    if (eventCounts) return peak_event_count();
    return std::max(a.peak_events_per_second, a.gate_peak_events_per_second);
}

double WorkloadGraphModel::event_panel_peak() const
{
    if (!has_analysis_) return 0;
    bool bytes = uses_byte_rate();
    return show_event_counts_ && !bytes ? peak_event_count() : bytes ? analysis_.workload().peak_bytes_per_second
        : std::max(analysis_.workload().peak_events_per_second, analysis_.workload().gate_peak_events_per_second);
}

// ---------------------------------------------------------------- frames

void WorkloadGraphModel::build_series(GraphStaticFrame& frame, const GraphRect& area, bool byteModel, bool eventCounts,
    double peak) const
{
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    double bucketSeconds = static_cast<double>(a.bucket_us) / 1000000.0;
    bool gate = analysis_.has_gate_projection();
    bool queue = analysis_.has_queue_projection();
    bool overflowWanted = queue && analysis_.queue_limit_enabled();
    frame.source_y.assign(static_cast<size_t>(area.width), area.bottom());
    if (gate) frame.gate_y.assign(static_cast<size_t>(area.width), area.bottom());
    for (int x = 0; x < area.width; ++x) {
        int first, last;
        bucket_range_at_pixel(x, area, first, last);
        double value = 0, gateValue = 0;
        bool predictedOverflow = false;
        for (int bucket = first; bucket < last; ++bucket) {
            const analysis::WorkloadBucket& b = a.buckets[static_cast<size_t>(bucket)];
            double current = byteModel ? static_cast<double>(b.byte_count) / bucketSeconds
                : eventCounts ? static_cast<double>(b.event_count) : static_cast<double>(b.event_count) / bucketSeconds;
            value = std::max(value, current);
            if (gate) {
                int64_t out = static_cast<size_t>(bucket) < a.gate_output_buckets.size() ? a.gate_output_buckets[static_cast<size_t>(bucket)] : 0;
                gateValue = std::max(gateValue, eventCounts ? static_cast<double>(out) : static_cast<double>(out) / bucketSeconds);
            }
            predictedOverflow = predictedOverflow || (queue && b.predicted_dropped_events > 0);
        }
        if (predictedOverflow && overflowWanted) frame.overflow_x.push_back(area.x + x);
        int y = peak <= 0 ? area.bottom() : area.bottom() - static_cast<int>(round_half_even((value / peak) * area.height));
        frame.source_y[static_cast<size_t>(x)] = std::max(area.y, std::min(area.bottom(), y));
        if (gate) {
            int gateY = peak <= 0 ? area.bottom() : area.bottom() - static_cast<int>(round_half_even((gateValue / peak) * area.height));
            frame.gate_y[static_cast<size_t>(x)] = std::max(area.y, std::min(area.bottom(), gateY));
        }
    }
}

void WorkloadGraphModel::build_lanes(GraphStaticFrame& frame, const GraphRect& area) const
{
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    frame.cluster_y = area.bottom() + S(25);
    frame.pressure_y = area.bottom() + S(38);
    frame.cluster_radius = S(3);
    frame.pressure_height = S(6);
    bool pressureWanted = analysis_.has_queue_projection() && analysis_.queue_limit_enabled();
    const analysis::AnalysisConfiguration* cfg = analysis_.configuration.get();
    for (int x = 0; x < area.width; ++x) {
        int first, last;
        bucket_range_at_pixel(x, area, first, last);
        int clusterSize = 0, occupancy = 0;
        int64_t queueAge = 0;
        for (int bucket = first; bucket < last; ++bucket) {
            const analysis::WorkloadBucket& b = a.buckets[static_cast<size_t>(bucket)];
            clusterSize = std::max(clusterSize, b.largest_cluster);
            occupancy = std::max(occupancy, b.predicted_peak_occupancy);
            queueAge = std::max(queueAge, b.predicted_peak_queue_age_us);
        }
        if (clusterSize >= 50) frame.cluster_x.push_back(area.x + x);
        if (pressureWanted) {
            double ratio = cfg->queue_age_limit_enabled
                ? static_cast<double>(queueAge) / static_cast<double>(std::max<int64_t>(1, cfg->queue_age_limit_us))
                : static_cast<double>(occupancy) / static_cast<double>(std::max(1, cfg->queue_length_limit));
            GraphPressureStrip strip;
            strip.x = area.x + x;
            strip.level = ratio > 1.0 ? PressureLevel::OverLimit : ratio >= 0.9 ? PressureLevel::High
                : ratio >= 0.65 ? PressureLevel::Medium : PressureLevel::Low;
            frame.pressure.push_back(strip);
        }
    }
}

void WorkloadGraphModel::build_legend(GraphStaticFrame& frame, const GraphRect& area) const
{
    auto line = [&](int offset, const std::string& text) {
        GraphTextItem item;
        item.bounds.x = area.x;
        item.bounds.y = area.bottom() + S(offset);
        item.bounds.width = area.width;
        item.bounds.height = S(18);
        item.text = text;
        frame.legend.push_back(item);
    };
    bool finite = analysis_.has_queue_projection() && analysis_.queue_limit_enabled();
    bool narrow = area.width < S(430);
    std::string bullet = std::string("  ") + format::kBullet + "  ";
    std::string resolution = analysis_format_resolution(analysis_.bucket_us(), culture_);
    bool gateSelected = analysis_.configuration != nullptr && analysis_.uses_gate();
    std::string pending;
    if (!analysis_.has_gate_projection() && !analysis_.has_queue_projection()) {
        std::string kind = gateSelected ? "gate" : "queue";
        pending = analysis_.state == analysis::ProjectionState::Pending
            ? "Workload only " + std::string(format::kEmDash) + " " + kind + " projection pending"
            : analysis_.state == analysis::ProjectionState::Cancelled
                ? "Workload only " + std::string(format::kEmDash) + " projection cancelled"
                : "Workload only " + std::string(format::kEmDash) + " " + kind + " projection failed";
    }
    const std::string blueAmber = "Blue: source MIDI" + bullet + "Amber: projected gate output";
    if (narrow) {
        line(47, std::string(kDiamond) + " Magenta: burst " + kGe + "50 simultaneous events");
        int next = 64;
        if (finite) {
            line(next, "Red area: overflow" + bullet + "Purple strip: over limit");
            next += 17;
        }
        line(next, "White dots: playback timeline");
        line(next + 17, "Green dash-dot: MIDI output" + bullet + resolution);
        if (analysis_.has_gate_projection()) line(next + 34, blueAmber);
        else if (!analysis_.has_queue_projection()) line(next + 34, pending);
        return;
    }
    line(47, std::string("Magenta diamond: ") + kGe + "50 simultaneous events");
    if (finite) line(64, "Red: predicted overflow" + bullet + "Purple pressure: over limit");
    line(finite ? 81 : 64, "White dots: playback timeline" + bullet + "Green dash-dot: MIDI output" + bullet + resolution);
    if (analysis_.has_gate_projection()) line(finite ? 98 : 81, blueAmber);
    else if (!analysis_.has_queue_projection()) line(finite ? 98 : 81, pending);
}

const GraphStaticFrame& WorkloadGraphModel::static_frame()
{
    if (static_valid_) return static_frame_;
    GraphStaticFrame frame;
    frame.client_width = client_width_;
    frame.client_height = client_height_;
    frame.message = empty_message_;
    if (!has_analysis_ || analysis_.workload().buckets.empty()) {
        frame.empty = true;
        static_frame_ = std::move(frame);
        static_valid_ = true;
        ++static_build_count_;
        return static_frame_;
    }
    frame.empty = false;
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    GraphRect area = graph_area();
    frame.area = area;
    bool byteModel = uses_byte_rate();
    frame.bytes_model = byteModel;
    bool eventCounts = show_event_counts_ && !byteModel;
    frame.event_counts = eventCounts;
    frame.scope.text = std::string(view_start_ == 0 && view_end_ >= a.duration_us ? "WHOLE FILE   " : "VISIBLE RANGE   ") +
        analysis_format_clock_short(view_start_) + " " + format::kEnDash + " " + analysis_format_clock_short(view_end_);
    frame.scope.bounds.x = area.x;
    frame.scope.bounds.y = S(5);
    frame.scope.bounds.width = area.width;
    frame.scope.bounds.height = S(20);
    std::string title = byteModel ? "MIDI bytes/sec" : event_panel_title();
    std::string unit = byteModel ? "bytes/sec" : eventCounts ? "events/interval" : "events/sec";
    double peak = panel_peak(byteModel, eventCounts);
    double capacity = analysis_.has_queue_projection()
        ? (byteModel ? a.byte_service_capacity_per_second : a.event_service_capacity_per_second) : 0;
    if (eventCounts) capacity *= static_cast<double>(a.bucket_us) / 1000000.0;
    for (int i = 1; i < 4; ++i) frame.grid_y.push_back(area.y + area.height * i / 4);
    build_series(frame, area, byteModel, eventCounts, peak);
    if (capacity > 0 && peak > 0 && capacity <= peak)
        frame.capacity_y = area.bottom() - static_cast<int>(round_half_even((capacity / peak) * area.height));
    frame.title.text = title;
    frame.title.bounds.x = area.x;
    frame.title.bounds.y = area.y - S(22);
    frame.title.bounds.width = area.width / 3;
    frame.title.bounds.height = S(20);
    std::string scale;
    if (area.width < S(430)) {
        scale = "peak " + format_n(peak, 0, culture_);
        if (capacity > 0) scale += std::string("  ") + format::kBullet + "  max " + format_n(capacity, 0, culture_) + (eventCounts ? "/interval" : "/s");
    } else {
        scale = "peak " + format_n(peak, 1, culture_) + " " + unit;
        if (capacity > 0) scale += "   maximum rate " + format_n(capacity, 1, culture_) + " " + unit;
    }
    frame.scale.text = scale;
    frame.scale.bounds.x = area.x + area.width / 3;
    frame.scale.bounds.y = area.y - S(22);
    frame.scale.bounds.width = area.width * 2 / 3;
    frame.scale.bounds.height = S(20);
    frame.ticks = build_ticks(area);
    build_lanes(frame, area);
    build_legend(frame, area);
    static_frame_ = std::move(frame);
    static_valid_ = true;
    ++static_build_count_;
    return static_frame_;
}

GraphRect WorkloadGraphModel::playback_statistics_bounds(const GraphRect& area) const
{
    GraphRect r;
    r.width = std::max(S(80), std::min(S(310), area.width - S(16)));
    r.height = S(8 + 7 * 17);
    r.x = area.x + S(7);
    r.y = area.y + S(7);
    return r;
}

std::string WorkloadGraphModel::fit_playback_line(const std::string& caption, const std::string& shortCaption,
    const std::string& value, int width) const
{
    auto fits = [&](const std::string& text) { return measure(text, false).width <= std::max(1, width); };
    std::string full = caption + value;
    if (fits(full)) return full;
    std::string compact = value;
    auto replace = [](std::string& s, const std::string& from, const std::string& to) {
        size_t at = 0;
        while ((at = s.find(from, at)) != std::string::npos) { s.replace(at, from.size(), to); at += to.size(); }
    };
    replace(compact, " / ", "/");
    replace(compact, " events/sec", " ev/s");
    if (fits(caption + compact)) return caption + compact;
    std::string abbreviated = abbreviate_playback_counts(compact, culture_);
    if (fits(caption + abbreviated)) return caption + abbreviated;
    return shortCaption + abbreviated;
}

GraphDynamicFrame WorkloadGraphModel::dynamic_frame() const
{
    GraphDynamicFrame frame;
    if (!has_analysis_ || analysis_.workload().buckets.empty()) return frame;
    GraphRect area = graph_area();
    auto marker = [&](bool have, int64_t time, bool output) {
        if (!have || time < view_start_ || time > view_end_) return;
        GraphLiveMarker m;
        m.x = client_x_at_time(time);
        m.midi_output = output;
        frame.markers.push_back(m);
    };
    marker(has_positions_, playback_timeline_, false);
    marker(has_positions_, midi_output_, true);
    if (playback_statistics_drawn()) {
        frame.overlay_visible = true;
        GraphRect panel = playback_statistics_bounds(area);
        frame.overlay_bounds = panel;
        std::string captions[7] = { "Playback: ", "Timeline / output: ", "Queue now / maximum: ",
            overlay_.events_caption.empty() ? std::string("Events sent / dropped: ") : overlay_.events_caption,
            "Output rate: ", "Effective speed: ", "Lag current / maximum: " };
        const char* shorts[7] = { "State: ", "Time: ", "Queue: ", "Events: ", "Rate: ", "Speed: ", "Lag: " };
        std::string values[7] = { overlay_.state, overlay_.timeline_and_output, overlay_.queue, overlay_.events,
            overlay_.output_rate, overlay_.effective_speed, overlay_.lag };
        for (int i = 0; i < 7; ++i) {
            GraphTextItem item;
            item.text = fit_playback_line(captions[i], shorts[i], values[i], panel.width - S(14));
            item.bounds.x = panel.x + S(7);
            item.bounds.y = panel.y + S(4 + i * 17);
            item.bounds.width = panel.width - S(14);
            item.bounds.height = S(17);
            frame.overlay_lines.push_back(item);
        }
    }
    if (has_pinned_ && pinned_ >= view_start_ && pinned_ <= view_end_) {
        frame.pin_visible = true;
        frame.pin_x = client_x_at_time(pinned_);
    }
    if (!has_inspected() || inspected_time() < view_start_ || inspected_time() > view_end_) return frame;
    int64_t inspected = inspected_time();
    int x = client_x_at_time(inspected);
    if (has_hover_) {
        frame.hover_visible = true;
        frame.hover_x = x;
    }
    bool bytes = uses_byte_rate();
    const analysis::WorkloadAnalysis& a = analysis_.workload();
    const analysis::WorkloadBucket* bucket = analysis_bucket_at(analysis_, inspected);
    int32_t bucketIndex = analysis_bucket_index_at(analysis_, inspected);
    int64_t start = static_cast<int64_t>(bucketIndex) * a.bucket_us;
    int64_t end = std::min(a.duration_us, start + a.bucket_us);
    double seconds = static_cast<double>(a.bucket_us) / 1000000.0;
    double value = bucket == nullptr ? 0 : static_cast<double>(bytes ? bucket->byte_count : bucket->event_count) / seconds;
    if (show_event_counts_ && !bytes) {
        int count = bucket == nullptr ? 0 : bucket->event_count;
        frame.tip_text = format_n0(count, culture_) + " events   " + analysis_format_interval_boundary(start) + format::kEnDash +
            analysis_format_interval_boundary(end);
    } else {
        frame.tip_text = analysis_format_clock(inspected) + "   " + format_n(value, 1, culture_) + (bytes ? " bytes/sec" : " events/sec");
    }
    int tipWidth = S(show_event_counts_ && !bytes ? 350 : 210);
    int tipHeight = S(20);
    int tipX = std::max(area.x, std::min(area.right() - tipWidth - S(5), x + S(7)));
    int tipY = area.y + S(5);
    GraphRect overlay = playback_statistics_bounds(area);
    GraphRect tip;
    tip.x = tipX;
    tip.y = tipY;
    tip.width = tipWidth;
    tip.height = tipHeight;
    if (playback_statistics_drawn() && tip.intersects(overlay)) tip.y = std::min(area.bottom() - tipHeight, overlay.bottom() + S(5));
    frame.tip_visible = true;
    frame.tip_bounds = tip;
    return frame;
}

std::string WorkloadGraphModel::tooltip_text(int x, int y) const
{
    bool inside = playback_statistics_drawn() && playback_statistics_bounds(graph_area()).contains(x, y);
    if (!inside) return kDefaultTooltip;
    return "Playback: " + overlay_.state + "\nTimeline / output: " + overlay_.timeline_and_output +
        "\nQueue now / maximum: " + overlay_.queue + "\n" +
        (overlay_.events_caption.empty() ? std::string("Events sent / dropped: ") : overlay_.events_caption) + overlay_.events +
        "\nOutput rate: " + overlay_.output_rate + "\nEffective speed: " + overlay_.effective_speed +
        "\nLag current / maximum: " + overlay_.lag;
}

// ---------------------------------------------------------------- pointer

void WorkloadGraphModel::mouse_wheel(int x, int y, int delta)
{
    if (graph_area().contains(x, y)) zoom_at(x, delta);
}

void WorkloadGraphModel::mouse_move(int x, int y)
{
    if (mouse_down_ && !mouse_down_at_edge_) {
        int dx = x - drag_origin_x_;
        if (!dragging_ && std::abs(dx) >= 4) dragging_ = true;
        if (dragging_) {
            int64_t span = std::max<int64_t>(1, view_end_ - view_start_);
            int64_t shift = round_half_even(static_cast<double>(static_cast<int64_t>(-dx) * span) /
                static_cast<double>(std::max(1, graph_area().width)));
            set_viewport(drag_view_start_ + shift, drag_view_start_ + shift + span);
            cursor_ = GraphCursor::SizeWE;
            return;
        }
    }
    if (!has_analysis_ || !graph_area().contains(x, y)) return;
    pending_pointer_x_ = x;
    pending_pointer_y_ = y;
    hover_update_pending_ = true;
    request_dynamic_repaint();
}

void WorkloadGraphModel::mouse_leave()
{
    if (mouse_down_) return;
    hover_update_pending_ = false;
    has_hover_ = false;
    if (has_pinned_) raise_inspection(pinned_, true);
    request_dynamic_repaint();
}

void WorkloadGraphModel::mouse_down(int x, int y)
{
    int64_t ignored;
    if (!try_get_pin_time(x, y, ignored)) return;
    mouse_down_ = true;
    mouse_down_at_edge_ = !graph_area().contains(x, y);
    dragging_ = false;
    drag_origin_x_ = x;
    drag_origin_y_ = y;
    drag_view_start_ = view_start_;
}

void WorkloadGraphModel::mouse_up(int x, int y)
{
    if (!mouse_down_) return;
    bool wasDragging = dragging_;
    mouse_down_ = false;
    mouse_down_at_edge_ = false;
    dragging_ = false;
    cursor_ = GraphCursor::Cross;
    int64_t pinTime;
    if (!wasDragging && try_get_pin_time(x, y, pinTime)) {
        pinned_ = pinTime;
        has_pinned_ = true;
        raise_inspection(pinned_, true);
        repaint_requested_ = true;
    }
}

void WorkloadGraphModel::mouse_double_click(int x, int y)
{
    int64_t pinTime;
    if (!try_get_pin_time(x, y, pinTime)) return;
    pinned_ = pinTime;
    has_pinned_ = true;
    GraphEvent e;
    e.kind = GraphEvent::SeekRequested;
    e.time_us = pinned_;
    e.pinned = true;
    events_.push_back(e);
    repaint_requested_ = true;
}

bool WorkloadGraphModel::process_dynamic_updates(int64_t nowMicroseconds)
{
    if (hover_update_pending_ && has_analysis_) {
        int64_t time = time_at_client_x(pending_pointer_x_);
        has_hover_ = true;
        hover_ = time;
        if (last_inspection_notification_us_ < 0 || nowMicroseconds - last_inspection_notification_us_ >= 1000000 / 30) {
            hover_update_pending_ = false;
            last_inspection_notification_us_ = nowMicroseconds;
            raise_inspection(time, false);
        }
    }
    bool repaint = false;
    if (dynamic_repaint_pending_) {
        dynamic_repaint_pending_ = false;
        repaint = true;
    }
    return repaint;
}

} // namespace app
} // namespace midibn
