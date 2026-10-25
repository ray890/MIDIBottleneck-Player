// First-party unit tests (no framework). Exit code 0 when all pass.
#include "midibn/channel_index.h"
#include "midibn/event_store.h"
#include "midibn/load_progress.h"
#include "midibn/midi_loader.h"
#include "midibn/net_decimal.h"
#include "midibn/tempo_clock.h"

#include "midibn/analysis/fast_pending_queue.h"
#include "midibn/analysis/workload_analysis.h"
#include "midibn/models/model_base.h"
#include "midibn/models/queue.h"
#include "midibn/models/rate.h"
#include "midibn/routing/route_plan_view.h"
#include "midibn/sha256.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

std::string sha(const std::string& text)
{
    midibn::Sha256 h;
    h.update(text);
    return h.hex_digest();
}

// P13-ANA: the Analysis's own pending queue (analysis::FastPendingQueue) must make exactly the same choices as the
// engine's models::PendingMidiQueue (the reference with the linear ranking scan), for every queue operation the
// overflow policies use, on random queues with many equal velocities, chords and shared lanes.
struct QueueTestRandom {
    uint64_t state;
    explicit QueueTestRandom(uint64_t seed) : state(seed * 2654435761ull + 88172645463325252ull) {}
    uint32_t next(uint32_t bound)
    {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return static_cast<uint32_t>((state >> 11) % bound);
    }
};

