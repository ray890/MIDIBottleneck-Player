// Model-trace replay (Milestone 3). Formats match the v1 reference side in
// v2/harness/v1-reference/V1ModelHarness.cs; see TRACE_FORMAT.md.
#include "models/replay_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace midibn {
namespace models {

// Read access to private model state, for trace state lines only.
struct TraceAccess {
    static int64_t clock_remainder(const ServiceDurationClock& clock) { return clock.remainder_; }

    static std::string pacer(const ProgressiveRatePacer& p)
    {
        std::string tail = p.has_tail_ ? s(p.tail_.timestamp) + ":" + s(p.tail_.count) : "-";
        return "i" + b(p.initialized_) + " x" + b(p.exhausted_) + " w" + s(p.next_whole_) + " r" + s(p.next_remainder_) +
            " h" + s(p.history_count_) + " v" + s(p.reserved_count_) + " q" + s(static_cast<int64_t>(p.history_.size())) +
            " t" + tail;
    }

    static int32_t adaptive_count(const AdaptiveNoteIntervalClock& a) { return a.open_output_count_; }

    static std::string pending(const PendingMidiQueue& q)
    {
        std::string items;
        for (int32_t index : q.event_indices()) items += s(index) + ",";
        return "n" + s(q.count()) + " o" + s(q.oldest_event_index()) + " k" + s(q.peak_backing_count()) +
            " e" + s(q.eligible_count()) + " s" + s(q.allocated_segment_count()) + " f" + s(q.free_) +
            " x" + s(q.next_fresh_) + " h" + s(q.head_) + " t" + s(q.tail_) + " eh" + s(q.eligible_head_) +
            " et" + s(q.eligible_tail_) + " ho" + s(q.simple_head_offset_) + " to" + s(q.simple_tail_offset_) +
            " d" + digest(items);
    }

    static std::string fdq(const ForwardDropQueue& q)
    {
        std::string items;
        for (int64_t service : q.pending_service_) items += s(service) + ",";
        return "o" + s(q.occupancy()) + " m" + s(q.maximum_occupancy()) + " b" + b(q.busy_) +
            " c" + s(q.completion_microseconds_) + " p" + s(static_cast<int64_t>(q.pending_service_.size())) +
            " d" + digest(items);
    }

    static std::string tracker(const CompleteNoteTracker& t)
    {
        std::string text;
        int64_t total = 0;
        for (size_t slot = 0; slot < t.occurrences_.size(); ++slot) {
            const auto& queue = t.occurrences_[slot];
            if (queue.empty()) continue;
            text += s(static_cast<int64_t>(slot)) + ":";
            for (const auto& occurrence : queue) {
                text += s(occurrence.attack_index) + "/" + s(occurrence.state) + ",";
                total++;
            }
            text += ";";
        }
        text += "|q" + s(total);
        for (uint8_t flag : { CompleteNoteTracker::AttackFlags::kUnmatchedAccepted, CompleteNoteTracker::AttackFlags::kEvicted,
                 CompleteNoteTracker::AttackFlags::kFiltered }) {
            std::vector<int32_t> values = t.flags_.members(flag);
            text += "|";
            for (int32_t value : values) text += s(value) + ",";
        }
        return text;
    }

    static int64_t tracker_accepted(const CompleteNoteTracker& t)
    { return t.flags_.count(CompleteNoteTracker::AttackFlags::kUnmatchedAccepted); }

    static bool pending_indexed(const PendingMidiQueue& q) { return q.note_index_enabled_; }
};

// ------------------------------------------------------------ formatting

std::string s(int64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
    return buffer;
}

std::string hex2(unsigned value)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string text(2, '0');
    text[0] = digits[(value >> 4) & 15];
    text[1] = digits[value & 15];
    return text;
}

