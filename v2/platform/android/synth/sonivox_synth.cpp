#include "sonivox_synth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "eas.h"          // Sonivox (v2/android/third_party/sonivox/host_src)
#include "eas_reverb.h"   // reverb parameter and preset numbers

// The player's own additions to Sonivox (sonivox_midi_stream.c), compiled with the library.
extern "C" {
EAS_RESULT MIDIBN_SetStreamPolyphony(EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle, EAS_I32 voices);
EAS_I32 MIDIBN_StreamActiveVoices(EAS_HANDLE streamHandle);
EAS_I32 MIDIBN_MaxVoices(void);
EAS_RESULT MIDIBN_SetMasterVolume(EAS_DATA_HANDLE pEASData, EAS_I32 volume);
}

namespace midibn {
namespace android {

namespace {

size_t round_up_pow2(size_t n)
{
    size_t p = 1024;
    while (p < n) p <<= 1;
    return p;
}

const size_t kScratchBytes = 1024;

} // namespace

SynthSettings& global_synth_settings()
{
    static SynthSettings settings;
    return settings;
}

int SonivoxSynth::max_voices() { return static_cast<int>(MIDIBN_MaxVoices()); }

SonivoxSynth::SonivoxSynth(size_t ring_bytes)
    : ring_(round_up_pow2(ring_bytes)), capacity_(ring_.size()), mask_(ring_.size() - 1),
      head_(0), tail_(0), discard_mark_(0), ready_(false),
      carry_(static_cast<size_t>(kBlockFrames) * kChannels), scratch_(kScratchBytes),
      bytes_in_(0), overflow_writes_(0), overflow_bytes_(0), not_running_writes_(0), discarded_bytes_(0),
      bytes_rendered_(0), blocks_rendered_(0), slow_buffers_(0), max_render_us_(0), render_errors_(0)
{
    voices_ = 0;
    peak_hold_ = 0;
    clipped_buffers_ = clipped_samples_ = 0;
}

SonivoxSynth::~SonivoxSynth() { shutdown(); }

bool SonivoxSynth::init(std::string& error)
{
    shutdown();
    const S_EAS_LIB_CONFIG* config = EAS_Config();
    if (!config || config->sampleRate != kSampleRate || config->numChannels != kChannels ||
        config->mixBufferSize != kBlockFrames) {
        error = "The built-in synthesizer library was built with settings this player does not expect.";
        return false;
    }
    EAS_DATA_HANDLE data = nullptr;
    if (EAS_Init(&data) != EAS_SUCCESS || !data) {
        error = "The built-in synthesizer could not be started.";
        return false;
    }
    EAS_HANDLE stream = nullptr;
    if (EAS_OpenMIDIStream(data, &stream, nullptr) != EAS_SUCCESS || !stream) {
        EAS_Shutdown(data);
        error = "The built-in synthesizer could not open its MIDI input.";
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(render_lock_);
        eas_ = data;
        stream_ = stream;
        carry_pos_ = carry_len_ = 0;
        applied_version_ = 0;   // the first render applies every setting to the new Sonivox instance
        applied_voices_ = 0;
        // A new Sonivox instance starts at master volume 90; the first render() moves it to the step the volume needs and
        // rescales gain_ so that the total stays the same (no ramp from an old level).
        master_applied_ = 90;
        gain_ = gain_target_ = static_cast<float>(settings_->volume_percent.load()) / 100.0f;
        applied_dls_gen_ = 0xFFFFFFFFu;
        reverb_wet_pending_ = false;
    }
    voices_ = 0;
    peak_hold_ = 0;
    clipped_buffers_ = clipped_samples_ = 0;
    // Fresh ring and counters for this session. (No other thread is using the synth: the audio has not started.)
    uint64_t head = head_.load(std::memory_order_relaxed);
    tail_.store(head, std::memory_order_relaxed);
    discard_mark_.store(head, std::memory_order_relaxed);
    bytes_in_ = overflow_writes_ = overflow_bytes_ = not_running_writes_ = 0;
    discarded_bytes_ = bytes_rendered_ = blocks_rendered_ = slow_buffers_ = max_render_us_ = render_errors_ = 0;
    ready_.store(true, std::memory_order_release);
    return true;
}

void SonivoxSynth::shutdown()
{
    ready_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> guard(render_lock_);
    if (eas_) {
        EAS_DATA_HANDLE data = static_cast<EAS_DATA_HANDLE>(eas_);
        if (stream_ && applied_dls_gen_ != 0xFFFFFFFFu) settings_->dls.uninstall(data, stream_);   // reference count under the bank's mutex
        if (stream_) EAS_CloseMIDIStream(data, static_cast<EAS_HANDLE>(stream_));
        EAS_Shutdown(data);
    }
    eas_ = nullptr;
    stream_ = nullptr;
}

bool SonivoxSynth::push(const uint8_t* data, size_t length)
{
    if (length == 0) return true;
    if (!ready_.load(std::memory_order_acquire)) {
        not_running_writes_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t head = head_.load(std::memory_order_relaxed);   // only this thread writes head_
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (length > capacity_ - static_cast<size_t>(head - tail)) {
        overflow_writes_.fetch_add(1, std::memory_order_relaxed);
        overflow_bytes_.fetch_add(length, std::memory_order_relaxed);
        return false;
    }
    const size_t start = static_cast<size_t>(head) & mask_;
    const size_t first = std::min(length, capacity_ - start);
    std::memcpy(&ring_[start], data, first);
    if (first < length) std::memcpy(&ring_[0], data + first, length - first);
    head_.store(head + length, std::memory_order_release);
    bytes_in_.fetch_add(length, std::memory_order_relaxed);
    return true;
}

void SonivoxSynth::discard_pending()
{
    // Everything pushed so far (by this same thread) is skipped by the render side the next time it reads.
    discard_mark_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
}

size_t SonivoxSynth::pending_bytes() const
{
    const uint64_t head = head_.load(std::memory_order_acquire);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    const uint64_t mark = discard_mark_.load(std::memory_order_acquire);
    const uint64_t from = std::max(tail, mark);
    return head > from ? static_cast<size_t>(head - from) : 0;
}

void SonivoxSynth::drain_ring()
{
    // Render thread, render_lock_ held.
    uint64_t tail = tail_.load(std::memory_order_relaxed);   // only this thread writes tail_
    const uint64_t mark = discard_mark_.load(std::memory_order_acquire);
    if (tail < mark) {
        discarded_bytes_.fetch_add(mark - tail, std::memory_order_relaxed);
        tail = mark;
        tail_.store(tail, std::memory_order_release);
    }
    const uint64_t head = head_.load(std::memory_order_acquire);
    while (tail < head) {
        const size_t start = static_cast<size_t>(tail) & mask_;
        size_t count = static_cast<size_t>(head - tail);
        count = std::min(count, capacity_ - start);
        count = std::min(count, scratch_.size());
        std::memcpy(scratch_.data(), &ring_[start], count);   // Sonivox takes a non-const pointer
        EAS_RESULT result = EAS_WriteMIDIStream(static_cast<EAS_DATA_HANDLE>(eas_), static_cast<EAS_HANDLE>(stream_),
                                                scratch_.data(), static_cast<EAS_I32>(count));
        if (result != EAS_SUCCESS) render_errors_.fetch_add(1, std::memory_order_relaxed);
        bytes_rendered_.fetch_add(count, std::memory_order_relaxed);
        tail += count;
        tail_.store(tail, std::memory_order_release);
    }
}

void SonivoxSynth::apply_settings()
{
    // Render thread, render_lock_ held, before the MIDI of this buffer is read.
    const uint32_t version = settings_->version.load(std::memory_order_acquire);
    if (version == applied_version_) return;
    applied_version_ = version;
    EAS_DATA_HANDLE data = static_cast<EAS_DATA_HANDLE>(eas_);

    // Volume: Sonivox's master volume (1 dB steps, applied to the mix before Sonivox saturates it to 16 bits) takes the part
    // of the volume that is a whole number of decibels, rounded so that what is left for the finished buffer is a small
    // reduction (never an increase). A lower volume therefore gives the loud passages headroom instead of just turning
    // clipped audio down. At 0 percent the master stays where it is and the ramped gain brings the output to exact silence.
    const int percent = settings_->volume_percent.load();
    int master = master_applied_;
    if (percent > 0) {
        const double db = 20.0 * std::log10(static_cast<double>(percent) / 100.0);
        master = 90 + static_cast<int>(std::ceil(db - 1e-9));
        master = master < 0 ? 0 : (master > 100 ? 100 : master);
    }
    if (master != master_applied_) {
        if (MIDIBN_SetMasterVolume(data, master) == EAS_SUCCESS) {
            gain_ *= static_cast<float>(std::pow(10.0, (master_applied_ - master) / 20.0));   // the total does not jump
            master_applied_ = master;
        } else {
            render_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    gain_target_ = static_cast<float>(static_cast<double>(percent) / 100.0 / std::pow(10.0, (master_applied_ - 90) / 20.0));

    int voices = std::min(settings_->voice_limit.load(), max_voices());
    if (voices < 1) voices = 1;
    if (voices != applied_voices_) {
        if (MIDIBN_SetStreamPolyphony(data, static_cast<EAS_HANDLE>(stream_), voices) == EAS_SUCCESS) applied_voices_ = voices;
        else render_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    // Reverb. Sonivox starts with it bypassed. Changing the preset makes Sonivox reload that preset's own wet level inside
    // the next block, so the wet level is set again right after that block (render()).
    reverb_amount_ = settings_->reverb_amount.load();
    EAS_RESULT r = EAS_SetParameter(data, EAS_MODULE_REVERB, EAS_PARAM_REVERB_PRESET, settings_->reverb_preset.load());
    r |= EAS_SetParameter(data, EAS_MODULE_REVERB, EAS_PARAM_REVERB_BYPASS, settings_->reverb_on.load() ? 0 : 1);
    r |= EAS_SetParameter(data, EAS_MODULE_REVERB, EAS_PARAM_REVERB_WET, reverb_amount_ * 32767 / SynthSettings::kAmountMax);
    if (r != EAS_SUCCESS) render_errors_.fetch_add(1, std::memory_order_relaxed);
    reverb_wet_pending_ = true;
}

void SonivoxSynth::apply_gain(int16_t* out, size_t frames)
{
    // The master volume: a gain on the finished buffer, ramped across it so a slider move makes no click.
    if (gain_ == 1.0f && gain_target_ == 1.0f) return;
    const float from = gain_, to = gain_target_;
    const size_t total = frames * kChannels;
    for (size_t i = 0; i < total; ++i) {
        const float g = from + (to - from) * static_cast<float>(i / kChannels + 1) / static_cast<float>(frames);
        float v = static_cast<float>(out[i]) * g;
        v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
        out[i] = static_cast<int16_t>(v < 0 ? v - 0.5f : v + 0.5f);
    }
    gain_ = to;
}

void SonivoxSynth::update_meter(const int16_t* out, size_t frames, size_t clipped)
{
    uint32_t peak = 0;
    for (size_t i = 0, n = frames * kChannels; i < n; ++i) {
        const uint32_t a = static_cast<uint32_t>(out[i] < 0 ? -static_cast<int>(out[i]) : out[i]);
        if (a > peak) peak = a;
    }
    // Peak hold: jumps up at once, falls 10 percent per buffer (about a tenth of a second per halving of the level).
    const uint32_t held = peak_hold_.load(std::memory_order_relaxed);
    const uint32_t fallen = held - (held + 9) / 10;
    peak_hold_.store(peak > fallen ? peak : fallen, std::memory_order_relaxed);
    if (clipped) {
        clipped_buffers_.fetch_add(1, std::memory_order_relaxed);
        clipped_samples_.fetch_add(clipped, std::memory_order_relaxed);
    }
}

void SonivoxSynth::render(int16_t* out, size_t frames)
{
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    std::lock_guard<std::mutex> guard(render_lock_);
    if (hook_) hook_();
    if (!eas_) {
        std::memset(out, 0, frames * kChannels * sizeof(int16_t));
        return;
    }
    EAS_DATA_HANDLE data = static_cast<EAS_DATA_HANDLE>(eas_);
    apply_settings();
    {
        // A DLS bank was loaded or removed (on another thread): install it here, the only thread that touches Sonivox.
        const uint32_t generation = settings_->dls.generation();
        if (generation != applied_dls_gen_) {
            if (applied_dls_gen_ == 0xFFFFFFFFu && !settings_->dls.loaded()) applied_dls_gen_ = generation;   // nothing to do yet
            else applied_dls_gen_ = settings_->dls.install(eas_, stream_);
        }
    }
    size_t done = 0;
    while (done < frames) {
        // Whole frames left over from the last block first.
        if (carry_pos_ < carry_len_) {
            const size_t n = std::min(frames - done, carry_len_ - carry_pos_);
            std::memcpy(out + done * kChannels, carry_.data() + carry_pos_ * kChannels, n * kChannels * sizeof(int16_t));
            carry_pos_ += n;
            done += n;
            continue;
        }
        drain_ring();   // MIDI that arrived since the last block takes effect in this one
        int16_t* target = (frames - done >= static_cast<size_t>(kBlockFrames)) ? out + done * kChannels : carry_.data();
        EAS_I32 generated = 0;
        EAS_RESULT result = EAS_Render(data, target, kBlockFrames, &generated);
        if (result != EAS_SUCCESS || generated != kBlockFrames) {
            render_errors_.fetch_add(1, std::memory_order_relaxed);
            std::memset(target, 0, static_cast<size_t>(kBlockFrames) * kChannels * sizeof(int16_t));
        }
        blocks_rendered_.fetch_add(1, std::memory_order_relaxed);
        if (reverb_wet_pending_) {
            reverb_wet_pending_ = false;
            if (EAS_SetParameter(data, EAS_MODULE_REVERB, EAS_PARAM_REVERB_WET, reverb_amount_ * 32767 / SynthSettings::kAmountMax) != EAS_SUCCESS)
                render_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        if (target == carry_.data()) {
            carry_pos_ = 0;
            carry_len_ = kBlockFrames;
        } else {
            done += kBlockFrames;
        }
    }
    // Clipping is what Sonivox's own saturation did: count the samples at the 16-bit limits before the fine gain.
    size_t clipped = 0;
    for (size_t i = 0, n = frames * kChannels; i < n; ++i)
        if (out[i] >= 32767 || out[i] <= -32768) ++clipped;
    apply_gain(out, frames);
    update_meter(out, frames, clipped);
    voices_.store(static_cast<uint32_t>(MIDIBN_StreamActiveVoices(static_cast<EAS_HANDLE>(stream_))), std::memory_order_relaxed);
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());
    uint64_t seen = max_render_us_.load(std::memory_order_relaxed);
    while (us > seen && !max_render_us_.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
    const uint64_t budget_us = static_cast<uint64_t>(frames) * 1000000u / kSampleRate;
    if (us > budget_us) slow_buffers_.fetch_add(1, std::memory_order_relaxed);
}

SynthStats SonivoxSynth::stats() const
{
    SynthStats s;
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.overflow_writes = overflow_writes_.load(std::memory_order_relaxed);
    s.overflow_bytes = overflow_bytes_.load(std::memory_order_relaxed);
    s.discarded_bytes = discarded_bytes_.load(std::memory_order_relaxed);
    s.not_running_writes = not_running_writes_.load(std::memory_order_relaxed);
    s.bytes_rendered = bytes_rendered_.load(std::memory_order_relaxed);
    s.blocks_rendered = blocks_rendered_.load(std::memory_order_relaxed);
    s.slow_buffers = slow_buffers_.load(std::memory_order_relaxed);
    s.max_render_us = max_render_us_.load(std::memory_order_relaxed);
    s.render_errors = render_errors_.load(std::memory_order_relaxed);
    s.voices = voices_.load(std::memory_order_relaxed);
    s.peak = peak_hold_.load(std::memory_order_relaxed);
    s.clipped_buffers = clipped_buffers_.load(std::memory_order_relaxed);
    s.clipped_samples = clipped_samples_.load(std::memory_order_relaxed);
    return s;
}

} // namespace android
} // namespace midibn