void test_fast_pending_queue()
{
    using namespace midibn;
    using models::PendingMidiQueue;
    long long comparisons = 0, leastEvictions = 0, withRelease = 0;
    bool allSame = true;
    for (int mode = 0; mode < 3 && allSame; ++mode) {
        const bool noteIndex = mode != 1;
        const bool ranking = mode == 0;
        for (uint64_t seed = 1; seed <= 400 && allSame; ++seed) {
            QueueTestRandom rnd(seed + 1000 * mode);
            models::EventViewList list;
            const int eventCount = 260;
            int64_t micros = 0;
            for (int i = 0; i < eventCount; ++i) {
                if (rnd.next(3) == 0) micros += 1 + rnd.next(5);
                int channel = rnd.next(4) == 0 ? 9 : static_cast<int>(rnd.next(3));
                int track = static_cast<int>(rnd.next(3));
                uint8_t key = static_cast<uint8_t>(40 + rnd.next(4));
                uint8_t velocity = static_cast<uint8_t>(1 + rnd.next(3) * 20);
                uint32_t kind = rnd.next(10);
                if (kind < 5)
                    list.events.push_back(models::EventView::create(micros, micros, track, EventKind::NoteOn, channel,
                        static_cast<uint8_t>(0x90 | channel), i, key, velocity, 3));
                else if (kind < 9)
                    list.events.push_back(models::EventView::create(micros, micros, track, EventKind::NoteOff, channel,
                        static_cast<uint8_t>(0x80 | channel), i, key, 0, 3));
                else
                    list.events.push_back(models::EventView::create(micros, micros, track, EventKind::ControlChange, channel,
                        static_cast<uint8_t>(0xB0 | channel), i, 7, 100, 3));
            }
            models::EventSource source(static_cast<const models::EventViewStore*>(&list));
            PendingMidiQueue reference(1 << 20, noteIndex);
            analysis::FastPendingQueue fast(source, noteIndex, ranking);
            std::vector<std::vector<int32_t>> unmatched(16 * 128);
            std::vector<size_t> unmatchedHead(16 * 128, 0);
            int32_t referenceCursor = 0, fastCursor = 0;
            const int capacity = 8 + static_cast<int>(rnd.next(60));
            auto same = [&]() {
                ++comparisons;
                return reference.count() == fast.count() && reference.oldest_event_index() == fast.oldest_event_index() &&
                    reference.event_indices() == fast.event_indices() && referenceCursor == fastCursor &&
                    (!noteIndex || reference.eligible_count() == fast.eligible_count());
            };
            for (int i = 0; i < eventCount && allSame; ++i) {
                // Some random removals first.
                for (int step = static_cast<int>(rnd.next(3)); step > 0 && allSame; --step) {
                    uint32_t op = rnd.next(100);
                    if (op < 8 && reference.count() > 0) {
                        allSame = reference.dequeue() == fast.dequeue();
                    }
                    else if (noteIndex && op < 20) {
                        int32_t ra = 0, rr = 0, fa = 0, fr = 0;
                        bool r = reference.try_evict_oldest_complete_note(ra, rr), f = fast.try_evict_oldest_complete_note(fa, fr);
                        allSame = r == f && ra == fa && rr == fr;
                    }
                    else if (noteIndex && op < 30) {
                        int32_t ra = 0, rr = 0, fa = 0, fr = 0;
                        bool r = reference.try_evict_oldest_complete_note_at_head(ra, rr),
                            f = fast.try_evict_oldest_complete_note_at_head(fa, fr);
                        allSame = r == f && ra == fa && rr == fr;
                    }
                    else if (noteIndex && ranking && op < 40) {
                        int32_t ra = 0, rr = 0, fa = 0, fr = 0;
                        bool r = reference.try_evict_least_important_complete_note(source, referenceCursor, ra, rr),
                            f = fast.try_evict_least_important_complete_note(fastCursor, fa, fr);
                        allSame = r == f && ra == fa && rr == fr;
                        leastEvictions += r ? 1 : 0;
                        withRelease += r && rr >= 0 ? 1 : 0;
                    }
                    else if (op >= 99) { reference.clear(); fast.clear(); }
                    if (allSame) allSame = same();
                }
                if (!allSame) break;
                while (noteIndex && ranking && reference.count() >= capacity && allSame) {
                    int32_t ra = 0, rr = 0, fa = 0, fr = 0;
                    bool r = reference.try_evict_least_important_complete_note(source, referenceCursor, ra, rr),
                        f = fast.try_evict_least_important_complete_note(fastCursor, fa, fr);
                    allSame = r == f && ra == fa && rr == fr && same();
                    leastEvictions += r ? 1 : 0;
                    withRelease += r && rr >= 0 ? 1 : 0;
                    if (!r) {   // nothing evictable (no queued attack): make room the plain way
                        allSame = reference.dequeue() == fast.dequeue() && same();
                    }
                }
                if (!(noteIndex && ranking) && reference.count() >= capacity && allSame) allSame = reference.dequeue() == fast.dequeue() && same();
                if (!allSame) break;
                // Then the event itself, paired like the complete-note tracker pairs a release with its attack.
                models::EventView view = list.at(i);
                PendingMidiQueue::Entry entry;
                entry.event_index = i;
                entry.paired_attack_index = -1;
                entry.is_note_on = view.kind == EventKind::NoteOn;
                entry.is_note_off = view.kind == EventKind::NoteOff;
                if (view.kind == EventKind::NoteOn || view.kind == EventKind::NoteOff) {
                    size_t slot = static_cast<size_t>(view.channel() * 128 + (view.data_byte(1) & 0x7F));
                    if (entry.is_note_on) unmatched[slot].push_back(i);
                    else if (unmatchedHead[slot] < unmatched[slot].size()) entry.paired_attack_index = unmatched[slot][unmatchedHead[slot]++];
                }
                if (rnd.next(12) == 0) continue;   // dropped by the policy
                reference.enqueue(entry);
                fast.enqueue(entry);
                allSame = same();
            }
        }
    }
    check(allSame, "FastPendingQueue matches PendingMidiQueue on 1200 random queues in three set-ups (every eviction, order, cursor)");
    std::printf("fast-pending-queue: %lld comparisons, %lld least-important evictions (%lld with a queued release)\n",
        comparisons, leastEvictions, withRelease);
    check(leastEvictions > 20000 && withRelease > 1000, "the random queues really exercised the least-important eviction with queued releases");
    check(comparisons > 100000, "the random queues were compared step by step");

    // A changed value in the Analysis window cancels the running projection: the finite-queue projection looks at the
    // cancellation request every 1024 passes of its loop (it used to look only every 16384 events).
    {
        models::EventViewList list;
        const int pairs = 60000;
        for (int i = 0; i < pairs; ++i) {
            int64_t micros = static_cast<int64_t>(i) * 10;
            uint8_t key = static_cast<uint8_t>(30 + i % 50);
            list.events.push_back(models::EventView::create(micros, micros, 0, EventKind::NoteOn, i % 3,
                static_cast<uint8_t>(0x90 | (i % 3)), 2 * i, key, static_cast<uint8_t>(1 + i % 100), 3));
            list.events.push_back(models::EventView::create(micros + 5, micros + 5, 0, EventKind::NoteOff, i % 3,
                static_cast<uint8_t>(0x80 | (i % 3)), 2 * i + 1, key, 0, 3));
        }
        analysis::AnalysisSong song;
        song.events = models::EventSource(static_cast<const models::EventViewStore*>(&list));
        song.duration_us = static_cast<int64_t>(pairs) * 10 + 10;
        auto configuration = std::make_shared<analysis::AnalysisConfiguration>();
        configuration->simulate_slowdown = true;
        configuration->service_duration_mode = models::ServiceDurationMode::ProcessingTime;
        configuration->processing_us = 200;
        configuration->queue_length_limit_enabled = true;
        configuration->queue_length_limit = 100;
        configuration->overflow_policy = models::OverflowPolicy::DropBackgroundNotes;
        long long polls = 0;
        analysis::AnalysisControl control;
        control.cancellation_requested = [&polls]() { ++polls; return false; };
        analysis::WorkloadAnalysis result = analysis::analyze_uncached(song, 100000, configuration, control);
        check(result.predicted_dropped_events > 0, "the dense test song overflows the 100-event queue");
        check(polls >= 2 * pairs / 1024 - 10, "the finite-queue projection polls for cancellation about every 1024 events");
    }
}

} // namespace