std::string digest(const std::string& text)
{
    uint64_t hash = 14695981039346656037ULL;
    for (char c : text) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

void append_number(std::string& text, int64_t value)
{
    char buffer[24];
    int length = 0;
    uint64_t magnitude = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        buffer[length++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) text += '-';
    while (length > 0) text += buffer[--length];
}

void append_event(std::string& text, const EventView& e)
{
    static const char digits[] = "0123456789ABCDEF";
    if (!e.is_valid()) { text += "invalid"; return; }
    append_number(text, e.event_index()); text += ':';
    append_number(text, e.tick); text += ':';
    append_number(text, e.micros); text += ':';
    append_number(text, e.track); text += ':';
    append_number(text, e.channel()); text += ':';
    uint8_t status = e.status();
    text += digits[status >> 4]; text += digits[status & 15]; text += ':';
    append_number(text, e.data_length);
    if (status < 0xF0) {
        text += ':';
        for (int i = 0; i < 3 && i < e.data_length; ++i) {
            uint8_t value = e.data_byte(i);
            text += digits[value >> 4];
            text += digits[value & 15];
        }
    }
}

std::string ev_text(const EventView& e)
{
    std::string text;
    append_event(text, e);
    return text;
}

std::string item_text(const OutputWorkItem& item)
{
    return s(item.generation_id) + "/" + s(static_cast<int32_t>(item.kind)) + "/" + s(item.source_event_index) + "/" +
        s(item.logical_output_microseconds) + "/" + s(static_cast<int32_t>(item.protection)) + "/" + s(item.occurrence_id) +
        "/" + (item.has_midi_event ? ev_text(item.midi_event) : std::string("-"));
}

std::string items_text(const std::vector<OutputWorkItem>& items)
{
    std::string text = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) text += ",";
        text += item_text(items[i]);
    }
    return text + "]";
}

std::string pacer_state(const ProgressiveRatePacer* pacer)
{
    return pacer == nullptr ? "none" : TraceAccess::pacer(*pacer);
}

std::string adaptive_state(const AdaptiveNoteIntervalClock& adaptive)
{
    return "next " + s(adaptive.next_allowed_boundary()) + " open " + s(adaptive.open_boundary()) + " count " +
        s(TraceAccess::adaptive_count(adaptive));
}

std::string tracker_text(const CompleteNoteTracker& tracker) { return TraceAccess::tracker(tracker); }

int64_t to_i64(const std::string& text) { return std::strtoll(text.c_str(), nullptr, 10); }

EventView parse_event(const ReplayContext& context, const std::string& token)
{
    if (token == "x") return EventView();
    size_t at = token.find('@'), hash = token.find('#');
    size_t end = at != std::string::npos ? at : hash != std::string::npos ? hash : token.size();
    EventView e = context.events.at(static_cast<int32_t>(to_i64(token.substr(1, end - 1))));
    if (at != std::string::npos)
        e = e.with_output_channel(static_cast<int>(to_i64(token.substr(at + 1,
            (hash != std::string::npos ? hash : token.size()) - at - 1))));
    if (hash != std::string::npos) e = e.with_logical_event_index(static_cast<int32_t>(to_i64(token.substr(hash + 1))));
    return e;
}

namespace {

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t next = text.find(separator, start);
        parts.push_back(text.substr(start, next == std::string::npos ? std::string::npos : next - start));
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return parts;
}

ServiceDurationMode parse_mode(const std::string& name)
{
    return name == "proc" ? ServiceDurationMode::ProcessingTime
        : name == "bitrate" ? ServiceDurationMode::MidiBitrate : ServiceDurationMode::EventsPerSecond;
}

std::string clock_state(const ReplayContext& c) { return "rem " + s(TraceAccess::clock_remainder(*c.clock)); }

} // namespace

// ================================================================ group 1

