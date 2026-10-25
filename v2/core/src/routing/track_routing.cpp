// Port of v1 TrackRouting.cs. Statement order, saturating arithmetic, list
// positions used in scores, and every message text follow v1 exactly.
#include "midibn/routing/track_routing.h"

#include <algorithm>
#include <deque>
#include <limits>

namespace midibn {
namespace routing {

using models::kInt32Max;
using models::kInt64Max;
using models::kInt64Min;

namespace {

const uint64_t kUInt64Max = (std::numeric_limits<uint64_t>::max)();

std::string n(int64_t value) { return std::to_string(value); }

std::u16string ascii16(const std::string& text) { return std::u16string(text.begin(), text.end()); }

int compare_patch(const MidiPatchIdentity& left, const MidiPatchIdentity& right)
{
    if (left.bank_msb != right.bank_msb) return left.bank_msb < right.bank_msb ? -1 : 1;
    if (left.bank_lsb != right.bank_lsb) return left.bank_lsb < right.bank_lsb ? -1 : 1;
    return left.program < right.program ? -1 : left.program > right.program ? 1 : 0;
}

bool try_classify_patch_state(const EventView& source, int& stateKind, int& stateValue)
{
    int command = source.status() & 0xF0;
    if (command == 0xC0 && source.data_length >= 2) {
        stateKind = 2;
        stateValue = source.data_byte(1);
        return true;
    }
    if (command == 0xB0 && source.data_length >= 3 && (source.data_byte(1) == 0 || source.data_byte(1) == 32)) {
        stateKind = source.data_byte(1) == 0 ? 0 : 1;
        stateValue = source.data_byte(2);
        return true;
    }
    stateKind = -1;
    stateValue = -1;
    return false;
}

bool is_channel_state(const EventView& source)
{
    int command = source.status() & 0xF0;
    return command != 0x80 && command != 0x90;
}

MidiPatchIdentity normalize_patch(const MidiPatchIdentity& patch)
{
    return MidiPatchIdentity(patch.bank_msb < 0 ? 0 : patch.bank_msb, patch.bank_lsb < 0 ? 0 : patch.bank_lsb,
        patch.program < 0 ? 0 : patch.program);
}

MidiPatchIdentity patch_at(const TrackRouteLaneProfile& profile, int32_t eventIndex)
{
    size_t low = 0;
    size_t high = profile.patch_timeline.size();
    while (low < high) {
        size_t middle = low + ((high - low) >> 1);
        if (profile.patch_timeline[middle].event_index <= eventIndex) low = middle + 1;
        else high = middle;
    }
    return low == 0 ? MidiPatchIdentity(-1, -1, -1) : profile.patch_timeline[low - 1].patch;
}

bool is_sounding_at(const TrackRouteLaneProfile& profile, int64_t microseconds)
{
    int32_t low = 0;
    int32_t high = static_cast<int32_t>(profile.sounding_intervals.size());
    while (low < high) {
        int32_t middle = low + ((high - low) >> 1);
        if (profile.sounding_intervals[static_cast<size_t>(middle)].start_us <= microseconds) low = middle + 1;
        else high = middle;
    }
    for (int32_t index = low - 1; index >= 0; --index) {
        const TrackRouteSoundInterval& interval = profile.sounding_intervals[static_cast<size_t>(index)];
        if (interval.prefix_maximum_end_us <= microseconds) return false;
        if (microseconds < interval.end_us) return true;
    }
    return false;
}

const TrackRouteLaneProfile* prefer_canonical_profile(const TrackRouteLaneProfile* left,
    const TrackRouteLaneProfile* right, const TrackLaneKey& representative)
{
    if (left == nullptr) return right;
    if (right == nullptr) return left;
    if (right->lane == representative) return right;
    if (left->lane == representative) return left;
    if (right->dominance_score > left->dominance_score) return right;
    if (right->dominance_score < left->dominance_score) return left;
    return right->lane.compare(left->lane) < 0 ? right : left;
}

const TrackRouteLaneProfile* best_profile_for_source_channel(const TrackRouteDestinationPreview& destination,
    int sourceChannel)
{
    const TrackRouteLaneProfile* result = nullptr;
    for (const auto& assignment : destination.assignments) {
        const TrackRouteLaneProfile* profile = assignment->profile.get();
        if (profile->lane.source_channel != sourceChannel) continue;
        result = prefer_canonical_profile(result, profile, destination.representative_lane);
    }
    return result;
}

const TrackRouteLaneProfile* canonical_profile_at_time(const TrackRouteDestinationPreview& destination,
    int64_t microseconds, const TrackRouteLaneProfile* fallback)
{
    const TrackRouteLaneProfile* active = nullptr;
    int activeSourceChannel = -1;
    bool multipleSourceChannels = false;
    const TrackRouteLaneProfile* representative = nullptr;
    for (const auto& assignment : destination.assignments) {
        const TrackRouteLaneProfile* profile = assignment->profile.get();
        if (profile->lane == destination.representative_lane) representative = profile;
        if (!is_sounding_at(*profile, microseconds)) continue;
        active = prefer_canonical_profile(active, profile, destination.representative_lane);
        if (activeSourceChannel < 0) activeSourceChannel = profile->lane.source_channel;
        else if (activeSourceChannel != profile->lane.source_channel) multipleSourceChannels = true;
    }
    if (multipleSourceChannels && representative != nullptr) return representative;
    return active != nullptr ? active : fallback;
}

EventView make_generated_patch_event(const EventView& source, int destinationChannel, EventKind kind, int controller,
    int value)
{
    uint8_t status = static_cast<uint8_t>((kind == EventKind::ProgramChange ? 0xC0 : 0xB0) | destinationChannel);
    return kind == EventKind::ProgramChange
        ? EventView::create(source.tick, source.micros, source.track, kind, destinationChannel, status, -1,
            static_cast<uint8_t>(value), 0, 2)
        : EventView::create(source.tick, source.micros, source.track, kind, destinationChannel, status, -1,
            static_cast<uint8_t>(controller), static_cast<uint8_t>(value), 3);
}

// ------------------------------------------------------------ planner

struct ActiveAttack {
    int64_t time = 0;
    int32_t velocity = 0;
    MidiPatchIdentity patch;
    int32_t event_index = 0;
    int32_t key = 0;
};

struct MutableLane {
    std::shared_ptr<TrackRouteLaneProfile> profile = std::make_shared<TrackRouteLaneProfile>();
    std::map<MidiPatchIdentity, uint64_t> patch_scores;
    bool sustain_on = false;
    std::vector<ActiveAttack> sustained;
};

struct LanePair {
    TrackLaneKey first;
    TrackLaneKey second;
    LanePair(const TrackLaneKey& a, const TrackLaneKey& b)
    {
        if (a.compare(b) <= 0) { first = a; second = b; }
        else { first = b; second = a; }
    }
    bool operator<(const LanePair& o) const
    {
        int c = first.compare(o.first);
        return c != 0 ? c < 0 : second.compare(o.second) < 0;
    }
};

using Overlaps = std::set<LanePair>;
using Profile = TrackRouteLaneProfile;
using ProfilePtr = std::shared_ptr<TrackRouteLaneProfile>;

struct DestinationGroup {
    int32_t destination = 0;
    std::vector<ProfilePtr> lanes;
};
using GroupPtr = std::shared_ptr<DestinationGroup>;

void add_dominance(MutableLane& lane, const ActiveAttack& attack, int64_t releaseTime)
{
    uint64_t duration = static_cast<uint64_t>(std::max<int64_t>(1, models::wrap_sub(releaseTime, attack.time)));
    uint64_t velocity = static_cast<uint32_t>(std::max(1, attack.velocity));
    uint64_t amount = duration > kUInt64Max / velocity ? kUInt64Max : duration * velocity;
    uint64_t prior = 0;
    auto found = lane.patch_scores.find(attack.patch);
    if (found != lane.patch_scores.end()) prior = found->second;
    lane.patch_scores[attack.patch] = kUInt64Max - prior < amount ? kUInt64Max : prior + amount;
}

void finalize_attack(MutableLane& lane, const ActiveAttack& attack, int64_t endMicroseconds, int32_t endEventIndex)
{
    add_dominance(lane, attack, endMicroseconds);
    lane.profile->last_release_us = std::max(lane.profile->last_release_us, endMicroseconds);
    TrackRouteSoundInterval interval;
    interval.start_event_index = attack.event_index;
    interval.end_event_index = endEventIndex;
    interval.start_us = attack.time;
    interval.end_us = endMicroseconds;
    interval.key = attack.key;
    lane.profile->sounding_intervals.push_back(interval);
}

using ActiveByKey = std::map<int32_t, std::map<TrackLaneKey, int32_t>>;

void decrement_owner(ActiveByKey& activeByKey, int32_t note, const TrackLaneKey& lane)
{
    auto owners = activeByKey.find(note);
    if (owners == activeByKey.end()) return;
    auto owner = owners->second.find(lane);
    if (owner == owners->second.end()) return;
    if (owner->second <= 1) owners->second.erase(owner);
    else owner->second--;
    if (owners->second.empty()) activeByKey.erase(owners);
}

void release_sustained(MutableLane& lane, int64_t endMicroseconds, int32_t endEventIndex, int32_t& activeAttackCount,
    ActiveByKey& activeByKey)
{
    for (const ActiveAttack& attack : lane.sustained) {
        finalize_attack(lane, attack, endMicroseconds, endEventIndex);
        activeAttackCount--;
        decrement_owner(activeByKey, attack.key, lane.profile->lane);
    }
    lane.sustained.clear();
}

using ActiveMap = std::map<std::pair<TrackLaneKey, int32_t>, std::deque<ActiveAttack>>;

void release_all_lane_attacks(const TrackLaneKey& laneKey, MutableLane& lane, int64_t endMicroseconds,
    int32_t endEventIndex, ActiveMap& active, int32_t& activeAttackCount, ActiveByKey& activeByKey)
{
    for (auto& pair : active) {
        if (pair.first.first != laneKey) continue;
        while (!pair.second.empty()) {
            ActiveAttack attack = pair.second.front();
            pair.second.pop_front();
            finalize_attack(lane, attack, endMicroseconds, endEventIndex);
            activeAttackCount--;
            decrement_owner(activeByKey, attack.key, laneKey);
        }
    }
    release_sustained(lane, endMicroseconds, endEventIndex, activeAttackCount, activeByKey);
}

void mix_state(Profile& profile, const EventView& midiEvent)
{
    uint64_t hash = profile.dynamic_state_signature == 0 ? 1469598103934665603ULL : profile.dynamic_state_signature;
    hash = (hash ^ static_cast<uint64_t>(midiEvent.micros)) * 1099511628211ULL;
    for (int index = 0; index < midiEvent.data_length; ++index)
        hash = (hash ^ midiEvent.data_byte(index)) * 1099511628211ULL;
    profile.dynamic_state_signature = hash;
}

void record_patch_change_for_channel(std::map<TrackLaneKey, MutableLane>& lanes, int sourceChannel,
    int32_t eventIndex, int bankMsb, int bankLsb, int program)
{
    MidiPatchIdentity patch(bankMsb, bankLsb, program);
    for (auto& pair : lanes) {
        if (pair.first.source_channel != sourceChannel) continue;
        std::vector<TrackRoutePatchChange>& timeline = pair.second.profile->patch_timeline;
        if (!timeline.empty() && timeline.back().patch == patch) continue;
        TrackRoutePatchChange change;
        change.event_index = eventIndex;
        change.patch = patch;
        timeline.push_back(change);
    }
}

std::vector<ProfilePtr> build_profiles(const RoutingSong& song, Overlaps& sameKeyOverlaps)
{
    std::map<TrackLaneKey, MutableLane> lanes;
    ActiveMap active;
    ActiveByKey activeByKey;
    int32_t activeAttackCount = 0;
    sameKeyOverlaps.clear();
    std::vector<bool> trackSystem(static_cast<size_t>(std::max(0, song.track_count)), false);
    const EventSource& events = song.events;
    const int32_t count = events.count();
    for (int32_t eventIndex = 0; eventIndex < count; ++eventIndex) {
        EventView source = events.at(eventIndex);
        if (source.channel() >= 0 && (source.status() & 0xF0) == 0x90 && source.data_length >= 3 &&
            source.data_byte(2) != 0) {
            TrackLaneKey key(source.track, source.channel());
            if (lanes.find(key) == lanes.end()) {
                MutableLane created;
                created.profile->lane = key;
                if (song.tracks != nullptr && key.track >= 0 && key.track < static_cast<int32_t>(song.tracks->size())) {
                    created.profile->track_name = (*song.tracks)[static_cast<size_t>(key.track)].name;
                    created.profile->instrument_name = (*song.tracks)[static_cast<size_t>(key.track)].instrument;
                }
                lanes.emplace(key, created);
            }
        }
    }
    int channelBankMsb[16];
    int channelBankLsb[16];
    int channelProgram[16];
    for (int channel = 0; channel < 16; ++channel) channelBankMsb[channel] = channelBankLsb[channel] = channelProgram[channel] = -1;
    for (int32_t eventIndex = 0; eventIndex < count; ++eventIndex) {
        EventView midiEvent = events.at(eventIndex);
        const int channelOfEvent = midiEvent.channel();
        if (channelOfEvent < 0) {
            if (midiEvent.track >= 0 && midiEvent.track < static_cast<int32_t>(trackSystem.size()))
                trackSystem[static_cast<size_t>(midiEvent.track)] = true;
            continue;
        }
        TrackLaneKey key(midiEvent.track, channelOfEvent);
        auto laneIt = lanes.find(key);
        MutableLane* lane = laneIt == lanes.end() ? nullptr : &laneIt->second;
        if (lane != nullptr) lane->profile->event_count++;
        int command = midiEvent.status() & 0xF0;
        if (command == 0xB0 && midiEvent.data_length >= 3) {
            int controller = midiEvent.data_byte(1);
            int value = midiEvent.data_byte(2);
            if (controller == 0) {
                channelBankMsb[channelOfEvent] = value;
                record_patch_change_for_channel(lanes, channelOfEvent, eventIndex, channelBankMsb[channelOfEvent],
                    channelBankLsb[channelOfEvent], channelProgram[channelOfEvent]);
            } else if (controller == 32) {
                channelBankLsb[channelOfEvent] = value;
                record_patch_change_for_channel(lanes, channelOfEvent, eventIndex, channelBankMsb[channelOfEvent],
                    channelBankLsb[channelOfEvent], channelProgram[channelOfEvent]);
            } else {
                for (auto& pair : lanes) {
                    if (pair.first.source_channel != channelOfEvent) continue;
                    MutableLane& target = pair.second;
                    target.profile->has_dynamic_state = true;
                    if (controller == 64) target.profile->has_sustain = true;
                    mix_state(*target.profile, midiEvent);
                    if (controller == 64) {
                        bool sustainOn = value >= 64;
                        if (target.sustain_on && !sustainOn)
                            release_sustained(target, midiEvent.micros, eventIndex, activeAttackCount, activeByKey);
                        target.sustain_on = sustainOn;
                    } else if (controller == 121) {
                        if (target.sustain_on)
                            release_sustained(target, midiEvent.micros, eventIndex, activeAttackCount, activeByKey);
                        target.sustain_on = false;
                    } else if (controller == 120 || controller == 123) {
                        release_all_lane_attacks(pair.first, target, midiEvent.micros, eventIndex, active,
                            activeAttackCount, activeByKey);
                        target.sustain_on = false;
                    }
                }
            }
        } else if (command == 0xC0 && midiEvent.data_length >= 2) {
            channelProgram[channelOfEvent] = midiEvent.data_byte(1);
            record_patch_change_for_channel(lanes, channelOfEvent, eventIndex, channelBankMsb[channelOfEvent],
                channelBankLsb[channelOfEvent], channelProgram[channelOfEvent]);
        } else if (command == 0xD0 || command == 0xE0) {
            for (auto& pair : lanes)
                if (pair.first.source_channel == channelOfEvent) {
                    pair.second.profile->has_dynamic_state = true;
                    mix_state(*pair.second.profile, midiEvent);
                }
        }

        bool attack = command == 0x90 && midiEvent.data_length >= 3 && midiEvent.data_byte(2) != 0;
        bool release = (command == 0x80 && midiEvent.data_length >= 3) ||
            (command == 0x90 && midiEvent.data_length >= 3 && midiEvent.data_byte(2) == 0);
        if (attack) {
            if (lane == nullptr) continue;
            int note = midiEvent.data_byte(1);
            MidiPatchIdentity patch(channelBankMsb[channelOfEvent], channelBankLsb[channelOfEvent],
                channelProgram[channelOfEvent]);
            if (lane->profile->patch_history.count(patch) == 0 &&
                static_cast<int32_t>(lane->profile->patch_history.size()) >= kMaximumDistinctPatchesPerLane)
                models::throw_invalid_operation(
                    ("Track-routing analysis exceeded the bounded distinct-patch limit for " + key.to_string() + ".").c_str());
            lane->profile->patch_history.insert(patch);
            lane->profile->attack_count++;
            if (lane->profile->first_attack_us < 0) lane->profile->first_attack_us = midiEvent.micros;
            std::deque<ActiveAttack>& queue = active[std::make_pair(key, note)];
            if (activeAttackCount >= kMaximumActiveAttacks)
                models::throw_invalid_operation("Track-routing analysis exceeded the bounded active-note occurrence limit.");
            ActiveAttack started;
            started.time = midiEvent.micros;
            started.velocity = midiEvent.data_byte(2);
            started.patch = patch;
            started.event_index = eventIndex;
            started.key = note;
            queue.push_back(started);
            activeAttackCount++;
            std::map<TrackLaneKey, int32_t>& owners = activeByKey[note];
            for (const auto& owner : owners)
                if (owner.second > 0 && owner.first != key) sameKeyOverlaps.insert(LanePair(key, owner.first));
            owners[key] = owners[key] + 1;
        } else if (release) {
            if (lane == nullptr) continue;
            int note = midiEvent.data_byte(1);
            auto queue = active.find(std::make_pair(key, note));
            if (queue != active.end() && !queue->second.empty()) {
                ActiveAttack started = queue->second.front();
                queue->second.pop_front();
                if (lane->sustain_on) lane->sustained.push_back(started);
                else {
                    finalize_attack(*lane, started, midiEvent.micros, eventIndex);
                    activeAttackCount--;
                    decrement_owner(activeByKey, note, key);
                }
            }
        }
    }
    for (auto& pending : active) {
        MutableLane& lane = lanes.at(pending.first.first);
        while (!pending.second.empty()) {
            ActiveAttack attack = pending.second.front();
            pending.second.pop_front();
            finalize_attack(lane, attack, song.duration_us, count);
        }
    }
    for (auto& pair : lanes) {
        MutableLane& lane = pair.second;
        for (const ActiveAttack& attack : lane.sustained) finalize_attack(lane, attack, song.duration_us, count);
        lane.sustained.clear();
    }
    std::vector<ProfilePtr> result;
    result.reserve(lanes.size());
    for (auto& pair : lanes) {
        MutableLane& lane = pair.second;
        std::vector<TrackRouteSoundInterval>& intervals = lane.profile->sounding_intervals;
        std::sort(intervals.begin(), intervals.end(), [](const TrackRouteSoundInterval& left, const TrackRouteSoundInterval& right) {
            if (left.start_event_index != right.start_event_index) return left.start_event_index < right.start_event_index;
            if (left.end_event_index != right.end_event_index) return left.end_event_index < right.end_event_index;
            return left.key < right.key;
        });
        lane.profile->sounding_end_events.clear();
        lane.profile->sounding_end_events.reserve(intervals.size());
        for (const TrackRouteSoundInterval& interval : intervals) lane.profile->sounding_end_events.push_back(interval.end_event_index);
        std::sort(lane.profile->sounding_end_events.begin(), lane.profile->sounding_end_events.end());
        int64_t prefixMaximumEnd = kInt64Min;
        for (TrackRouteSoundInterval& interval : intervals) {
            prefixMaximumEnd = std::max(prefixMaximumEnd, interval.end_us);
            interval.prefix_maximum_end_us = prefixMaximumEnd;
        }
        const TrackLaneKey& laneKey = lane.profile->lane;
        lane.profile->track_contains_system_traffic = laneKey.track >= 0 &&
            laneKey.track < static_cast<int32_t>(trackSystem.size()) && trackSystem[static_cast<size_t>(laneKey.track)];
        MidiPatchIdentity representative(channelBankMsb[laneKey.source_channel], channelBankLsb[laneKey.source_channel],
            channelProgram[laneKey.source_channel]);
        uint64_t best = 0;
        for (const auto& score : lane.patch_scores)
            if (score.second > best || (score.second == best && compare_patch(score.first, representative) < 0)) {
                best = score.second;
                representative = score.first;
            }
        lane.profile->representative_patch = representative;
        lane.profile->dominance_score = best;
        if (lane.profile->patch_history.empty()) lane.profile->patch_history.insert(representative);
        result.push_back(lane.profile);
    }
    return result;
}

bool time_ranges_overlap(const Profile& left, const Profile& right)
{
    if (left.first_attack_us < 0 || right.first_attack_us < 0) return false;
    return left.first_attack_us <= right.last_release_us && right.first_attack_us <= left.last_release_us;
}

bool state_conflict(const Profile& left, const Profile& right)
{
    if (!time_ranges_overlap(left, right)) return false;
    if (!left.has_dynamic_state && !right.has_dynamic_state) return false;
    return left.dynamic_state_signature != right.dynamic_state_signature;
}

void apply_patch_changes(const std::vector<TrackRoutePatchChange>& changes, int32_t eventIndex, size_t& cursor,
    MidiPatchIdentity& laneState, MidiPatchIdentity& sharedState)
{
    while (cursor < changes.size() && changes[cursor].event_index == eventIndex) {
        laneState = changes[cursor].patch;
        sharedState = laneState;
        cursor++;
    }
}

// The event indexes at which a lane's sounding intervals end, ascending. The profile carries them when the
// planner built it; a profile made by other code gets them computed here.
const std::vector<int32_t>& sorted_end_events(const Profile& profile, std::vector<int32_t>& scratch)
{
    if (profile.sounding_end_events.size() == profile.sounding_intervals.size()) return profile.sounding_end_events;
    scratch.clear();
    for (const TrackRouteSoundInterval& interval : profile.sounding_intervals) scratch.push_back(interval.end_event_index);
    std::sort(scratch.begin(), scratch.end());
    return scratch;
}

// One lane's three sorted event-index sequences (patch changes, interval starts, interval ends), walked together.
struct LaneBoundaries {
    const std::vector<TrackRoutePatchChange>& changes;
    const std::vector<TrackRouteSoundInterval>& intervals;  // ascending start_event_index
    const std::vector<int32_t>& ends;
    size_t change = 0, start = 0, end = 0;

