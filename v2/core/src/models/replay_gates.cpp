// Model-trace replay for the gate families (Milestone 3 groups 3-6). State
// texts match V1ModelHarness.Gates.cs exactly.
#include "replay_internal.h"

#include "midibn/models/delayed_note_off_gate.h"
#include "midibn/models/per_note_gate.h"
#include "midibn/models/rate_gates.h"

namespace midibn {
namespace models {

struct GateInstances {
    std::unique_ptr<PerNoteIntervalGate> per_note;
    std::unique_ptr<DelayedNoteOffGate> delayed;
    std::unique_ptr<MusicalAttackRateLimiter> attack;
    std::unique_ptr<OutputRateGate> output_rate;
};

void GateInstancesDeleter::operator()(GateInstances* p) const { delete p; }

// Appends without temporaries: these texts are built after every op.
struct TextOut {
    std::string& t;
    TextOut& n(int64_t v) { append_number(t, v); return *this; }
    TextOut& c(char ch) { t += ch; return *this; }
    TextOut& f(bool v) { t += v ? '1' : '0'; return *this; }
    TextOut& e(const EventView& v) { append_event(t, v); return *this; }
};

struct GateTraceAccess {
    template <typename Occurrence> static void per_note_fields(TextOut& o, const Occurrence& x)
    {
        o.f(x.in_use).c(',').f(x.ended).c(',').f(x.removed_by_routing).c(',').n(x.disposition).c(',').n(x.track).c(',')
            .n(x.channel).c(',').n(x.pitch).c(',').n(x.next_active).c(',').n(x.next_group).c(',').n(x.next_free).c(',')
            .e(x.note_on).c(',').e(x.note_off);
    }

    static std::string per_note(const PerNoteIntervalGate* g)
    {
        if (g == nullptr) return "none";
        std::string t;
        t.reserve(1 << 16);
        TextOut o{ t };
        for (size_t p = 0; p < 128; p++) {
            o.n(static_cast<int64_t>(p)).c(':').f(g->output_down_[p]);
            for (int64_t v : { g->minimum_release_boundary_[p], g->next_attack_boundary_[p], g->attack_boundary_[p],
                     g->preparatory_release_boundary_[p], g->natural_release_boundary_[p], g->candidate_boundary_[p],
                     g->release_carry_order_[p] })
                o.c(',').n(v);
            for (int32_t v : { g->output_channel_[p], g->selected_support_count_[p], g->candidate_head_[p],
                     g->candidate_representative_[p], g->candidate_members_[p], g->tick_head_[p], g->tick_tail_[p],
                     g->tick_representative_[p], g->tick_members_[p] })
                o.c(',').n(v);
            o.c(',').e(g->pending_attack_[p]).c(',').e(g->natural_release_[p]).c(';');
            for (int32_t start : { g->tick_head_[p], g->candidate_head_[p] })
                for (int32_t node = start; node >= 0; node = g->occurrence(node).next_group) {
                    o.c('g').n(node).c(':');
                    per_note_fields(o, g->occurrence(node));
                    o.c(';');
                }
        }
        for (size_t k = 0; k < g->active_head_.size(); k++) {
            if (g->active_head_[k] < 0 && g->active_tail_[k] < 0) continue;
            o.c('k').n(static_cast<int64_t>(k)).c('/').n(g->active_tail_[k]).c(':');
            for (int32_t node = g->active_head_[k]; node >= 0; node = g->occurrence(node).next_active) {
                o.n(node).c(':');
                per_note_fields(o, g->occurrence(node));
                o.c(';');
            }
        }
        return "nb" + s(g->next_boundary_microseconds()) + " oc" + s(g->next_boundary_output_count()) + " f" +
            s(g->filtered_events_) + " st" + s(g->source_tick_) + " fr" + s(g->free_occurrence_) + " nu" +
            s(g->next_unused_occurrence_) + " sc" + b(g->source_complete_) + " rc" + s(g->next_release_carry_order_) +
            " fc" + s(g->musical_fairness_cursor_) + " seg" + s(g->occurrence_segment_count()) + " d" + digest(t);
    }