bool replay_rate_op(ReplayContext& c, const Args& a, OpOutput& out)
{
    const std::string& op = a[0];
    if (op == "settings.immediate") {
        out.set(b(service_settings_immediate(parse_mode(a[1]), to_i64(a[2]), to_i64(a[4]))));
        return true;
    }
    if (op == "new" && a[1] == "clock") {
        out.state = [&c] { return clock_state(c); };
        c.clock.reset(new ServiceDurationClock(parse_mode(a[2]), to_i64(a[3]), to_i64(a[4]), to_i64(a[5])));
        return true;
    }
    if (op.compare(0, 6, "clock.") == 0) {
        out.state = [&c] { return clock_state(c); };
        if (op == "clock.immediate") out.set(b(c.clock->is_immediate()));
        else if (op == "clock.next") out.set(s(c.clock->next_microseconds(parse_event(c, a[1]))));
        else if (op == "clock.total") out.set(s(c.clock->total_for_next_events(to_i64(a[1]))));
        else if (op == "clock.peek") {
            ServiceDurationClock copy = *c.clock;
            out.set(s(copy.next_microseconds(parse_event(c, a[1]))));
        }
        else return false;
        return true;
    }
    if (op == "svc.calc") {
        out.set(s(calculate_service_microseconds(parse_event(c, a[1]), parse_mode(a[2]), to_i64(a[3]), to_i64(a[4]))));
        return true;
    }
    if (op == "svc.bitrate") {
        out.set(s(calculate_bitrate_microseconds(static_cast<int32_t>(to_i64(a[1])), to_i64(a[2]))));
        return true;
    }
    if (op == "new" && a[1] == "pacer") {
        out.state = [&c] { return pacer_state(c.pacer.get()); };
        c.pacer.reset();
        c.pacer.reset(new ProgressiveRatePacer(to_i64(a[2]), to_i64(a[3]), to_i64(a[4])));
        return true;
    }
    if (op.compare(0, 6, "pacer.") == 0) {
        out.state = [&c] { return pacer_state(c.pacer.get()); };
        ProgressiveRatePacer& p = *c.pacer;
        if (op == "pacer.consume") out.set(b(p.try_consume(to_i64(a[1]))));
        else if (op == "pacer.avail") out.set(s(p.available_slots(to_i64(a[1]), to_i64(a[2]))));
        else if (op == "pacer.reserve") out.set(b(p.try_reserve(to_i64(a[1]))));
        else if (op == "pacer.ahead") out.set(b(p.try_reserve_ahead(to_i64(a[1]))));
        else if (op == "pacer.rolling") out.set(s(p.rolling_available_slots(to_i64(a[1]), to_i64(a[2]))));
        else if (op == "pacer.complete") p.complete_reservation(to_i64(a[1]));
        else if (op == "pacer.cancel") p.cancel_reservation();
        else return false;
        return true;
    }
    if (op == "new" && a[1] == "adaptive") {
        out.state = [&c] { return adaptive_family_state(c); };
        c.adaptive.reset();
        c.adaptive.reset(new AdaptiveNoteIntervalClock(to_i64(a[2]), a[3] == "1"));
        return true;
    }
    if (op == "adaptive.smul") { out.set(s(AdaptiveNoteIntervalClock::saturating_multiply(to_i64(a[1]), to_i64(a[2])))); return true; }
    if (op == "adaptive.sadd") { out.set(s(AdaptiveNoteIntervalClock::saturating_add(to_i64(a[1]), to_i64(a[2])))); return true; }
    if (op.compare(0, 9, "adaptive.") == 0) {
        out.state = [&c] { return adaptive_family_state(c); };
        AdaptiveNoteIntervalClock& ad = *c.adaptive;
        if (op == "adaptive.effective") out.set(s(ad.effective_boundary(to_i64(a[1]))));
        else if (op == "adaptive.pass") ad.record_boundary_pass(to_i64(a[1]), static_cast<int32_t>(to_i64(a[2])));
        else if (op == "adaptive.complete") out.set(b(ad.complete_if_closed(to_i64(a[1]), to_i64(a[2]))));
        else if (op == "adaptive.clear") ad.clear();
        else return false;
        return true;
    }
    return false;
}

// ================================================================ group 2

namespace {

std::string entry_text(const PendingMidiQueue::Entry& e)
{
    return s(e.event_index) + " " + s(e.paired_attack_index) + " " + b(e.is_note_on) + " " + b(e.is_note_off);
}

std::string match_text(const CompleteNoteTracker::NoteOffMatch& m)
{
    return s(m.attack_index) + " " + b(m.accepted) + " " + b(m.suppress) + " " + b(m.filtered);
}

std::string tracker_state(const ReplayContext& c)
{
    return "a" + s(TraceAccess::tracker_accepted(*c.tracker)) + " d" + digest(TraceAccess::tracker(*c.tracker));
}

} // namespace