    int32_t next() const
    {
        int32_t result = (std::numeric_limits<int32_t>::max)();
        if (change < changes.size()) result = std::min(result, changes[change].event_index);
        if (start < intervals.size()) result = std::min(result, intervals[start].start_event_index);
        if (end < ends.size()) result = std::min(result, ends[end]);
        return result;
    }
    bool exhausted() const { return change >= changes.size() && start >= intervals.size() && end >= ends.size(); }
};

bool patch_states_remain_compatible_while_sounding(const Profile& left, const Profile& right)
{
    // Walks every event index at which either lane changes patch or starts or stops sounding, in ascending
    // order and once each, as the sorted list of boundaries did; no per-interval container is built.
    std::vector<int32_t> leftScratch, rightScratch;
    LaneBoundaries l{ left.patch_timeline, left.sounding_intervals, sorted_end_events(left, leftScratch) };
    LaneBoundaries r{ right.patch_timeline, right.sounding_intervals, sorted_end_events(right, rightScratch) };
    int32_t leftSound = 0, rightSound = 0;
    MidiPatchIdentity leftState(-1, -1, -1);
    MidiPatchIdentity rightState(-1, -1, -1);
    MidiPatchIdentity sharedState(-1, -1, -1);
    while (!l.exhausted() || !r.exhausted()) {
        const int32_t eventIndex = std::min(l.next(), r.next());
        while (l.change < l.changes.size() && l.changes[l.change].event_index == eventIndex) {
            leftState = l.changes[l.change].patch;
            sharedState = leftState;
            l.change++;
        }
        while (r.change < r.changes.size() && r.changes[r.change].event_index == eventIndex) {
            rightState = r.changes[r.change].patch;
            sharedState = rightState;
            r.change++;
        }
        while (l.start < l.intervals.size() && l.intervals[l.start].start_event_index == eventIndex) { leftSound++; l.start++; }
        while (l.end < l.ends.size() && l.ends[l.end] == eventIndex) { leftSound--; l.end++; }
        while (r.start < r.intervals.size() && r.intervals[r.start].start_event_index == eventIndex) { rightSound++; r.start++; }
        while (r.end < r.ends.size() && r.ends[r.end] == eventIndex) { rightSound--; r.end++; }
        if (leftSound > 0 && sharedState != leftState) return false;
        if (rightSound > 0 && sharedState != rightState) return false;
    }
    return true;
}
bool exact_compatible(const Profile& left, const Profile& right, const Overlaps& overlaps)
{
    return patch_states_remain_compatible_while_sounding(left, right) && !state_conflict(left, right) &&
        overlaps.count(LanePair(left.lane, right.lane)) == 0;
}

void add_assignment(std::vector<std::shared_ptr<TrackRouteAssignment>>& assignments, DestinationGroup& group,
    const ProfilePtr& lane, TrackRoutingTier tier, bool manual, const Overlaps& overlaps)
{
    auto assignment = std::make_shared<TrackRouteAssignment>();
    assignment->profile = lane;
    assignment->destination_channel = group.destination;
    assignment->tier = tier;
    assignment->manual = manual;
    for (const ProfilePtr& other : group.lanes) {
        if (state_conflict(*lane, *other)) assignment->state_conflict = true;
        if (overlaps.count(LanePair(lane->lane, other->lane)) != 0) assignment->same_key_overlap = true;
        if (!time_ranges_overlap(*lane, *other) && lane->representative_patch != other->representative_patch)
            assignment->time_handoff = true;
        if (lane->representative_patch != other->representative_patch) assignment->retimbred = true;
    }
    assignment->percussion_exception = (lane->lane.source_channel == 9) != (group.destination == 9);
    if (assignment->state_conflict) assignment->warnings.push_back("Conflicting channel-wide state");
    if (assignment->same_key_overlap) assignment->warnings.push_back("Receiver-dependent same-key overlap");
    if (assignment->time_handoff) assignment->warnings.push_back("Audible patch/state handoff");
    if (assignment->retimbred) assignment->warnings.push_back("Representative patch retimbres this lane");
    if (tier == TrackRoutingTier::GmFamily &&
        (!lane->representative_patch.is_general_midi_bank() ||
            !group.lanes.empty() && !group.lanes[0]->representative_patch.is_general_midi_bank()))
        assignment->warnings.push_back("GM-family inference uses an unknown or nonzero bank");
    if (assignment->percussion_exception) assignment->warnings.push_back("Percussion convention override");
    group.lanes.push_back(lane);
    assignments.push_back(assignment);
}

const Profile* dominant_lane(const DestinationGroup* group)
{
    if (group == nullptr || group->lanes.empty()) return nullptr;
    const Profile* representative = group->lanes[0].get();
    for (size_t laneIndex = 1; laneIndex < group->lanes.size(); ++laneIndex) {
        const Profile* lane = group->lanes[laneIndex].get();
        if (lane->dominance_score > representative->dominance_score ||
            (lane->dominance_score == representative->dominance_score && lane->lane.compare(representative->lane) < 0))
            representative = lane;
    }
    return representative;
}

DestinationGroup* find_or_create_group(std::vector<GroupPtr>& groups, int32_t destination, std::set<int32_t>& used)
{
    for (const GroupPtr& group : groups)
        if (group->destination == destination) return group.get();
    auto result = std::make_shared<DestinationGroup>();
    result->destination = destination;
    groups.push_back(result);
    used.insert(destination);
    return result.get();
}

bool list_contains(const std::vector<int32_t>& list, int32_t value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

int32_t choose_unused_destination(const std::vector<int32_t>& available, const std::set<int32_t>& used, int32_t preferred)
{
    if (list_contains(available, preferred) && used.count(preferred) == 0) return preferred;
    for (int32_t destination : available)
        if (used.count(destination) == 0) return destination;
    return -1;
}

DestinationGroup* find_exact_group(std::vector<GroupPtr>& groups, const Profile& lane, const Overlaps& overlaps,
    const TrackRoutingSettings& settings)
{
    DestinationGroup* best = nullptr;
    for (const GroupPtr& groupPtr : groups) {
        DestinationGroup* group = groupPtr.get();
        if (settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 && group->destination == 9 &&
            lane.lane.source_channel != 9)
            continue;
        bool compatible = true;
        for (const ProfilePtr& other : group->lanes)
            if (!exact_compatible(lane, *other, overlaps)) {
                compatible = false;
                break;
            }
        if (!compatible) continue;
        if (group->destination == lane.lane.source_channel) return group;
        if (best == nullptr || group->destination < best->destination) best = group;
    }
    return best;
}

int32_t tier_cost(const DestinationGroup& group, const Profile& lane, const Overlaps& overlaps, TrackRoutingTier tier)
{
    int32_t cost = 0;
    for (const ProfilePtr& otherPtr : group.lanes) {
        const Profile& other = *otherPtr;
        if (tier == TrackRoutingTier::GmFamily &&
            (lane.representative_patch.gm_family() < 0 ||
                lane.representative_patch.gm_family() != other.representative_patch.gm_family()))
            return kInt32Max;
        if (lane.representative_patch != other.representative_patch) cost += 4;
        if (state_conflict(lane, other)) cost += 3;
        if (overlaps.count(LanePair(lane.lane, other.lane)) != 0) cost += 8;
        if (!time_ranges_overlap(lane, other)) cost += 1;
    }
    return cost;
}

void resolve_by_tier(std::vector<ProfilePtr>& unresolved, std::vector<std::shared_ptr<TrackRouteAssignment>>& assignments,
    std::vector<GroupPtr>& groups, const Overlaps& overlaps, const TrackRoutingSettings& settings, TrackRoutingTier tier)
{
    for (int32_t index = static_cast<int32_t>(unresolved.size()) - 1; index >= 0; --index) {
        ProfilePtr lane = unresolved[static_cast<size_t>(index)];
        DestinationGroup* best = nullptr;
        int32_t bestCost = kInt32Max;
        for (const GroupPtr& groupPtr : groups) {
            DestinationGroup* group = groupPtr.get();
            if (settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 && group->destination == 9)
                continue;
            int32_t cost = tier_cost(*group, *lane, overlaps, tier);
            if (cost < bestCost || (cost == bestCost && best != nullptr && group->destination < best->destination)) {
                best = group;
                bestCost = cost;
            }
        }
        if (best == nullptr || bestCost == kInt32Max) continue;
        add_assignment(assignments, *best, lane, tier, false, overlaps);
        unresolved.erase(unresolved.begin() + index);
    }
}

int64_t aggressive_representative_score(const Profile& lane, int32_t dominanceIndex, int32_t candidateCount,
    const std::vector<ProfilePtr>& selected, const Overlaps& overlaps)
{
    int64_t score = static_cast<int64_t>(candidateCount - dominanceIndex) * 100;
    for (const ProfilePtr& otherPtr : selected) {
        const Profile& other = *otherPtr;
        if (overlaps.count(LanePair(lane.lane, other.lane)) != 0) score += 120;
        if (state_conflict(lane, other)) score += 80;
        if (lane.representative_patch != other.representative_patch) score += 60;
        if (lane.representative_patch.gm_family() != other.representative_patch.gm_family()) score += 30;
        if (time_ranges_overlap(lane, other)) score += 20;
    }
    return score;
}

void resolve_aggressive_globally(std::vector<ProfilePtr>& unresolved,
    std::vector<std::shared_ptr<TrackRouteAssignment>>& assignments, std::vector<GroupPtr>& groups,
    const std::vector<int32_t>& available, std::set<int32_t>& used, const Overlaps& overlaps,
    const TrackRoutingSettings& settings)
{
    std::vector<ProfilePtr> representatives;
    for (const GroupPtr& group : groups)
        representatives.insert(representatives.end(), group->lanes.begin(), group->lanes.end());
    while (!unresolved.empty()) {
        bool hasFreeDestination = false;
        for (int32_t destination : available)
            if (used.count(destination) == 0) {
                hasFreeDestination = true;
                break;
            }
        if (!hasFreeDestination) break;
        int32_t bestIndex = 0;
        int64_t bestScore = kInt64Min;
        const int32_t candidates = static_cast<int32_t>(unresolved.size());
        for (int32_t index = 0; index < candidates; ++index) {
            int64_t score = aggressive_representative_score(*unresolved[static_cast<size_t>(index)], index, candidates,
                representatives, overlaps);
            if (score > bestScore || score == bestScore &&
                unresolved[static_cast<size_t>(index)]->lane.compare(unresolved[static_cast<size_t>(bestIndex)]->lane) < 0) {
                bestIndex = index;
                bestScore = score;
            }
        }
        ProfilePtr selected = unresolved[static_cast<size_t>(bestIndex)];
        int32_t freeDestination = choose_unused_destination(available, used, selected->lane.source_channel);
        DestinationGroup* group = find_or_create_group(groups, freeDestination, used);
        add_assignment(assignments, *group, selected, TrackRoutingTier::Aggressive, false, overlaps);
        representatives.push_back(selected);
        unresolved.erase(unresolved.begin() + bestIndex);
    }

    for (const ProfilePtr& lanePtr : unresolved) {
        const Profile& lane = *lanePtr;
        DestinationGroup* best = nullptr;
        int32_t bestCost = kInt32Max;
        uint64_t weakestRepresentative = kUInt64Max;
        for (const GroupPtr& groupPtr : groups) {
            DestinationGroup* group = groupPtr.get();
            if (settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 && group->destination == 9)
                continue;
            int32_t cost = tier_cost(*group, lane, overlaps, TrackRoutingTier::Aggressive);
            if (cost < bestCost || cost == bestCost && best != nullptr && group->destination < best->destination) {
                best = group;
                bestCost = cost;
            }
            const Profile* representative = dominant_lane(group);
            if (representative != nullptr && representative->dominance_score < weakestRepresentative)
                weakestRepresentative = representative->dominance_score;
        }
        bool destructiveMerge = bestCost >= 15 && weakestRepresentative != kUInt64Max &&
            lane.dominance_score < weakestRepresentative;
        if (destructiveMerge) {
            auto omitted = std::make_shared<TrackRouteAssignment>();
            omitted->profile = lanePtr;
            omitted->destination_channel = -1;
            omitted->tier = TrackRoutingTier::Aggressive;
            omitted->omitted = true;
            omitted->retimbred = true;
            omitted->warnings.push_back("Omitted " + n(lane.attack_count) +
                (lane.attack_count == 1 ? " note start" : " note starts") +
                " because every available merge has destructive overlap/state conflicts");
            assignments.push_back(omitted);
        } else if (best != nullptr)
            add_assignment(assignments, *best, lanePtr, TrackRoutingTier::Aggressive, false, overlaps);
    }
    unresolved.clear();
}

std::vector<int32_t> available_destinations(const TrackRoutingSettings& settings, const std::vector<ProfilePtr>& profiles,
    std::vector<std::string>& errors)
{
    std::vector<int32_t> result;
    int32_t target = std::max(1, std::min(16, settings.target_capacity));
    std::set<int32_t> manual;
    for (const ProfilePtr& profile : profiles) {
        auto found = settings.manual_destinations.find(profile->lane);
        if (found == settings.manual_destinations.end()) continue;
        int32_t destination = found->second;
        if (destination < 0 || destination > 15 || !settings.usable_destination_channels[static_cast<size_t>(destination)])
            continue;
        bool reservedPercussion = settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 &&
            destination == 9 && profile->lane.source_channel == 9;
        if (!reservedPercussion) manual.insert(destination);
    }
    result.assign(manual.begin(), manual.end());  // already sorted
    if (static_cast<int32_t>(result.size()) > target)
        errors.push_back("Manual corrections use more melodic destinations than the selected capacity.");
    for (int32_t channel = 0; channel < 16 && static_cast<int32_t>(result.size()) < target; ++channel)
        if (settings.usable_destination_channels[static_cast<size_t>(channel)] &&
            !(settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 && channel == 9) &&
            !list_contains(result, channel))
            result.push_back(channel);
    return result;
}

std::vector<TrackRouteDestinationPreview> build_preview(std::vector<GroupPtr>& groups,
    std::vector<std::shared_ptr<TrackRouteAssignment>>& assignments)
{
    std::vector<TrackRouteDestinationPreview> result;
    std::sort(groups.begin(), groups.end(),
        [](const GroupPtr& left, const GroupPtr& right) { return left->destination < right->destination; });
    for (const GroupPtr& group : groups) {
        const Profile* representative = dominant_lane(group.get());
        TrackRouteDestinationPreview preview;
        preview.destination_channel = group->destination;
        preview.representative_lane = representative->lane;
        preview.representative_patch = representative->representative_patch;
        for (const auto& assignment : assignments)
            if (assignment->destination_channel == group->destination) {
                assignment->representative_patch = assignment->profile->lane == representative->lane;
                preview.assignments.push_back(assignment);
            }
        result.push_back(preview);
    }
    return result;
}

} // namespace

// ------------------------------------------------------------ value types

std::string TrackLaneKey::to_string() const { return "track " + n(static_cast<int64_t>(track) + 1) + ", ch " + n(static_cast<int64_t>(source_channel) + 1); }

bool MidiPatchIdentity::operator<(const MidiPatchIdentity& o) const { return compare_patch(*this, o) < 0; }

std::string MidiPatchIdentity::to_string() const
{
    return "bank " + (bank_msb < 0 ? std::string("?") : n(bank_msb)) + "/" + (bank_lsb < 0 ? std::string("?") : n(bank_lsb)) +
        ", program " + (program < 0 ? std::string("?") : n(static_cast<int64_t>(program) + 1));
}

std::u16string TrackRouteLaneProfile::display_name() const
{
    const std::u16string& name = !track_name.empty() ? track_name : instrument_name;
    return name.empty() ? ascii16("Track " + n(static_cast<int64_t>(lane.track) + 1)) : name;
}

// ------------------------------------------------------------ compiler

CompiledTrackRoute::CompiledTrackRoute(const RoutingSong& source, std::vector<ProfilePtr> lanesIn,
    std::vector<std::shared_ptr<TrackRouteAssignment>> assignmentsIn,
    std::vector<TrackRouteDestinationPreview> destinationsIn, std::vector<std::string> errorsIn,
    bool requiresExplicitApply, TrackRoutingTier highestTier)
    : source_identity_(source.identity), source_events_(source.events)
{
    const EventSource& sourceStore = source.events;
    destinations_by_slot_.assign(static_cast<size_t>(std::max(0, source.track_count)) * 16, -1);
    for (const auto& assignment : assignmentsIn) {
        int64_t slot = static_cast<int64_t>(assignment->profile->lane.track) * 16 + assignment->profile->lane.source_channel;
        if (slot >= 0 && slot < static_cast<int64_t>(destinations_by_slot_.size()))
            destinations_by_slot_[static_cast<size_t>(slot)] = assignment->destination_channel;
    }
    int32_t canonicalDestination[16];
    for (int channel = 0; channel < 16; ++channel) canonicalDestination[channel] = -1;
    for (const auto& assignment : assignmentsIn) {
        if (assignment->omitted || assignment->destination_channel < 0) continue;
        int sourceChannel = assignment->profile->lane.source_channel;
        if (canonicalDestination[sourceChannel] < 0) canonicalDestination[sourceChannel] = assignment->destination_channel;
    }
    const int32_t count = sourceStore.count();
    event_destinations_.assign(static_cast<size_t>(count), 0);
    for (int32_t eventIndex = 0; eventIndex < count; ++eventIndex) {
        EventView view = sourceStore.at(eventIndex);
        if (view.channel() < 0) {
            event_destinations_[static_cast<size_t>(eventIndex)] = -1;
            continue;
        }
        int stateKind;
        int stateValue;
        if (try_classify_patch_state(view, stateKind, stateValue)) {
            // Destination patch state is compiled separately below.  No source
            // Bank/Program event is allowed to race with that canonical timeline.
            event_destinations_[static_cast<size_t>(eventIndex)] = -2;
            continue;
        }
        int64_t slot = static_cast<int64_t>(view.track) * 16 + view.channel();
        int32_t assigned = slot >= 0 && slot < static_cast<int64_t>(destinations_by_slot_.size())
            ? destinations_by_slot_[static_cast<size_t>(slot)] : -1;
        if (assigned < 0 && is_channel_state(view)) assigned = canonicalDestination[view.channel()];
        event_destinations_[static_cast<size_t>(eventIndex)] = assigned < 0 ? static_cast<int8_t>(-2) : static_cast<int8_t>(assigned);
    }
    lanes = std::move(lanesIn);
    assignments = std::move(assignmentsIn);
    destinations = std::move(destinationsIn);
    build_canonical_patch_events(sourceStore);
    build_statistics(sourceStore);
    errors = std::move(errorsIn);
    requires_explicit_apply = requiresExplicitApply;
    highest_tier = highestTier;
}

void CompiledTrackRoute::build_canonical_patch_events(const EventSource& sourceStore)
{
    const TrackRouteDestinationPreview* byDestination[16] = {};
    for (const TrackRouteDestinationPreview& destination : destinations)
        byDestination[destination.destination_channel] = &destination;
    std::map<TrackLaneKey, const Profile*> byLane;
    for (const auto& assignment : assignments)
        if (!assignment->omitted) byLane[assignment->profile->lane] = assignment->profile.get();

    MidiPatchIdentity current[16];
    int32_t sourceDestinations[16];
    for (int channel = 0; channel < 16; ++channel) {
        current[channel] = MidiPatchIdentity(0, 0, 0);
        sourceDestinations[channel] = -1;
    }
    for (const auto& assignment : assignments)
        if (!assignment->omitted && assignment->destination_channel >= 0)
            sourceDestinations[assignment->profile->lane.source_channel] = assignment->destination_channel;
    const int32_t count = sourceStore.count();
    for (int32_t eventIndex = 0; eventIndex < count; ++eventIndex) {
        EventView source = sourceStore.at(eventIndex);
        if (source.channel() < 0) continue;
        bool attack = (source.status() & 0xF0) == 0x90 && source.data_length >= 3 && source.data_byte(2) != 0 &&
            event_destinations_[static_cast<size_t>(eventIndex)] >= 0;
        int stateKind, stateValue;
        bool patchState = try_classify_patch_state(source, stateKind, stateValue);
        if (!attack && !patchState) continue;
        int32_t destinationChannel = attack ? event_destinations_[static_cast<size_t>(eventIndex)]
                                            : sourceDestinations[source.channel()];
        if (destinationChannel < 0) continue;
        const TrackRouteDestinationPreview* destination = byDestination[destinationChannel];
        if (destination == nullptr) continue;
        const Profile* sourceProfile = nullptr;
        auto found = byLane.find(TrackLaneKey(source.track, source.channel()));
        if (found != byLane.end()) sourceProfile = found->second;
        if (sourceProfile == nullptr) sourceProfile = best_profile_for_source_channel(*destination, source.channel());
        const Profile* owner = canonical_profile_at_time(*destination, source.micros, sourceProfile);
        if (owner == nullptr ||
            patchState && (!is_sounding_at(*owner, source.micros) || owner->lane.source_channel != source.channel()))
            continue;
        MidiPatchIdentity desired = normalize_patch(patch_at(*owner, eventIndex));
        TrackRouteGeneratedPatch group;
        group.source_event_index = eventIndex;
        group.destination_channel = static_cast<uint8_t>(destinationChannel);
        bool bankChanged = current[destinationChannel].bank_msb != desired.bank_msb ||
            current[destinationChannel].bank_lsb != desired.bank_lsb;
        if (current[destinationChannel].bank_msb != desired.bank_msb) {
            group.mask |= 1;
            group.bank_msb = static_cast<uint8_t>(desired.bank_msb);
        }
        if (current[destinationChannel].bank_lsb != desired.bank_lsb) {
            group.mask |= 2;
            group.bank_lsb = static_cast<uint8_t>(desired.bank_lsb);
        }
        if (bankChanged || current[destinationChannel].program != desired.program) {
            group.mask |= 4;
            group.program = static_cast<uint8_t>(desired.program);
        }
        if (group.mask != 0) generated_.push_back(group);
        current[destinationChannel] = desired;
    }
}

void CompiledTrackRoute::build_statistics(const EventSource& sourceStore)
{
    TrackRouteStatistics s;
    for (const auto& assignment : assignments) {
        if (assignment->omitted) {
            s.omitted_lane_count++;
            s.omitted_note_start_count = kInt64Max - s.omitted_note_start_count < assignment->profile->attack_count
                ? kInt64Max : s.omitted_note_start_count + assignment->profile->attack_count;
            continue;
        }
        if (assignment->destination_channel >= 0) s.routed_lane_count++;
        if (assignment->retimbred) s.retimbred_lane_count++;
    }
    for (const TrackRouteDestinationPreview& destination : destinations)
        if (destination.assignments.size() > 1)
            s.merged_lane_count += static_cast<int32_t>(destination.assignments.size()) - 1;
    int32_t generated = 0;
    for (const TrackRouteGeneratedPatch& group : generated_) {
        if (kInt32Max - generated < group.event_count())
            models::throw_invalid_operation("Track routing would exceed the supported indexed routed-view limit.");
        generated += group.event_count();
    }
    generated_.shrink_to_fit();
    int32_t included = 0;
    const int32_t count = sourceStore.count();
    for (int32_t index = 0; index < count; ++index)
        if (includes(sourceStore.at(index))) included++;
    if (generated > kInt32Max - included)
        models::throw_invalid_operation("Track routing would exceed the supported indexed routed-view limit.");
    s.destination_count = static_cast<int32_t>(destinations.size());
    s.generated_destination_state_count = generated;
    s.included_source_event_count = included;
    s.routed_output_event_count = included + generated;
    statistics = s;
}

bool CompiledTrackRoute::try_transform(const EventView& source, EventView& routed) const
{
    if (source.channel() < 0) {
        routed = source;
        return true;
    }
    int32_t eventIndex = source.source_index;
    if (eventIndex < 0 || eventIndex >= static_cast<int32_t>(event_destinations_.size()) ||
        event_destinations_[static_cast<size_t>(eventIndex)] < 0) {
        routed = EventView();
        return false;
    }
    routed = source.with_output_channel(event_destinations_[static_cast<size_t>(eventIndex)]);
    return true;
}

bool CompiledTrackRoute::includes(const EventView& source) const
{
    return source.channel() < 0 || source.source_index >= 0 &&
        source.source_index < static_cast<int32_t>(event_destinations_.size()) &&
        event_destinations_[static_cast<size_t>(source.source_index)] >= -1;
}

const TrackRouteGeneratedPatch* CompiledTrackRoute::generated_patch_before(int32_t sourceEventIndex) const
{
    auto found = std::lower_bound(generated_.begin(), generated_.end(), sourceEventIndex,
        [](const TrackRouteGeneratedPatch& group, int32_t index) { return group.source_event_index < index; });
    return found == generated_.end() || found->source_event_index != sourceEventIndex ? nullptr : &*found;
}

EventView CompiledTrackRoute::generated_event(const TrackRouteGeneratedPatch& group, int k) const
{
    EventView source = source_events_.at(group.source_event_index);
    const int destination = group.destination_channel;
    int seen = 0;
    if (group.mask & 1) {
        if (seen++ == k) return make_generated_patch_event(source, destination, EventKind::ControlChange, 0, group.bank_msb);
    }
    if (group.mask & 2) {
        if (seen++ == k) return make_generated_patch_event(source, destination, EventKind::ControlChange, 32, group.bank_lsb);
    }
    if (group.mask & 4) {
        if (seen++ == k) return make_generated_patch_event(source, destination, EventKind::ProgramChange, -1, group.program);
    }
    models::throw_argument_out_of_range("k");
}

std::shared_ptr<CompiledTrackRoute> compile_route(const RoutingSong& song, const TrackRoutingSettings& settings)
{
    Overlaps sameKeyOverlaps;
    std::vector<ProfilePtr> profiles = build_profiles(song, sameKeyOverlaps);
    std::vector<std::string> errors;
    std::vector<int32_t> available = available_destinations(settings, profiles, errors);
    bool needsMelodicDestination = settings.percussion_policy != PercussionRoutingPolicy::ReserveChannel10;
    for (size_t index = 0; index < profiles.size() && !needsMelodicDestination; ++index)
        needsMelodicDestination = profiles[index]->lane.source_channel != 9;
    if (available.empty() && needsMelodicDestination) errors.push_back("No usable melodic destination channel is selected.");

    std::vector<GroupPtr> groups;
    std::vector<std::shared_ptr<TrackRouteAssignment>> assignments;
    std::vector<ProfilePtr> unresolved;
    std::set<int32_t> used;

    std::sort(profiles.begin(), profiles.end(), [](const ProfilePtr& left, const ProfilePtr& right) {
        if (left->dominance_score != right->dominance_score) return left->dominance_score > right->dominance_score;
        return left->lane.compare(right->lane) < 0;
    });
    for (const ProfilePtr& lane : profiles) {
        auto manual = settings.manual_destinations.find(lane->lane);
        if (manual != settings.manual_destinations.end()) {
            int32_t destination = manual->second;
            if (destination < 0 || destination > 15 ||
                !settings.usable_destination_channels[static_cast<size_t>(destination)]) {
                errors.push_back(lane->lane.to_string() + " has an unavailable manual destination.");
                unresolved.push_back(lane);
                continue;
            }
            DestinationGroup* manualGroup = find_or_create_group(groups, destination, used);
            add_assignment(assignments, *manualGroup, lane, TrackRoutingTier::Aggressive, true, sameKeyOverlaps);
            continue;
        }

        if (settings.percussion_policy == PercussionRoutingPolicy::ReserveChannel10 && lane->lane.source_channel == 9) {
            if (!settings.usable_destination_channels[9]) {
                errors.push_back(lane->lane.to_string() + " is percussion but destination channel 10 is unavailable.");
                unresolved.push_back(lane);
            } else {
                DestinationGroup* percussion = find_or_create_group(groups, 9, used);
                add_assignment(assignments, *percussion, lane, TrackRoutingTier::ExactSafe, false, sameKeyOverlaps);
            }
            continue;
        }

        if (settings.maximum_automatic_tier == TrackRoutingTier::Aggressive) {
            unresolved.push_back(lane);
            continue;
        }

        DestinationGroup* exact = find_exact_group(groups, *lane, sameKeyOverlaps, settings);
        if (exact == nullptr) {
            int32_t destination = choose_unused_destination(available, used, lane->lane.source_channel);
            if (destination >= 0) exact = find_or_create_group(groups, destination, used);
        }
        if (exact == nullptr) unresolved.push_back(lane);
        else add_assignment(assignments, *exact, lane, TrackRoutingTier::ExactSafe, false, sameKeyOverlaps);
    }

    if (settings.maximum_automatic_tier == TrackRoutingTier::Aggressive)
        resolve_aggressive_globally(unresolved, assignments, groups, available, used, sameKeyOverlaps, settings);
    else if (settings.maximum_automatic_tier >= TrackRoutingTier::GmFamily)
        resolve_by_tier(unresolved, assignments, groups, sameKeyOverlaps, settings, TrackRoutingTier::GmFamily);
    for (const ProfilePtr& lane : unresolved)
        errors.push_back(lane->lane.to_string() + " cannot be routed within the selected capacity and tier.");

    std::sort(assignments.begin(), assignments.end(),
        [](const std::shared_ptr<TrackRouteAssignment>& left, const std::shared_ptr<TrackRouteAssignment>& right) {
            return left->profile->lane.compare(right->profile->lane) < 0;
        });
    std::vector<TrackRouteDestinationPreview> preview = build_preview(groups, assignments);
    int32_t stateDestinations[16];
    for (int channel = 0; channel < 16; ++channel) stateDestinations[channel] = -1;
    for (const auto& assignment : assignments) {
        if (assignment->omitted || assignment->destination_channel < 0) continue;
        int sourceChannel = assignment->profile->lane.source_channel;
        if (stateDestinations[sourceChannel] >= 0 && stateDestinations[sourceChannel] != assignment->destination_channel)
            errors.push_back("Source channel " + n(sourceChannel + 1) +
                " has global state but its MIDI tracks use multiple destinations.");
        else stateDestinations[sourceChannel] = assignment->destination_channel;
    }
    TrackRoutingTier highest = TrackRoutingTier::ExactSafe;
    bool explicitApply = false;
    for (const auto& assignment : assignments) {
        if (assignment->tier > highest) highest = assignment->tier;
        if (assignment->tier != TrackRoutingTier::ExactSafe || assignment->manual) explicitApply = true;
    }
    std::vector<ProfilePtr> displayLanes(profiles);
    std::sort(displayLanes.begin(), displayLanes.end(),
        [](const ProfilePtr& left, const ProfilePtr& right) { return left->lane.compare(right->lane) < 0; });
    return std::make_shared<CompiledTrackRoute>(song, std::move(displayLanes), std::move(assignments), std::move(preview),
        std::move(errors), explicitApply, highest);
}

// ------------------------------------------------------------ routed view

TrackRoutedEventStore::TrackRoutedEventStore(const RoutingSong& source, std::shared_ptr<const CompiledTrackRoute> route)
    : source_(source.events), route_(std::move(route))
{
    if (!route_) models::throw_argument_null("route");
    if (!route_->is_for_source(source.identity))
        models::throw_invalid_operation("The compiled track route belongs to a different source song.");
    if (!route_->is_valid()) models::throw_invalid_operation("A track route with unresolved lanes cannot be applied.");
    entries_.reserve(static_cast<size_t>(route_->statistics.routed_output_event_count));
    const std::vector<TrackRouteGeneratedPatch>& groups = route_->generated_patches();
    if (groups.size() >= (static_cast<size_t>(1) << 29))
        models::throw_invalid_operation("Track routing would exceed the supported indexed routed-view limit.");
    size_t next = 0;
    const int32_t count = source_.count();
    for (int32_t index = 0; index < count; ++index) {
        while (next < groups.size() && groups[next].source_event_index == index) {
            for (int k = 0; k < groups[next].event_count(); ++k)
                entries_.push_back(-1 - static_cast<int32_t>((next << 2) | static_cast<size_t>(k)));
            ++next;
        }
        if (route_->includes(source_.at(index))) entries_.push_back(index);
    }
    if (static_cast<int32_t>(entries_.size()) != route_->statistics.routed_output_event_count)
        models::throw_invalid_operation("Track-routing statistics do not match the compiled routed event view.");
}

EventView TrackRoutedEventStore::at(int32_t index) const
{
    const int32_t entry = entries_[static_cast<size_t>(index)];
    if (entry < 0) {
        const int32_t code = -1 - entry;
        const TrackRouteGeneratedPatch& group = route_->generated_patches()[static_cast<size_t>(code >> 2)];
        return route_->generated_event(group, code & 3).with_logical_event_index(index);
    }
    EventView source = source_.at(entry);
    EventView routed;
    if (!route_->try_transform(source, routed))
        models::throw_invalid_operation(("The compiled track route does not assign " +
            TrackLaneKey(source.track, source.channel()).to_string() + ".").c_str());
    return routed.with_logical_event_index(index);
}

} // namespace routing
} // namespace midibn