    static std::string delayed(const DelayedNoteOffGate* g)
    {
        if (g == nullptr) return "none";
        std::string t;
        t.reserve(1 << 16);
        TextOut o{ t };
        for (size_t p = 0; p < 128; p++) {
            o.n(static_cast<int64_t>(p)).c(':').n(g->state_[p]).c(',').n(g->owner_occurrence_[p]).c(',')
                .n(g->candidate_occurrence_[p]).c(',').f(g->release_pending_[p]).c(',').f(g->release_for_retrigger_[p]);
            for (int64_t v : { g->attack_due_[p], g->attack_emission_[p], g->attack_order_[p], g->release_due_[p],
                     g->release_order_[p], g->recovery_due_[p], g->recovery_order_[p] })
                o.c(',').n(v);
            o.c(',').e(g->attack_[p]).c(',').e(g->release_[p]).c(';');
        }
        for (bool down : g->sustain_down_) o.f(down);
        for (size_t k = 0; k < g->active_head_.size(); k++) {
            if (g->active_head_[k] < 0 && g->active_tail_[k] < 0) continue;
            o.c('k').n(static_cast<int64_t>(k)).c('/').n(g->active_tail_[k]).c(':');
            for (int32_t node = g->active_head_[k]; node >= 0; node = g->occurrence(node).next_active) {
                const auto& x = g->occurrence(node);
                o.n(node).c(':').f(x.in_use).c(',').f(x.key_released).c(',').f(x.sustain_held).c(',').n(x.disposition).c(',')
                    .n(x.track).c(',').n(x.channel).c(',').n(x.pitch).c(',').n(x.previous_active).c(',').n(x.next_active).c(',')
                    .n(x.next_free).c(',').n(x.order).c(',').e(x.note_on).c(',').e(x.note_off).c(';');
            }
        }
        return "nb" + s(g->next_boundary_microseconds()) + " oc" + s(g->next_boundary_output_count()) + " f" +
            s(g->filtered_events_) + " id" + s(g->inside_duration_filtered_) + " rl" + s(g->recovery_lost_filtered_) +
            " re" + s(g->recovery_ended_filtered_) + " so" + s(g->source_order_) + " sc" + b(g->source_complete_) +
            " live" + s(g->live_occurrences_) + " fr" + s(g->free_occurrence_) + " nu" + s(g->next_unused_occurrence_) +
            " fc" + s(g->musical_fairness_cursor_) + " seg" + s(g->occurrence_segment_count()) + " d" + digest(t);
    }
    static std::string attack(const MusicalAttackRateLimiter* g)
    {
        if (g == nullptr) return "none";
        std::string t;
        for (size_t slot = 0; slot < g->head_.size(); slot++) {
            if (g->head_[slot] < 0 && g->tail_[slot] < 0) continue;
            t += s(static_cast<int64_t>(slot)) + "/" + s(g->tail_[slot]) + ":";
            for (int32_t node = g->head_[slot]; node >= 0; node = g->get(node).next) {
                const auto& o = g->get(node);
                t += s(node) + ":" + b(o.in_use) + "," + b(o.accepted) + "," + s(o.channel) + "," + s(o.pitch) + "," +
                    s(o.previous) + "," + s(o.next) + "," + s(o.next_free) + ";";
            }
        }
        return "live" + s(g->live_) + " fr" + s(g->free_) + " nu" + s(g->next_unused_) + " seg" +
            s(g->occurrence_segment_count()) + " fc" + s(g->fairness_cursor_) + " rj" + s(g->rejected_attacks_) + " sp" +
            s(g->suppressed_releases_) + " te" + s(static_cast<int64_t>(g->tick_events_.size())) + " p[" +
            pacer_state(&g->pacer_) + "] d" + digest(t);
    }