bool replay_queue_op(ReplayContext& c, const Args& a, OpOutput& out)
{
    const std::string& op = a[0];
    if (op == "new" && a[1] == "pending") {
        out.state = [&c] { return c.pending ? TraceAccess::pending(*c.pending) : std::string("none"); };
        c.pending.reset(new PendingMidiQueue(static_cast<int32_t>(to_i64(a[2])), a.size() > 3 && a[3] == "1"));
        return true;
    }
    if (op.compare(0, 8, "pending.") == 0) {
        out.state = [&c] { return TraceAccess::pending(*c.pending); };
        PendingMidiQueue& q = *c.pending;
        int32_t attack = 0, release = 0;
        if (op == "pending.enq") {
            PendingMidiQueue::Entry e;
            e.event_index = static_cast<int32_t>(to_i64(a[1]));
            e.paired_attack_index = static_cast<int32_t>(to_i64(a[2]));
            e.is_note_on = a[3] == "1";
            e.is_note_off = a[4] == "1";
            q.enqueue(e);
        }
        else if (op == "pending.deq") out.set(entry_text(q.dequeue_entry()));
        else if (op == "pending.evict-oldest") {
            bool ok = q.try_evict_oldest_complete_note(attack, release);
            out.set(b(ok) + " " + s(attack) + " " + s(release));
        }
        else if (op == "pending.evict-head") {
            bool ok = q.try_evict_oldest_complete_note_at_head(attack, release);
            out.set(b(ok) + " " + s(attack) + " " + s(release));
        }
        else if (op == "pending.evict-least") {
            int32_t cursor = static_cast<int32_t>(to_i64(a[1]));
            bool ok = q.try_evict_least_important_complete_note(c.events, cursor, attack, release);
            out.set(b(ok) + " " + s(attack) + " " + s(release) + " " + s(cursor));
        }
        else if (op == "pending.index") q.set_note_index_enabled(a[1] == "1");
        else if (op == "pending.clear") q.clear();
        else return false;
        return true;
    }
    if (op == "new" && a[1] == "fdq") {
        out.state = [&c] { return c.fdq ? TraceAccess::fdq(*c.fdq) : std::string("none"); };
        c.fdq.reset(new ForwardDropQueue(static_cast<int32_t>(to_i64(a[2]))));
        return true;
    }
    if (op.compare(0, 4, "fdq.") == 0) {
        out.state = [&c] { return TraceAccess::fdq(*c.fdq); };
        if (op == "fdq.advance") c.fdq->advance(to_i64(a[1]));
        else if (op == "fdq.admit") out.set(b(c.fdq->try_admit(to_i64(a[1]), to_i64(a[2]), a[3] == "1")));
        else return false;
        return true;
    }
    if (op == "new" && a[1] == "tracker") {
        out.state = [&c] { return tracker_state(c); };
        c.tracker.reset(new CompleteNoteTracker());
        return true;
    }
    if (op.compare(0, 8, "tracker.") == 0) {
        out.state = [&c] { return tracker_state(c); };
        CompleteNoteTracker& t = *c.tracker;
        if (op == "tracker.classify") out.set(s(static_cast<int32_t>(CompleteNoteTracker::classify(parse_event(c, a[1])))));
        else if (op == "tracker.safety") out.set(b(CompleteNoteTracker::is_safety_control(parse_event(c, a[1]))));
        else if (op == "tracker.on")
            t.record_note_on(parse_event(c, a[1]), static_cast<int32_t>(to_i64(a[2])), a[3] == "1", a[4] == "1");
        else if (op == "tracker.filtered") t.record_filtered_note_on(parse_event(c, a[1]), static_cast<int32_t>(to_i64(a[2])));
        else if (op == "tracker.off") out.set(match_text(t.take_note_off(parse_event(c, a[1]))));
        else if (op == "tracker.suppress") out.set(b(t.should_suppress_note_off(parse_event(c, a[1]))));
        else if (op == "tracker.evicted") t.mark_unsent_attack_evicted(static_cast<int32_t>(to_i64(a[1])));
        else if (op == "tracker.mfiltered") t.mark_unsent_attack_filtered(static_cast<int32_t>(to_i64(a[1])));
        else return false;
        return true;
    }
    if (op == "select") {
        int32_t cursor = static_cast<int32_t>(to_i64(a[1]));
        int32_t capacity = static_cast<int32_t>(to_i64(a[2]));
        int32_t count = static_cast<int32_t>(to_i64(a[3]));
        std::vector<MusicalNotePriorityCandidate> candidates;
        for (const std::string& item : split(a[4], ';')) {
            std::vector<std::string> f = split(item, ',');
            MusicalNotePriorityCandidate m;
            m.pitch = static_cast<int32_t>(to_i64(f[0]));
            m.velocity = static_cast<int32_t>(to_i64(f[1]));
            m.track = static_cast<int32_t>(to_i64(f[2]));
            m.channel = static_cast<int32_t>(to_i64(f[3]));
            m.source_order = static_cast<int32_t>(to_i64(f[4]));
            m.intended_microseconds = to_i64(f[5]);
            m.duration_microseconds = to_i64(f[6]);
            candidates.push_back(m);
        }
        int32_t victim = c.selector.select_least_important(candidates.data(), capacity, count, cursor);
        out.set(s(victim) + " " + s(c.selector.next_fairness_cursor(candidates[static_cast<size_t>(victim)])));
        return true;
    }
    return false;
}