int main()
{
    using namespace midibn;

    test_fast_pending_queue();

    // SHA-256 known answers (FIPS 180-4 examples).
    check(sha("") == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855", "sha256 empty");
    check(sha("abc") == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "sha256 abc");
    check(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1", "sha256 two blocks");
    {
        Sha256 h;
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.update(chunk);
        check(h.hex_digest() == "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0", "sha256 million a");
    }

    // System.Decimal behaviour that v1 relies on.
    NetDecimal third = decimal_divide(decimal_multiply(1, 2), 6);
    check(decimal_to_string(third) == "0.3333333333333333333333333333", "1/3 has 28 digits");
    NetDecimal sum = decimal_add(decimal_add(third, third), third);
    check(decimal_to_string(sum) == "0.9999999999999999999999999999", "three thirds stay below 1");
    NetDecimal half = decimal_add(sum, decimal_divide(decimal_multiply(1, 3), 6));
    int64_t rounded = 0;
    check(decimal_round_away_to_int64(half, rounded) && rounded == 1, "1.4999... rounds to 1");
    check(decimal_to_string(decimal_divide(decimal_multiply(10, 1), 3)) == "3.3333333333333333333333333333",
        "10/3 uses 29 significant digits");
    check(decimal_to_string(decimal_divide(decimal_multiply(80, 1), 3)) == "26.666666666666666666666666667",
        "80/3 rounds half-even at scale 27");
    check(decimal_to_string(decimal_divide(decimal_multiply(1, 1), 8)) == "0.125", "1/8 exact");
    NetDecimal exactHalf = decimal_divide(decimal_multiply(3, 1), 2);
    check(decimal_round_away_to_int64(exactHalf, rounded) && rounded == 2, "1.5 rounds away from zero");

    // Exact integer tempo clock (DD-020).
    {
        std::vector<TempoRecord> none;
        TempoClock plain(none, 480);
        int64_t us = 0;
        check(plain.time_at(0, us) && us == 0 && plain.time_at(1, us) && us == 1042 && plain.time_at(480, us) && us == 500000,
            "default tempo: 1 tick of 480 is 1,041.67 us, rounded to 1,042");
        std::vector<TempoRecord> halves = { TempoRecord{ 0, 2, 0, 0 } };
        TempoClock tie(halves, 4);
        check(tie.time_at(1, us) && us == 1 && tie.time_at(3, us) && us == 2, "an exact half microsecond rounds up");
        std::vector<TempoRecord> thirds = { TempoRecord{ 0, 1, 0, 0 }, TempoRecord{ 1, 2, 0, 0 }, TempoRecord{ 2, 3, 0, 0 } };
        TempoClock thirdsClock(thirds, 6);  // 1/6 + 2/6 + 3/6 ... exact sum is 1.0 after 3 ticks: 1/6+2/6+3/6
        check(thirdsClock.time_at(3, us) && us == 1, "sum of sixths is exact (no 0.999... noise)");
        std::vector<TempoRecord> big = { TempoRecord{ 0, 16777215, 0, 0 } };
        TempoClock overflow(big, 1);
        check(!overflow.time_at(INT64_MAX, us), "a time beyond Int64 is reported");
    }

    // DD-056: a clock asked for many ticks in a row (the loader's merge) gives the same microseconds as a clock asked
    // for each tick alone (one long step from the start of the tempo segment), with ascending ticks, repeated ticks,
    // jumps over several tempo changes, huge steps, and a tick that goes back inside the current segment.
    {
        uint64_t state = 0x2545F4914F6CDD1Dull;
        auto next = [&state]() {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return state >> 33;
        };
        int mismatches = 0, queries = 0;
        for (int round = 0; round < 300; ++round) {
            const int ppqn = round % 7 == 0 ? 1 : static_cast<int>(1 + next() % 32767);
            std::vector<TempoRecord> changes;
            int64_t at = static_cast<int64_t>(next() % 50);
            for (int c = 0, count = static_cast<int>(next() % 25); c < count; ++c) {
                changes.push_back(TempoRecord{ at, static_cast<int32_t>(1 + next() % 16777215), 0, c });
                at += static_cast<int64_t>(next() % (round % 3 == 0 ? 3000000 : 5000));
            }
            TempoClock steady(changes, ppqn);
            int64_t tick = 0, floorTick = 0, top = 0;
            for (int q = 0; q < 400; ++q) {
                const uint64_t pick = next() % 20;
                if (pick == 0) tick += static_cast<int64_t>(next() % 2000000000ull) * 997;            // a huge step
                else if (pick < 4) tick += static_cast<int64_t>(next() % 100000);
                else if (pick < 8) { /* the same tick again */ }
                else tick += static_cast<int64_t>(next() % 40);
                int64_t asked = tick;
                // A tick that goes back may lie before an earlier query but never before a tempo change already passed.
                if (pick == 19 && top > floorTick) asked = floorTick + static_cast<int64_t>(next() % static_cast<uint64_t>(top - floorTick + 1));
                int64_t a = 0, b = 0;
                TempoClock alone(changes, ppqn);
                const bool okSteady = steady.time_at(asked, a);
                const bool okAlone = alone.time_at(asked, b);
                ++queries;
                if (okSteady != okAlone || (okSteady && a != b)) ++mismatches;
                if (asked > top) top = asked;
                for (size_t c = 0; c < changes.size(); ++c)
                    if (changes[c].tick <= top && changes[c].tick > floorTick) floorTick = changes[c].tick;
            }
        }
        check(mismatches == 0 && queries == 120000, "a clock asked many ticks in a row equals a clock asked each tick alone");
    }

    // The stored event: short messages and SysEx of any length read back as written.
    {
        EventStore store;
        PackedRecord note = {};
        note.tick = 5;
        note.micros = 7;
        note.packed = 0x90u | (60u << 8) | (100u << 16);
        note.track = 3;
        note.metadata = PackedRecord::make_metadata(EventKind::NoteOn, 0, 3, false, false);
        store.records.append(note);
        PackedRecord shortSysex = note;
        shortSysex.packed = 0xF0u | (0xF7u << 8);
        shortSysex.metadata = PackedRecord::make_metadata(EventKind::SystemExclusive, -1, 2, false, false);
        store.records.append(shortSysex);
        const uint8_t bytes[5] = { 0xF0, 1, 2, 3, 0xF7 };
        store.payloads.append(bytes, 5);
        PackedRecord longSysex = note;
        longSysex.metadata = PackedRecord::make_metadata(EventKind::SystemExclusive, -1, 0, true, true);
        store.records.append_long(longSysex, 5);
        EventRecord a = store.records[0], b = store.records[1], c = store.records[2];
        check(a.status == 0x90 && a.payload_length == 3 && a.channel() == 0 && a.kind() == EventKind::NoteOn && a.track == 3 &&
            a.payload_offset == -1 && store.data_byte(0, 1) == 60, "note record decodes");
        check(b.status == 0xF0 && b.payload_length == 2 && b.packed == (0xF0u | (0xF7u << 8)) && b.channel() == -1,
            "short SysEx keeps its bytes inline");
        check(c.status == 0xF7 && c.payload_length == 5 && c.payload_offset == 0 && c.packed == 0 && store.data_byte(2, 4) == 0xF7,
            "long SysEx finds its payload through the table");
    }
    // Record layout and segment arithmetic.
    check(sizeof(PackedRecord) == 24, "stored event is 24 bytes");
    RecordSegments records;
    for (int i = 0; i < 5000; ++i) {
        PackedRecord r = {};
        r.tick = i;
        records.append(r);
    }
    check(records.count() == 5000 && records[4097].tick == 4097 && records.segment_count() == 3, "record segments");
    PayloadSegments payloads;
    std::string big(1024 * 1024 + 10, 'x');
    big[1024 * 1024] = 'y';
    payloads.append(reinterpret_cast<const uint8_t*>(big.data()), big.size());
    check(payloads.length() == static_cast<int64_t>(big.size()) && payloads.at(1024 * 1024) == 'y', "payload segments");

    // Decimal hardening (Milestone 2): .NET bit patterns, not just values.
    NetDecimal halfOnly = decimal_divide(decimal_multiply(1, 1), 2);
    check(halfOnly.lo == 5 && halfOnly.scale == 1, "1/2 is 5 at scale 1 (no trailing zeros)");
    NetDecimal maxValue;
    maxValue.lo = maxValue.mid = maxValue.hi = 0xFFFFFFFFu;
    NetDecimal one = decimal_multiply(1, 1);
    NetDecimal overflowed;
    check(!decimal_add_checked(maxValue, one, overflowed), "2^96-1 + 1 at scale 0 overflows like .NET");
    NetDecimal tenth = decimal_divide(one, 10);
    NetDecimal scaledMax = maxValue;
    scaledMax.scale = 1;
    check(decimal_add_checked(scaledMax, tenth, overflowed) && overflowed.scale == 0, "near-2^96 sum unscales");

    // Load progress clamps exactly as v1 MidiLoadProgress does.
    LoadProgress clamped("x", 0, 0, 25, 0, -5, 0);
    check(clamped.overall_total == 1 && clamped.overall_completed == 1 && clamped.stage_completed == 0 &&
        clamped.stage_total == 1, "progress clamps");

    // Index series lookups across a 4,096-entry segment boundary.
    IndexSeries series;
    for (int i = 0; i < 5000; ++i) series.append(IndexEntry{ i * 2, i });
    IndexEntry found;
    check(series.latest_before(8193, found) && found.value == 4096, "series lookup across segments");
    check(!series.latest_before(0, found) && series.latest_before(1, found) && found.value == 0, "series lookup bounds");

    // Chase: RPN data entry, increment clamp, Reset All Controllers.
    ChaseIndex chase;
    auto cc = [](int controller, int value) {
        IndexInput in = { 0, 0xB0, 3, static_cast<uint8_t>(controller), static_cast<uint8_t>(value) };
        return in;
    };
    int32_t index = 0;
    for (const IndexInput& in : { cc(101, 0), cc(100, 0), cc(6, 127), cc(38, 127), cc(96, 0), cc(7, 90), cc(121, 0),
             cc(10, 30) })
        chase.add(in, index++);
    chase.complete(CancellationCheck());
    check(chase.parameter_series_count() == 1 && chase.parameters(0)[0].values.count() == 3 &&
        chase.parameters(0)[0].values[2].value == 16383, "data increment clamps at 16383");
    std::array<ChannelReadout, 16> before = chase.attribute_readouts(6);
    std::array<ChannelReadout, 16> after = chase.attribute_readouts(8);
    check(before[0].volume == 90 && after[0].volume == 90 && after[0].pan == 30, "volume survives reset");
    std::vector<ChaseMessage> messages = chase.messages(6, nullptr, nullptr);
    // Volume; parameter 0/0 as 101,100,6,38; then the active selector 101,100.
    check(messages.size() == 7 && messages[0].bytes[1] == 7 && messages[1].bytes[1] == 101 &&
        messages[3].bytes[1] == 6 && messages[4].bytes[2] == 127 && messages[5].bytes[1] == 101,
        "chase emits controller, parameter group and active selector");
    check(chase.messages(7, nullptr, nullptr).size() == 1, "after reset only preserved volume remains");

    // DD-004: misuse of the models no longer leaves half-changed state or throws for a representable answer.
    {
        using namespace midibn::models;
        // A bitrate near Int64.Max: v1 threw OverflowException from (bytes x 10^7 + bitrate - 1) / bitrate.
        check(calculate_bitrate_microseconds(3, kInt64Max) == 1, "bitrate Int64.Max: three bytes take 1 us (rounded up)");
        check(calculate_bitrate_microseconds(0, kInt64Max) == 0, "bitrate Int64.Max: no bytes take no time");
        check(calculate_bitrate_microseconds(1, 31250) == 320 && calculate_bitrate_microseconds(3, 31250) == 960 &&
            calculate_bitrate_microseconds(1, 3) == 3333334 && calculate_bitrate_microseconds(1, 10000000) == 1,
            "ordinary bitrates round up exactly as before");

        // A negative timestamp is recorded as the clamped time 0 that expiry already uses; v1 recorded -5, so a later
        // -7 failed the monotonic check although both mean "time 0".
        ProgressiveRatePacer pacer(1000, 1000000, 64);
        bool threw = false;
        try { pacer.try_consume(-5); pacer.try_consume(-7); pacer.try_consume(0); }
        catch (const ModelException&) { threw = true; }
        check(!threw, "negative pacer timestamps are clamped consistently");

        // A second attack with an index already in the queue is refused and changes nothing.
        PendingMidiQueue queue(8, true);
        PendingMidiQueue::Entry attack;
        attack.event_index = 597;
        attack.paired_attack_index = -1;
        attack.is_note_on = true;
        queue.enqueue(attack);
        PendingMidiQueue::Entry other = attack;
        other.event_index = 600;
        queue.enqueue(other);
        threw = false;
        try { queue.enqueue(attack); }
        catch (const ModelException& e) { threw = std::string(e.type) == "ArgumentException"; }
        check(threw, "a duplicate attack index is still refused with the same exception");
        check(queue.count() == 2 && queue.eligible_count() == 2 && queue.event_indices() == std::vector<int32_t>({ 597, 600 }),
            "the refused duplicate left the queue unchanged");
        queue.enqueue(PendingMidiQueue::Entry{ 601, -1, true, false });
        check(queue.count() == 3 && queue.dequeue() == 597 && queue.dequeue() == 600 && queue.dequeue() == 601,
            "the queue keeps working after the refused duplicate");
    }

    // DD-005: a manual destination outside channels 1-16 leaves the correction selector empty instead of
    // pointing past its last entry (v1 threw while opening the dialog).
    {
        using namespace midibn::routing;
        TrackRoutingSettings settings;
        TrackLaneKey lane(0, 0);
        check(route_correction_index(settings, lane) == 0, "no manual destination: Automatic");
        settings.manual_destinations[lane] = 0;
        check(route_correction_index(settings, lane) == 1, "destination channel 1 is entry 1");
        settings.manual_destinations[lane] = 15;
        check(route_correction_index(settings, lane) == 16, "destination channel 16 is entry 16");
        settings.manual_destinations[lane] = 16;
        check(route_correction_index(settings, lane) == -1, "destination 16 (a 17th channel) leaves the selector empty");
        settings.manual_destinations[lane] = -3;
        check(route_correction_index(settings, lane) == -1, "a negative destination leaves the selector empty");
    }

    // 32-bit large-file warning threshold: half of the real user-mode address space (DD-114 draft).
    {
        const int64_t gib = 1024LL * 1024 * 1024;
        check(half_address_space_warning_threshold(0x7FFEFFFFull) == gib, "32-bit Windows / XP (0x7FFEFFFF): 1 GiB");
        check(half_address_space_warning_threshold(0xBFFEFFFFull) == gib + gib / 2, "/3GB (0xBFFEFFFF): 1.5 GiB");
        check(half_address_space_warning_threshold(0xFFFEFFFFull) == 2 * gib, "32-bit large-address-aware on 64-bit Windows (0xFFFEFFFF): 2 GiB");
        check(half_address_space_warning_threshold(0x7FFFFFFEFFFFull) == 64 * 1024 * gib, "64-bit user space (0x7FFFFFFEFFFF): 64 TiB");
        check(half_address_space_warning_threshold(0) == 0, "unknown address space: built-in value");
        PreflightCounts counts;
        counts.dispatchable_events = 9000000;
        counts.note_ons = 3000000;
        MemoryProjection builtIn = preflight_estimate(counts, 4);
        check(builtIn.warning_threshold == gib, "32-bit built-in threshold is still 1 GiB");
        MemoryProjection wide = preflight_estimate(counts, 4, 2 * gib);
        check(wide.warning_threshold == 2 * gib && wide.retained == builtIn.retained, "a given threshold replaces only the threshold");
        check(preflight_estimate(counts, 8).warning_threshold == 4 * gib, "64-bit threshold unchanged: 4 GiB");
        check(preflight_required(900LL * 1024 * 1024 / 64, 4) == preflight_required(900LL * 1024 * 1024 / 64, 4, 0), "0 means the built-in value");
        check(preflight_required(20LL * 1024 * 1024, 4, 2 * gib) == false && preflight_required(40LL * 1024 * 1024, 4, 2 * gib) == true,
            "the inspection starts at about (threshold - 64 MiB) / 64 of file size");
    }

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