    static std::string output_rate(const OutputRateGate* g)
    {
        if (g == nullptr) return "none";
        std::string t;
        for (size_t slot = 0; slot < g->head_.size(); slot++) {
            if (g->head_[slot] < 0 && g->tail_[slot] < 0) continue;
            t += s(static_cast<int64_t>(slot)) + "/" + s(g->tail_[slot]) + ":";
            for (int32_t node = g->head_[slot]; node >= 0; node = g->get(node).next) {
                const auto& o = g->get(node);
                t += s(node) + ":" + b(o.in_use) + "," + b(o.accepted) + "," + s(o.candidate_index) + "," + s(o.channel) + "," +
                    s(o.pitch) + "," + s(o.previous) + "," + s(o.next) + "," + s(o.next_free) + ";";
            }
        }
        std::string pending;
        for (bool value : g->pending_required_release_) pending += b(value);
        return "live" + s(g->live_) + " fr" + s(g->free_) + " nu" + s(g->next_unused_) + " fc" + s(g->fairness_cursor_) +
            " rj" + s(g->rejected_attacks_) + " sp" + s(g->suppressed_releases_) + " ro" + s(g->rejected_other_messages_) +
            " te" + s(static_cast<int64_t>(g->tick_events_.size())) + " q" + pending + " p[" + pacer_state(&g->pacer_) +
            "] d" + digest(t);
    }
};

namespace {

GateInstances& gates(ReplayContext& c)
{
    if (!c.gates) c.gates.reset(new GateInstances());
    return *c.gates;
}

std::string emissions_text(const EventView* emissions, int32_t count)
{
    std::string text = s(count) + " [";
    for (int32_t i = 0; i < count; i++) {
        if (i > 0) text += ",";
        text += ev_text(emissions[i]);
    }
    return text + "]";
}

std::string with_adaptive(const ReplayContext& c, const std::string& gate)
{
    return gate + " | " + (c.adaptive ? adaptive_state(*c.adaptive) : std::string("none"));
}

int32_t i32(const std::string& text) { return static_cast<int32_t>(to_i64(text)); }

} // namespace

// The adaptive clock's state line: alone in the rate family, beside its
// gate in the interval-gate families.
std::string adaptive_family_state(ReplayContext& c)
{
    if (c.family == "pernote") return with_adaptive(c, GateTraceAccess::per_note(gates(c).per_note.get()));
    if (c.family == "delayed") return with_adaptive(c, GateTraceAccess::delayed(gates(c).delayed.get()));
    return c.adaptive ? adaptive_state(*c.adaptive) : std::string("none");
}

bool replay_gate_op(ReplayContext& c, const Args& a, OpOutput& out)
{
    const std::string& op = a[0];
    GateInstances& g = gates(c);

    // ------------------------------------------------------------ pernote
    if (op == "pernote.query") {
        int32_t pitch = i32(a[1]);
        out.set(b(g.per_note->is_active(pitch)) + " " + s(g.per_note->owner_channel(pitch)) + " " +
            b(g.per_note->has_active_occurrence(i32(a[2]), i32(a[3]), pitch)));
        return true;
    }
    if (op == "pernote.classify") {
        int32_t pitch;
        bool on;
        bool ok = PerNoteIntervalGate::try_classify(parse_event(c, a[1]), pitch, on);
        out.set(b(ok) + " " + s(pitch) + " " + b(on));
        return true;
    }
    if (op == "pernote.better") {
        out.set(b(PerNoteIntervalGate::is_better_candidate(parse_event(c, a[1]), parse_event(c, a[2]))));
        return true;
    }
    if ((op == "new" && a[1] == "pernote") || op.compare(0, 8, "pernote.") == 0) {
        out.state = [&c] { return adaptive_family_state(c); };
        if (op == "new") {
            g.per_note.reset();
            g.per_note.reset(new PerNoteIntervalGate(to_i64(a[2]), a[3] == "1", i32(a[4])));
            return true;
        }
        PerNoteIntervalGate& gate = *g.per_note;
        if (op == "pernote.begin") gate.begin_source_tick(to_i64(a[1]));
        else if (op == "pernote.admit") out.set(s(static_cast<int32_t>(gate.admit(parse_event(c, a[1])))));
        else if (op == "pernote.end") gate.end_source_tick();
        else if (op == "pernote.take") out.set(s(gate.take_filtered_event_count()));
        else if (op == "pernote.emit") {
            int32_t length = i32(a[2]);
            std::vector<EventView> buffer(static_cast<size_t>(length));
            int32_t count = gate.emit_boundary(to_i64(a[1]), buffer.data(), length);
            out.set(emissions_text(buffer.data(), count));
        }
        else if (op == "pernote.complete") gate.complete_source(to_i64(a[1]));
        else if (op == "pernote.retire") out.set(s(gate.retire_channel(i32(a[1]), to_i64(a[2]))));
        else return false;
        return true;
    }

    // ------------------------------------------------------------ delayed
    if (op == "delayed.precedes") {
        out.set(b(g.delayed->source_precedes_boundary(parse_event(c, a[1]), to_i64(a[2]))));
        return true;
    }
    if (op == "delayed.precedes1") {
        out.set(b(g.delayed->source_precedes_boundary(parse_event(c, a[1]))));
        return true;
    }
    if (op == "delayed.terminating") {
        out.set(b(DelayedNoteOffGate::is_note_terminating_control(parse_event(c, a[1]))));
        return true;
    }
    if ((op == "new" && a[1] == "delayed") || op.compare(0, 8, "delayed.") == 0) {
        out.state = [&c] { return adaptive_family_state(c); };
        if (op == "new") {
            g.delayed.reset();
            g.delayed.reset(new DelayedNoteOffGate(to_i64(a[2]), a[3] == "1", i32(a[4])));
            return true;
        }
        DelayedNoteOffGate& gate = *g.delayed;
        if (op == "delayed.admit") out.set(s(static_cast<int32_t>(gate.admit(parse_event(c, a[1])))));
        else if (op == "delayed.emit") {
            std::vector<EventView> emissions;
            int32_t count = gate.emit_boundary(to_i64(a[1]), emissions);
            out.set(emissions_text(emissions.data(), count));
        }
        else if (op == "delayed.take") out.set(s(gate.take_filtered_event_count()));
        else if (op == "delayed.complete") gate.complete_source(to_i64(a[1]));
        else if (op == "delayed.retire") out.set(s(gate.retire_channel(i32(a[1]), to_i64(a[2]))));
        else return false;
        return true;
    }

    // ------------------------------------------------------------- attack
    if ((op == "new" && a[1] == "attack") || op.compare(0, 7, "attack.") == 0) {
        out.state = [&c] { return GateTraceAccess::attack(gates(c).attack.get()); };
        if (op == "new") {
            g.attack.reset();
            g.attack.reset(new MusicalAttackRateLimiter(to_i64(a[2])));
            return true;
        }
        MusicalAttackRateLimiter& gate = *g.attack;
        if (op == "attack.begin") gate.begin_source_tick(to_i64(a[1]));
        else if (op == "attack.admit") gate.admit(parse_event(c, a[1]));
        else if (op == "attack.end") {
            std::vector<EventView> emissions;
            int32_t count = gate.end_source_tick(emissions);
            out.set(emissions_text(emissions.data(), count));
        }
        else if (op == "attack.take-rejected") out.set(s(gate.take_rejected_attacks()));
        else if (op == "attack.take-suppressed") out.set(s(gate.take_suppressed_releases()));
        else if (op == "attack.retire") gate.retire_channel(i32(a[1]));
        else if (op == "attack.avail") out.set(s(gate.available_starts(to_i64(a[1]), to_i64(a[2]))));
        else if (op == "attack.complete") gate.complete_source();
        else return false;
        return true;
    }

    // --------------------------------------------------------- outputrate
    if (op == "outrate.protected") {
        out.set(b(OutputRateGate::is_protected_traffic(parse_event(c, a[1]))));
        return true;
    }
    if ((op == "new" && a[1] == "outrate") || op.compare(0, 8, "outrate.") == 0) {
        out.state = [&c] { return GateTraceAccess::output_rate(gates(c).output_rate.get()); };
        if (op == "new") {
            g.output_rate.reset();
            g.output_rate.reset(new OutputRateGate(to_i64(a[2]), to_i64(a[3])));
            return true;
        }
        OutputRateGate& gate = *g.output_rate;
        if (op == "outrate.begin") gate.begin_source_tick(to_i64(a[1]));
        else if (op == "outrate.admit") gate.admit(parse_event(c, a[1]));
        else if (op == "outrate.end") {
            std::vector<EventView> emissions;
            int32_t count = gate.end_source_tick(emissions, to_i64(a[1]));
            out.set(emissions_text(emissions.data(), count));
        }
        else if (op == "outrate.take") {
            int64_t rejected = gate.take_rejected_attacks();
            int64_t suppressed = gate.take_suppressed_releases();
            int64_t other = gate.take_rejected_other_messages();
            out.set(s(rejected) + " " + s(suppressed) + " " + s(other));
        }
        else if (op == "outrate.dispatch") out.set(b(gate.record_dispatch(to_i64(a[1]), a[2] == "1")));
        else if (op == "outrate.exceptional") out.set(b(gate.record_exceptional_dispatch(to_i64(a[1]))));
        else if (op == "outrate.retire") gate.retire_channel(i32(a[1]));
        else if (op == "outrate.avail") out.set(s(gate.available_events(to_i64(a[1]))));
        else if (op == "outrate.avail2") out.set(s(gate.available_events(to_i64(a[1]), to_i64(a[2]))));
        else if (op == "outrate.complete") gate.complete_source();
        else return false;
        return true;
    }
    return false;
}

} // namespace models
} // namespace midibn
