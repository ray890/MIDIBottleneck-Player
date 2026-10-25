// Built-in software synthesizer for Android: a thin, thread-aware wrapper around Sonivox EAS (Android's own synth;
// Apache 2.0; v2/android/third_party/sonivox). No Android classes here, so the tests run on any host.
//
// Threads. Sonivox is not thread-safe, so exactly one thread touches it: the audio render thread (render()).
//   * The engine's output thread calls push() and discard_pending(). They are lock-free (a single-producer
//     single-consumer byte ring plus atomics) and never wait for the render thread, however long it takes.
//   * The render thread calls render(): it drains the ring into EAS_WriteMIDIStream, then EAS_Render.
//   * init() and shutdown() run while no render thread is active (the audio sink starts after init and has stopped
//     before shutdown). render() and shutdown() share a mutex only so a late call cannot race the teardown; push()
//     and discard_pending() never take it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "dls_bank.h"

namespace midibn {
namespace android {

// The user's synth settings (the "Synth settings" page). Written by any thread through the setters (atomics); the render
// thread reads them before the next buffer and applies them to Sonivox, so Sonivox is never called from another thread.
// Nothing is saved: the values live as long as this object (the process, for the real one), and a synth that opens later
// starts with whatever is set.
struct SynthSettings {
    static const int kVolumeMax = 200;     // percent; 100 = the synth's normal level, above 100 amplifies (and may clip)
    static const int kPresetCount = 4;     // Sonivox reverb presets: 0 large hall, 1 hall, 2 chamber, 3 room
    static const int kAmountMax = 100;     // percent of the reverb's own wet level

    // Android's normal voice limit: what the synth starts with, even when the build allows more (max_voices()).
    static const int kDefaultVoices = 64;

    SynthSettings() : volume_percent(100), voice_limit(kDefaultVoices), reverb_on(false), reverb_preset(1), reverb_amount(85), version(1) {}

    // Setters clamp to the valid range. voice_limit is clamped by the synth to its build maximum when applied.
    void set_volume_percent(int v) { volume_percent.store(clamp(v, 0, kVolumeMax)); bump(); }
    void set_voice_limit(int v) { voice_limit.store(clamp(v, 1, 1 << 20)); bump(); }
    void set_reverb_on(bool on) { reverb_on.store(on); bump(); }
    void set_reverb_preset(int p) { reverb_preset.store(clamp(p, 0, kPresetCount - 1)); bump(); }
    void set_reverb_amount(int a) { reverb_amount.store(clamp(a, 0, kAmountMax)); bump(); }
    void reset() { volume_percent = 100; voice_limit = kDefaultVoices; reverb_on = false; reverb_preset = 1; reverb_amount = 85; bump(); }

    std::atomic<int> volume_percent;
    std::atomic<int> voice_limit;       // kDefaultVoices until the user changes it; the synth caps it at max_voices() when applying
    std::atomic<bool> reverb_on;
    std::atomic<int> reverb_preset;
    std::atomic<int> reverb_amount;
    std::atomic<uint32_t> version;      // changes whenever a field changes

    // The DLS sound bank the user loaded (empty = Sonivox's built-in sounds). It has its own generation counter; the render
    // thread installs it. Not part of reset(): "Use built-in sounds" is its own button. Not saved.
    DlsBank dls;

private:
    static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    void bump() { version.fetch_add(1, std::memory_order_release); }
};

// The settings of the real synth (one per process). Tests use the synth's own settings instead.
SynthSettings& global_synth_settings();

struct SynthStats {
    uint64_t bytes_in = 0;          // bytes accepted into the ring
    uint64_t overflow_writes = 0;   // writes dropped because the ring was full
    uint64_t overflow_bytes = 0;    // bytes in those writes
    uint64_t discarded_bytes = 0;   // bytes thrown away unread by discard_pending (reset)
    uint64_t not_running_writes = 0;// writes dropped because no audio was running
    uint64_t bytes_rendered = 0;    // bytes handed to Sonivox
    uint64_t blocks_rendered = 0;   // Sonivox blocks (128 frames each) rendered
    uint64_t slow_buffers = 0;      // render() calls that took longer than the audio they produced
    uint64_t max_render_us = 0;     // longest render() call
    uint64_t render_errors = 0;     // EAS errors
    uint32_t voices = 0;            // voices sounding after the last render (release tails included)
    // Level meter. Clipping is counted where Sonivox saturates: samples at the 16-bit limits (32767 or -32768) in the
    // rendered audio before the player's own fine gain. The peak is of the audio that is sent to the speaker.
    uint32_t peak = 0;              // 0 to 32768: loudest sample of the last few buffers (falls 10 percent per buffer)
    uint64_t clipped_buffers = 0;   // render() calls that had at least one clipped sample
    uint64_t clipped_samples = 0;   // clipped samples (left and right counted separately)
};

class SonivoxSynth {
public:
    static const int kSampleRate = 22050;   // fixed at library build time (-D_SAMPLE_RATE_22050)
    static const int kChannels = 2;
    static const int kBlockFrames = 128;    // frames per EAS_Render call (BUFFER_SIZE_IN_MONO_SAMPLES)

