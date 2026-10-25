#include "sonivox_route_target.h"

#include <cstdio>
#include <mutex>

namespace midibn {
namespace android {

const char* const kSonivoxDeviceName = "Built-in synth (Sonivox)";

namespace {

std::mutex g_status_lock;
SonivoxRouteTarget* g_open_target = nullptr;

} // namespace

SonivoxRouteTarget::SonivoxRouteTarget(std::unique_ptr<AudioSink> sink, size_t ring_bytes)
    : synth_(ring_bytes), sink_(std::move(sink))
{
    config_.sample_rate = SonivoxSynth::kSampleRate;
    config_.channels = SonivoxSynth::kChannels;
    // Two Sonivox blocks (11.6 ms) per buffer, three buffers queued: about 35 ms ahead of the speaker. A smaller buffer
    // means less delay and finer note timing but a bigger risk of dropouts on a slow phone.
    config_.frames_per_buffer = 2 * SonivoxSynth::kBlockFrames;
    config_.buffer_count = 3;
}

SonivoxRouteTarget::~SonivoxRouteTarget() { close(); }

std::vector<platform::OutputDeviceInfo> SonivoxRouteTarget::devices() const
{
    platform::OutputDeviceInfo info;
    info.device_id = 0;
    info.name = kSonivoxDeviceName;
    return std::vector<platform::OutputDeviceInfo>(1, info);
}

bool SonivoxRouteTarget::open(uint32_t device_id, std::string& error)
{
    close();
    if (device_id != 0) { error = "No such MIDI device."; return false; }
    if (!sink_) { error = "The built-in synthesizer has no audio output on this device."; return false; }
    if (!synth_.init(error)) return false;
    SonivoxSynth* synth = &synth_;
    if (!sink_->start(config_, [synth](int16_t* out, size_t frames) { synth->render(out, frames); }, error)) {
        synth_.shutdown();
        if (error.empty()) error = "The audio output for the built-in synthesizer could not be started.";
        return false;
    }
    open_ = true;
    std::lock_guard<std::mutex> guard(g_status_lock);
    g_open_target = this;
    return true;
}

void SonivoxRouteTarget::close()
{
    if (!open_ && !synth_.is_ready()) return;
    {
        std::lock_guard<std::mutex> guard(g_status_lock);
        if (g_open_target == this) g_open_target = nullptr;
    }
    open_ = false;
    if (sink_) sink_->stop();   // returns after the last render call
    synth_.shutdown();
}

bool SonivoxRouteTarget::write(const uint8_t* data, size_t length, std::string&)
{
    synth_.push(data, length);   // a drop is counted inside; playback goes on
    return true;
}

double SonivoxRouteTarget::queue_latency_ms() const
{
    return 1000.0 * static_cast<double>(config_.frames_per_buffer) * config_.buffer_count / config_.sample_rate;
}

std::string SonivoxRouteTarget::status_text() const
{
    const SynthStats s = synth_.stats();
    char text[460];
    std::snprintf(text, sizeof(text),
        "Built-in synth (Sonivox): %d Hz stereo, %d buffers of %.1f ms (%.0f ms queued, plus the phone's own audio delay). "
        "Messages dropped because the send ring was full: %llu writes (%llu bytes); dropped while stopped: %llu; "
        "slow buffers: %llu; longest buffer %llu us; render errors: %llu; voices sounding: %u.",
        config_.sample_rate, config_.buffer_count,
        1000.0 * static_cast<double>(config_.frames_per_buffer) / config_.sample_rate, queue_latency_ms(),
        static_cast<unsigned long long>(s.overflow_writes), static_cast<unsigned long long>(s.overflow_bytes),
        static_cast<unsigned long long>(s.not_running_writes), static_cast<unsigned long long>(s.slow_buffers),
        static_cast<unsigned long long>(s.max_render_us), static_cast<unsigned long long>(s.render_errors),
        static_cast<unsigned>(s.voices));
    return text;
}

std::string SonivoxRouteTarget::describe() const
{
    char text[200];
    std::snprintf(text, sizeof(text), "Built-in synthesizer (Sonivox), about %.0f ms of audio queued.", queue_latency_ms());
    return text;
}

std::unique_ptr<RouteTarget> make_sonivox_route_target()
{
#ifdef __ANDROID__
    SonivoxRouteTarget* target = new SonivoxRouteTarget(make_opensles_audio_sink());
    target->synth().use_settings(&global_synth_settings());   // the Synth settings page; read again at every open
    return std::unique_ptr<RouteTarget>(target);
#else
    return nullptr;
#endif
}

std::string sonivox_route_status_text()
{
    std::lock_guard<std::mutex> guard(g_status_lock);
    return g_open_target ? g_open_target->status_text() : std::string();
}

bool sonivox_route_stats(SynthStats& out)
{
    std::lock_guard<std::mutex> guard(g_status_lock);
    if (!g_open_target) return false;
    out = g_open_target->synth().stats();
    return true;
}

} // namespace android
} // namespace midibn
