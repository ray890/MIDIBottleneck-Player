// MIDI file loading: container probe, preflight scan and memory projection,
// and the production parser (v1 MidiFileContent / MidiLargeFilePreflight /
// DirectCompactMidiParser equivalents; results and messages are v1's except where the
// improvement register, v2/docs/DIFFERENCES_FROM_V1.md, says otherwise).
#pragma once

#include "midibn/channel_index.h"
#include "midibn/event_store.h"
#include "midibn/load_progress.h"
#include "midibn/platform.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace midibn {

// A failure reported exactly as v1 reports it: the .NET exception type name
// and the user-visible message.
struct LoadError {
    std::string type;
    std::string message;
};

struct TrackMetadata {
    std::u16string name;  // UTF-16 code units, as v1 stores them
    bool name_truncated = false;
    std::u16string instrument;
    bool instrument_truncated = false;
};

struct TempoMapEntry {
    int64_t tick;
    int64_t time_us;
    int32_t microseconds_per_quarter;
    int32_t track;
    int32_t order;
};

struct Song {
    int64_t file_size = 0;
    int format = 0;
    int track_count = 0;
    int division = 0;
    int64_t note_count = 0;
    int64_t running_status_repairs = 0;
    int64_t duration_us = 0;
    std::vector<TrackMetadata> tracks;
    EventStore store;
    SourceValueIndex source_index;  // built in the merge pass, as in v1
    ChaseIndex chase_index;
    std::vector<TempoMapEntry> tempo_map;  // filled only when requested
};

struct LoadOptions {
    bool collect_tempo_map = false;
    LoadMonitor* monitor = nullptr;  // progress and cancellation; may be null
};

// Returns false and fills `error` on failure (cancellation included; see
// is_cancellation).
bool load_song(platform::ByteSource& source, const LoadOptions& options, Song& song, LoadError& error);
bool is_cancellation(const LoadError& error);

// Container probe: true for bare SMF or RIFF/RMID with a data chunk.
bool probe_container(platform::ByteSource& source, bool& is_rmid, LoadError& error);

struct PreflightCounts {
    int64_t file_size = 0;
    int format = 0;
    int track_count = 0;
    int division = 0;
    int64_t dispatchable_events = 0;
    int64_t note_ons = 0;
    int64_t final_long_payload_bytes = 0;
    int64_t final_long_payload_events = 0;  // SysEx messages of 4 bytes or more (not part of v1's trace)
    int64_t provisional_payload_bytes = 0;
    int64_t tempo_changes = 0;
    int64_t source_value_entries = 0;
    int64_t chase_state_entries = 0;
    int64_t chase_parameter_value_entries = 0;
    int64_t running_status_repairs = 0;
    int64_t routing_lanes = 0;
    int64_t routing_patch_state_events = 0;
    int64_t routing_patch_timeline_upper_bound = 0;
    int64_t routing_generated_state_upper_bound = 0;
    std::vector<int64_t> events_by_track;
    std::vector<int64_t> payload_bytes_by_track;
    std::vector<int64_t> tempo_changes_by_track;
    std::array<int64_t, 16 * 9> source_entries_by_slot = {};
    std::array<int64_t, 16> routing_note_lanes_by_channel = {};
    std::array<int64_t, 16> routing_patch_state_events_by_channel = {};
};

struct MemoryProjection {
    int64_t retained = 0;
    int64_t peak_low = 0;
    int64_t peak_high = 0;
    int64_t provisional = 0;
    int64_t merge_workspace = 0;
    int64_t retained_names = 0;
    int64_t routing_retained = 0;
    int64_t routing_peak = 0;
    int64_t retained_with_routing = 0;
    int64_t routing_peak_total = 0;
    int pointer_size = 0;
    bool architecture_risk = false;
    int64_t warning_threshold = 0;
};

bool preflight_scan(platform::ByteSource& source, PreflightCounts& counts, LoadError& error,
    LoadMonitor* monitor = nullptr);
// v1 MidiLargeFilePreflight.Estimate for a CLR pointer size of 4 or 8.
// warning_threshold 0 = the built-in value for the pointer size (v1's: 4 GiB for 8, 1 GiB for 4).
MemoryProjection preflight_estimate(const PreflightCounts& counts, int pointer_size, int64_t warning_threshold = 0);
// 32-bit builds: half of the process's real user-mode address space (GetSystemInfo lpMaximumApplicationAddress), rounded
// to the nearest 64 MiB so that the 64 KiB guard at the top does not show: 0x7FFEFFFF -> 1 GiB (32-bit Windows, XP),
// 0xBFFEFFFF -> 1.5 GiB (/3GB), 0xFFFEFFFF -> 2 GiB (32-bit large-address-aware program on 64-bit Windows).
// Returns 0 (= use the built-in value) when the address is 0.
int64_t half_address_space_warning_threshold(uint64_t maximum_application_address);
bool preflight_requires_warning(const MemoryProjection& projection);
bool preflight_required(int64_t file_size, int pointer_size, int64_t warning_threshold = 0);

// v1 MidiFileParser.LoadWithPreflight: large files (or forced) are inspected
// first; when v1's projection requires a warning (or forced) the decision is
// asked, and a refusal ends the flow without loading.
enum class LoadFlowResult { Loaded, Declined, Failed };

struct LoadFlowOptions {
    LoadMonitor* monitor = nullptr;
    // Empty, or returning false, declines the load (v1: null result).
    std::function<bool(const PreflightCounts&, const MemoryProjection&)> decision;
    bool force_preflight = false;
    bool force_warning = false;
    int64_t warning_threshold = 0;   // 0 = the built-in value for the pointer size
    bool collect_tempo_map = false;
};

LoadFlowResult load_with_preflight(platform::ByteSource& source, int pointer_size, const LoadFlowOptions& options,
    Song& song, LoadError& error);

} // namespace midibn