// ================================================================ ordered

namespace {

CaptureDecision make_decision(const std::string& spec)
{
    if (spec == "none") return CaptureDecision();
    if (spec == "badid")
        return [](const EventView& e) { return OrderedFiniteCapturedDecision{ e.with_logical_event_index(e.event_index() + 1), false }; };
    if (spec == "invalid") return [](const EventView&) { return OrderedFiniteCapturedDecision{ EventView(), false }; };
    std::vector<std::string> parts = split(spec, ':');
    int mask = static_cast<int>(std::strtol(parts[1].c_str(), nullptr, 16));
    if (parts[0] == "mask")
        return [mask](const EventView& e) {
            return OrderedFiniteCapturedDecision{ e, e.channel() >= 0 && ((mask >> e.channel()) & 1) != 0 };
        };
    int shift = static_cast<int>(to_i64(parts[2]));
    return [mask, shift](const EventView& e) {
        bool filtered = e.channel() >= 0 && ((mask >> e.channel()) & 1) != 0;
        if (filtered || e.channel() < 0) return OrderedFiniteCapturedDecision{ e, filtered };
        return OrderedFiniteCapturedDecision{ e.with_output_channel((e.channel() + shift) % 16), false };
    };
}

bool parse_chase(const ReplayContext& c, const std::string& token, std::vector<EventView>& chase)
{
    if (token == "-") return false;
    std::string inner = token.substr(1, token.size() - 2);
    if (!inner.empty())
        for (const std::string& item : split(inner, ',')) chase.push_back(parse_event(c, item));
    return true;
}

std::string admission_text(const OrderedFiniteAdmissionResult& r)
{
    return s(static_cast<int32_t>(r.disposition)) + " " + b(r.paired_release_suppressed) + " " + items_text(r.evicted);
}

std::string clear_text(const OrderedFiniteClearResult& r)
{
    return items_text(r.cleared_work) + " " + s(r.first_skipped_source_event_index) + " " + s(r.source_event_cursor_after) +
        " " + s(r.dropped_skipped_source_events) + " " + s(r.filtered_skipped_source_events) + " " + s(r.dropped_events()) +
        " " + b(r.requires_safety_reset());
}

std::string ordered_state(const ReplayContext& c)
{
    if (!c.ordered) return "none";
    const OrderedFiniteAdmissionCore& o = *c.ordered;
    std::string text;
    for (const auto& g : o.generations().generations()) {
        text += s(g->id) + "," + b(g->is_admission_closed()) + "," + s(g->source_event_cursor) + "," +
            s(g->source_clock_anchor_ticks) + "," + s(g->frozen_elapsed_ticks) + "," + s(g->target_source_microseconds) + "," +
            s(g->chase_prefix_count()) + "," + s(g->mode_state->musical_fairness_cursor) + ":";
        for (const OutputWorkItem& item : g->admitted()) text += item_text(item) + ";";
        text += ":" + tracker_text(g->mode_state->complete_notes) + "\n";
    }
    return "live" + s(o.live_generation_count()) + " pend" + s(o.pending_count()) + " out" + s(o.outstanding_count()) +
        " next" + s(o.next_generation_id()) + " svc" + (o.has_in_service() ? item_text(o.in_service()) : std::string("-")) +
        " d" + digest(text);
}

OverflowPolicy policy_of(const std::string& text) { return static_cast<OverflowPolicy>(to_i64(text)); }

} // namespace

