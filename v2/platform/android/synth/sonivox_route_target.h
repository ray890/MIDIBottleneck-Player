// The built-in synthesizer as a route target (same interface as the Java and AMidi targets, route_output.h), so the shared
// BatchingOutput (encoding, bursts, SysEx assembly, reset, panic) applies unchanged.
#pragma once

#include "audio_sink.h"
#include "route_output.h"
#include "sonivox_synth.h"

#include <memory>
#include <string>

namespace midibn {
namespace android {

// Name shown in the output list.
extern const char* const kSonivoxDeviceName;

class SonivoxRouteTarget : public RouteTarget {
public:
    // ring_bytes: capacity of the send ring (default 64 KiB; tests use small rings).
    explicit SonivoxRouteTarget(std::unique_ptr<AudioSink> sink, size_t ring_bytes = 65536);
    ~SonivoxRouteTarget() override;

    const char* name() const override { return "Built-in synthesizer (Sonivox)"; }
    std::vector<platform::OutputDeviceInfo> devices() const override;
    bool open(uint32_t device_id, std::string& error) override;
    void close() override;
    bool is_open() const override { return open_; }
    // Queues the bytes for the audio thread; never blocks, never fails. A full ring drops the write and counts it.
    bool write(const uint8_t* data, size_t length, std::string& error) override;
    void discard_pending() override { synth_.discard_pending(); }
    std::string describe() const override;

    SonivoxSynth& synth() { return synth_; }
    const AudioConfig& audio_config() const { return config_; }
    // Audio queued ahead of the speaker by this player (buffers x buffer length), in milliseconds.
    double queue_latency_ms() const;
    // One line: latency and counters, for the self-test screen and the logs.
    std::string status_text() const;

private:
    SonivoxSynth synth_;
    std::unique_ptr<AudioSink> sink_;
    AudioConfig config_;
    bool open_ = false;
};

// The target for the real device: Sonivox into OpenSL ES. Null off Android (tests pass their own sink to the constructor).
std::unique_ptr<RouteTarget> make_sonivox_route_target();

// Status of the synth route that is open right now (any thread); empty text when none is open.
std::string sonivox_route_status_text();

// Level meter counters of the synth route that is open right now; false when none is open.
bool sonivox_route_stats(SynthStats& out);

} // namespace android
} // namespace midibn
