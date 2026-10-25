// Sample file format and analysis shared by the real-device measurement tools
// (v2/tools/device_measure_main.cpp and the v1 driver in
// v2/harness/v1-reference/device/DeviceMeasure.cs, which writes the same format).
//
// A "sample file" holds the sampled sends of ONE playback run: every Nth send
// (N = sample_every) is timestamped before and after the output call. The
// analyzer turns it into plain key=value lines (lateness percentiles, stalls,
// periodicity, send-call duration, ...). See v2/docs/DEVICE_MEASUREMENTS.md.
//
// Sample file layout (little endian):
//   header, 80 bytes:
//     char    magic[8] = "MBDEVS01"
//     uint32  version = 1
//     uint32  sample_every
//     int64   frequency            (timer ticks per second, QueryPerformanceFrequency / Stopwatch.Frequency)
//     int64   total_sends          (all sends of the run, sampled or not)
//     int64   sample_count
//     double  qpc_cost_ns          (cost of one timer read; subtracted from the send-call durations)
//     int64   expected_events      (events in the fixture)
//     int64   epoch_rel_ticks      (the engine's own time zero, in ticks after t0; before_rel is measured from t0)
//     int64   reserved[2]
//   then sample_count records of 32 bytes:
//     int64   k                    (0-based index of the send)
//     int64   intended_us          (the event's intended time, microseconds from the song start)
//     int64   before_rel_ticks     (timer value just before the send call, minus the value just before engine start)
//     int32   duration_ticks       (timer value after the send call minus before_rel's source value)
//     int32   reserved
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace tools {

struct SampleHeader {
    char magic[8];
    uint32_t version;
    uint32_t sample_every;
    int64_t frequency;
    int64_t total_sends;
    int64_t sample_count;
    double qpc_cost_ns;
    int64_t expected_events;
    int64_t epoch_rel_ticks;
    int64_t reserved[2];
};
static_assert(sizeof(SampleHeader) == 80, "sample header must be 80 bytes");

struct SampleRecord {
    int64_t k;
    int64_t intended_us;
    int64_t before_rel_ticks;
    int32_t duration_ticks;
    int32_t reserved;
};
static_assert(sizeof(SampleRecord) == 32, "sample record must be 32 bytes");

struct SampleRun {
    SampleHeader header;
    std::vector<SampleRecord> samples;
};

bool write_sample_file(const std::string& path, const SampleRun& run);
bool read_sample_file(const std::string& path, SampleRun& run, std::string& error);

// Analysis result as ordered key=value lines (no trailing newline per line is
// added; each entry is "key=value"). `timeline_csv_path` (optional) receives a
// per-millisecond lateness timeline.
std::vector<std::string> analyze_samples(const SampleRun& run, const std::string& timeline_csv_path);

} // namespace tools
} // namespace midibn