bool replay_ordered_op(ReplayContext& c, const Args& a, OpOutput& out)
{
    const std::string& op = a[0];
    if (op == "new" && a[1] == "ordered") {
        out.state = [&c] { return ordered_state(c); };
        c.ordered.reset(new OrderedFiniteAdmissionCore(c.events, static_cast<int32_t>(to_i64(a[2]))));
        return true;
    }
    if (op.compare(0, 8, "ordered.") != 0) return false;
    out.state = [&c] { return ordered_state(c); };
    OrderedFiniteAdmissionCore& o = *c.ordered;
    if (op == "ordered.start" || op == "ordered.seek") {
        std::vector<EventView> chase;
        bool hasChase = parse_chase(c, a[3], chase);
        int64_t target = to_i64(a[1]);
        int32_t cursor = static_cast<int32_t>(to_i64(a[2]));
        if (op == "ordered.start") o.start(target, cursor, hasChase ? &chase : nullptr, to_i64(a[4]), to_i64(a[5]));
        else out.set(b(o.try_append_seek(target, cursor, hasChase ? &chase : nullptr, to_i64(a[4]), to_i64(a[5]))));
    }
    else if (op == "ordered.admit")
        out.set(admission_text(o.admit_next_source(static_cast<int32_t>(to_i64(a[1])), policy_of(a[2]))));
    else if (op == "ordered.admitx")
        out.set(admission_text(o.admit_next_source(parse_event(c, a[1]), a[2] == "1", static_cast<int32_t>(to_i64(a[3])),
            policy_of(a[4]))));
    else if (op == "ordered.begin" || op == "ordered.peek") {
        OutputWorkItem item;
        bool ok = op == "ordered.begin" ? o.try_begin_dispatch(item) : o.try_peek_dispatch(item);
        out.set(b(ok) + " " + (ok ? item_text(item) : std::string("-")));
    }
    else if (op == "ordered.complete") o.complete_dispatch();
    else if (op == "ordered.pressure") {
        OrderedFinitePressureSnapshot p = o.measure_due_pressure(to_i64(a[1]), make_decision(a[2]));
        out.set(s(p.admitted_outstanding_count) + " " + s(p.external_due_count) + " " + s(p.total_due_count) + " " +
            s(p.oldest_due_microseconds) + " " + s(p.current_age_microseconds));
    }
    else if (op == "ordered.repair") {
        OrderedFiniteAgeRepairResult r = o.repair_waiting_time_pressure(to_i64(a[1]), to_i64(a[2]), policy_of(a[3]),
            make_decision(a[4]));
        out.set(items_text(r.evicted) + " " + (r.has_clear_result ? clear_text(r.clear_result) : std::string("-")) + " " +
            b(r.stalled_by_in_service_or_preservation_barrier) + " " + s(r.age_after_microseconds));
    }
    else if (op == "ordered.clear") out.set(clear_text(o.clear_open_tail_and_catch_up(to_i64(a[1]), make_decision(a[2]))));
    else if (op == "ordered.shift") o.shift_source_clock_anchors(to_i64(a[1]));
    else if (op == "ordered.atcap") out.set(b(o.is_at_or_above_capacity(static_cast<int32_t>(to_i64(a[1])))));
    else return false;
    return true;
}

} // namespace models

// ================================================================ driver

std::string replay_model_trace(const std::string& script, const FixtureLoader& loader, const std::string& producer)
{
    using namespace models;
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start < script.size()) {
            size_t end = script.find('\n', start);
            if (end == std::string::npos) end = script.size();
            std::string line = script.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            start = end + 1;
        }
    }
    ReplayContext context;
    context.loader = &loader;
    std::string out = "# producer " + producer + "\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == '#') continue;
        if (line.size() >= 2 && line[1] == ' ' && (line[0] == '=' || line[0] == '~' || line[0] == '!')) continue;
        out += line + "\n";
        if (line.compare(0, 7, "family ") == 0) context.family = line.substr(7);
        if (line.compare(0, 2, "> ") != 0) continue;

        size_t j = i + 1;
        while (j < lines.size() && lines[j].size() >= 2 && lines[j][1] == ' ' && (lines[j][0] == '=' || lines[j][0] == '!')) ++j;
        bool wantState = j < lines.size() && lines[j].compare(0, 2, "~ ") == 0;

        Args args = split(line.substr(2), ' ');
        OpOutput result;
        try {
            if (args[0] == "fixture") {
                std::string error;
                context.song = std::shared_ptr<Song>(loader(args[1], error).release());
                context.song_serial++;
                context.events.store = context.song ? &context.song->store : nullptr;
                if (!context.song) result.set("load failed: " + error);
                else result.set("events " + s(context.events.count()) + " duration " + s(context.song->duration_us));
            }
            else if (!replay_rate_op(context, args, result) && !replay_queue_op(context, args, result) &&
                !replay_ordered_op(context, args, result) && !replay_gate_op(context, args, result) &&
                !replay_analysis_op(context, args, result))
                result.set("? unknown op");
            if (result.has_result) out += "= " + result.result + "\n";
        }
        catch (const ModelException& e) {
            out += std::string("! ") + e.type + " " + (e.param.empty() ? "-" : e.param) + " " + e.message + "\n";
        }
        if (wantState) out += "~ " + (result.state ? result.state() : std::string("? no state")) + "\n";
    }
    return out;
}

} // namespace midibn