    // ring_bytes is rounded up to a power of two.
    explicit SonivoxSynth(size_t ring_bytes = 65536);
    ~SonivoxSynth();
    SonivoxSynth(const SonivoxSynth&) = delete;
    SonivoxSynth& operator=(const SonivoxSynth&) = delete;

    // Starts Sonivox and opens its MIDI stream. False with a plain sentence in `error`.
    bool init(std::string& error);
    // Stops Sonivox. The audio must not be rendering any more (or render() must not be called again).
    void shutdown();
    bool is_ready() const { return ready_.load(std::memory_order_acquire); }

    // ---- send side (one thread) ---------------------------------------------------------------------------------
    // Queues MIDI bytes for the render thread. Never blocks. False when the bytes were dropped (ring full, or the
    // synth is not ready); the counters say which.
    bool push(const uint8_t* data, size_t length);
    // The core's reset: bytes queued but not yet read by the render thread are skipped. Never blocks.
    void discard_pending();

    // ---- render side (one thread) -------------------------------------------------------------------------------
    // Writes `frames` stereo frames (interleaved 16-bit, 2 * frames values) to `out`. Silence when not ready.
    void render(int16_t* out, size_t frames);

    // ---- settings ------------------------------------------------------------------------------------------------
    // Any thread. The settings object read by the render thread: the synth's own by default; the real route points it at
    // global_synth_settings() before init. Set it only while the synth is closed.
    SynthSettings& settings() { return *settings_; }
    void use_settings(SynthSettings* s) { settings_ = s ? s : &own_settings_; }
    // The voice maximum this Sonivox build was compiled with (MAX_SYNTH_VOICES).
    static int max_voices();

    // Test hook: called inside render() while the render lock is held, once per call, before the work.
    void set_render_hook(std::function<void()> hook) { hook_ = std::move(hook); }

    SynthStats stats() const;
    size_t ring_capacity() const { return capacity_; }
    // Buffered bytes not yet read (an estimate; send side).
    size_t pending_bytes() const;

private:
    void drain_ring();
    void apply_settings();          // render thread, render_lock_ held: pushes changed settings into Sonivox
    void apply_gain(int16_t* out, size_t frames);
    void update_meter(const int16_t* out, size_t frames, size_t clipped);

    std::vector<uint8_t> ring_;
    size_t capacity_;
    size_t mask_;
    alignas(64) std::atomic<uint64_t> head_;          // written by the send side
    alignas(64) std::atomic<uint64_t> tail_;          // written by the render side
    alignas(64) std::atomic<uint64_t> discard_mark_;  // written by the send side: head at the last discard_pending
    std::atomic<bool> ready_;

    std::mutex render_lock_;
    void* eas_ = nullptr;          // EAS_DATA_HANDLE
    void* stream_ = nullptr;       // EAS_HANDLE
    std::vector<int16_t> carry_;   // one block, for buffers that are not a multiple of the block size
    size_t carry_pos_ = 0, carry_len_ = 0;                 // frames
    std::vector<uint8_t> scratch_;
    std::function<void()> hook_;

    SynthSettings own_settings_;
    SynthSettings* settings_ = &own_settings_;
    uint32_t applied_version_ = 0;     // render thread: settings version last applied (0 = nothing applied yet)
    int applied_voices_ = 0;
    // Volume. The user's percent is split into Sonivox's own master volume (before its final saturation, 1 dB steps, so a
    // lower volume gives headroom) and a small remaining gain on the finished buffer (ramped, so no click and exact silence at
    // 0 percent). Total gain = gain_ * 10^((master_applied_ - 90) / 20).
    int master_applied_ = 90;          // Sonivox master volume now in effect (90 = the library's default after EAS_Init)
    float gain_ = 1.0f;                // render thread: the output gain reached so far (ramps to gain_target_)
    float gain_target_ = 1.0f;
    uint32_t applied_dls_gen_ = 0xFFFFFFFFu;   // DLS bank generation installed in Sonivox (0xFFFFFFFF = none looked at yet)
    std::atomic<uint32_t> peak_hold_;
    std::atomic<uint64_t> clipped_buffers_, clipped_samples_;
    bool reverb_wet_pending_ = false;  // re-apply the wet level after the next block: a preset change resets it inside Sonivox
    int reverb_amount_ = 85;
    std::atomic<uint32_t> voices_;

    std::atomic<uint64_t> bytes_in_, overflow_writes_, overflow_bytes_, not_running_writes_;
    std::atomic<uint64_t> discarded_bytes_, bytes_rendered_, blocks_rendered_, slow_buffers_, max_render_us_, render_errors_;
};

} // namespace android
} // namespace midibn
