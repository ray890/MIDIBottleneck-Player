// Tests of the built-in synthesizer route (Sonivox) with a fake audio sink: no sound card, no JNI, no Android needed.
// Covers: note-on renders sound; note-off and panic decay to silence; reset throws away queued bytes; a full ring drops and
// counts instead of blocking; the send side never waits for the render side; a stress run with a sender thread and a render
// thread; buffer sizes that are not a multiple of the block; SysEx; open/close/reopen; sink failure.
// Built by v2/android/run-native-tests.ps1 (target midibn-synth-tests) and runnable on any host with g++ (see README-dev.md).
#include "route_output.h"
#include "sonivox_route_target.h"
#include "sonivox_synth.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <unistd.h>
#include <thread>
#include <vector>

using namespace midibn;
using namespace midibn::android;
using midibn::platform::OutputStatus;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

typedef std::chrono::steady_clock Clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Pulls audio by hand instead of a sound card.
class FakeSink : public AudioSink {
public:
    RenderFn render;
    bool started = false;
    bool fail_start = false;
    int start_calls = 0, stop_calls = 0;
    AudioConfig config;

    const char* name() const override { return "Fake audio"; }
    bool start(const AudioConfig& c, RenderFn r, std::string& error) override
    {
        ++start_calls;
        if (fail_start) { error = "Fake audio failure."; return false; }
        config = c;
        render = r;
        started = true;
        return true;
    }
    void stop() override { if (started) { started = false; ++stop_calls; } render = nullptr; }
    std::vector<int16_t> pump(size_t frames)
    {
        std::vector<int16_t> out(frames * 2, 1234);   // 1234 = "not written yet"
        if (render) render(out.data(), frames);
        return out;
    }
};

static int peak(const std::vector<int16_t>& samples, size_t from = 0)
{
    int p = 0;
    for (size_t i = from; i < samples.size(); ++i) p = std::max(p, std::abs(static_cast<int>(samples[i])));
    return p;
}

struct Rig {
    FakeSink* sink;
    SonivoxRouteTarget* target;
    std::unique_ptr<BatchingOutput> out;
    explicit Rig(size_t ring_bytes = 65536)
    {
        std::unique_ptr<FakeSink> s(new FakeSink());
        sink = s.get();
        std::unique_ptr<SonivoxRouteTarget> t(new SonivoxRouteTarget(std::move(s), ring_bytes));
        target = t.get();
        out.reset(new BatchingOutput(std::move(t)));
    }
};

static uint32_t pack(unsigned status, unsigned d1, unsigned d2) { return status | (d1 << 8) | (d2 << 16); }

static const size_t kHalfSecond = SonivoxSynth::kSampleRate / 2;

static void test_device_and_open()
{
    Rig r;
    std::vector<platform::OutputDeviceInfo> list = r.out->enumerate();
    CHECK(list.size() == 1 && list[0].name == "Built-in synth (Sonivox)");
    CHECK(std::string(r.out->backend_name()) == "Built-in synthesizer (Sonivox)");
    CHECK(!r.out->is_open());
    OutputStatus st = r.out->open(0);
    CHECK(st.ok());
    CHECK(r.out->is_open() && r.sink->started && r.sink->start_calls == 1);
    CHECK(r.sink->config.sample_rate == 22050 && r.sink->config.channels == 2);
    const double latency = r.target->queue_latency_ms();
    CHECK(latency > 20.0 && latency < 60.0);
    CHECK(r.out->path_description().find("Built-in synthesizer") != std::string::npos);
    CHECK(r.target->status_text().find("Built-in synth (Sonivox)") != std::string::npos);
    CHECK(sonivox_route_status_text().find("buffers of") != std::string::npos);
    r.out->close();
    CHECK(!r.out->is_open() && !r.sink->started && r.sink->stop_calls == 1);
    CHECK(sonivox_route_status_text().empty());
    CHECK(!r.out->open(3).ok());   // no such device
    CHECK(!r.out->is_open());
    std::printf("  latency reported: %.1f ms queued (%d buffers of %zu frames at %d Hz)\n", latency,
        r.sink->config.buffer_count, r.target->audio_config().frames_per_buffer, r.target->audio_config().sample_rate);
}

static void test_note_on_is_audible()
{
    Rig r;
    CHECK(r.out->open(0).ok());
    std::vector<int16_t> quiet = r.sink->pump(2205);
    CHECK(peak(quiet) == 0);   // nothing playing yet
    CHECK(r.out->send_short(pack(0x90, 60, 100)).ok());
    std::vector<int16_t> sound = r.sink->pump(kHalfSecond);
    int p = peak(sound);
    std::printf("  note-on peak: %d\n", p);
    CHECK(p > 500);
    CHECK(r.target->synth().stats().bytes_rendered == 3);
    // batched burst of a chord arrives in one write
    {
        platform::SendBatch batch(r.out.get());
        platform::OutputStatus f;
        r.out->try_send_short(pack(0x90, 64, 100), f);
        r.out->try_send_short(pack(0x90, 67, 100), f);
        r.out->try_send_short(pack(0x99, 38, 110), f);   // drum channel
    }
    CHECK(r.out->write_count() == 2);
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    r.out->close();
}

static void test_note_off_decays_to_silence()
{
    Rig r;
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 0));       // piano
    r.out->send_short(pack(0x90, 60, 100));
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    r.out->send_short(pack(0x80, 60, 0));
    int seconds_to_silence = -1;
    for (int s = 1; s <= 12; ++s) {
        std::vector<int16_t> chunk = r.sink->pump(SonivoxSynth::kSampleRate / 4);
        std::vector<int16_t> chunk2 = r.sink->pump(SonivoxSynth::kSampleRate / 4);
        if (peak(chunk) == 0 && peak(chunk2) == 0) { seconds_to_silence = s; break; }
    }
    std::printf("  after note-off: silent after %d s\n", seconds_to_silence);
    CHECK(seconds_to_silence > 0);
    r.out->close();
}

static void test_panic_silences()
{
    Rig r;
    CHECK(r.out->open(0).ok());
    // Sustained notes on several channels, pedal down.
    r.out->send_short(pack(0xB0, 64, 127));
    for (unsigned ch = 0; ch < 4; ++ch) {
        r.out->send_short(pack(0xC0 | ch, 48 + ch * 8, 0));   // strings, organ-like: sustained
        r.out->send_short(pack(0x90 | ch, 60 + ch, 110));
    }
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    CHECK(r.out->panic().ok());
    // The audio already queued by the sound card cannot be taken back; what the synthesizer renders after the panic
    // fades out within a few milliseconds and is then silent. Measure where the last non-zero frame is.
    std::vector<int16_t> after = r.sink->pump(SonivoxSynth::kSampleRate / 4);
    size_t last_sound = 0;
    for (size_t i = 0; i < after.size(); ++i) if (after[i] != 0) last_sound = i / 2 + 1;
    const double fade_ms = 1000.0 * static_cast<double>(last_sound) / SonivoxSynth::kSampleRate;
    std::printf("  panic: sound stops %.1f ms after the panic bytes are read (last non-zero frame %zu)\n", fade_ms, last_sound);
    CHECK(fade_ms < 60.0);
    std::vector<int16_t> later = r.sink->pump(SonivoxSynth::kSampleRate / 4);
    CHECK(peak(later) == 0);
    std::vector<int16_t> much_later = r.sink->pump(SonivoxSynth::kSampleRate);
    CHECK(peak(much_later) == 0);
    r.out->close();
}

static void test_reset_clears_pending_bytes()
{
    Rig r;
    CHECK(r.out->open(0).ok());
    CHECK(r.out->send_short(pack(0x90, 60, 100)).ok());   // queued for the audio thread, not rendered yet
    CHECK(r.target->synth().pending_bytes() == 3);
    CHECK(r.out->reset().ok());
    CHECK(r.target->synth().pending_bytes() == 0);
    CHECK(peak(r.sink->pump(kHalfSecond)) == 0);          // the note never sounds
    CHECK(r.target->synth().stats().discarded_bytes == 3);
    CHECK(r.target->synth().stats().bytes_rendered == 0);
    // After a reset the output works again, and the panic of reset_and_silence is not discarded.
    CHECK(r.out->send_short(pack(0x90, 62, 100)).ok());
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    CHECK(platform::reset_and_silence(r.out.get()).ok());
    r.sink->pump(256);   // the first buffer after a panic may still carry the end of the note
    CHECK(peak(r.sink->pump(SonivoxSynth::kSampleRate / 4)) == 0);
    r.out->close();
}

static void test_ring_overflow_is_counted_not_blocking()
{
    Rig r(1024);   // 1 KiB ring
    CHECK(r.target->synth().ring_capacity() == 1024);
    CHECK(r.out->open(0).ok());
    const Clock::time_point t0 = Clock::now();
    int failures = 0;
    for (int i = 0; i < 1000; ++i)    // 3000 bytes into a 1024-byte ring while nothing renders
        if (!r.out->send_short(pack(0x90, i & 127, 1 + (i % 100))).ok()) ++failures;
    const double elapsed = ms_since(t0);
    SynthStats s = r.target->synth().stats();
    std::printf("  overflow: %llu writes / %llu bytes dropped, %llu bytes queued, 1000 sends took %.2f ms\n",
        static_cast<unsigned long long>(s.overflow_writes), static_cast<unsigned long long>(s.overflow_bytes),
        static_cast<unsigned long long>(s.bytes_in), elapsed);
    CHECK(failures == 0);                                  // dropping is not an error: playback goes on
    CHECK(s.overflow_writes > 600);
    CHECK(s.bytes_in + s.overflow_bytes == 3000);
    CHECK(s.bytes_in <= 1024 && s.bytes_in >= 1021);
    CHECK(elapsed < 500.0);
    CHECK(r.target->status_text().find("dropped because the send ring was full") != std::string::npos);
    // The render side drains the ring; the next sends are accepted again.
    r.sink->pump(256);
    CHECK(r.target->synth().pending_bytes() == 0);
    const uint64_t before = r.target->synth().stats().overflow_writes;
    CHECK(r.out->send_short(pack(0x90, 60, 100)).ok());
    CHECK(r.target->synth().stats().overflow_writes == before);
    CHECK(r.target->synth().pending_bytes() == 3);
    r.out->close();
}

static void test_write_while_stopped_is_dropped()
{
    std::unique_ptr<FakeSink> s(new FakeSink());
    SonivoxRouteTarget target(std::move(s));
    std::string error;
    const uint8_t note[3] = {0x90, 60, 100};
    CHECK(target.write(note, 3, error));   // not open: dropped, counted, no failure
    CHECK(target.synth().stats().not_running_writes == 1);
    CHECK(target.synth().stats().bytes_in == 0);
}

static void test_send_side_never_waits_for_the_render_side()
{
    SonivoxSynth synth(65536);
    std::string error;
    CHECK(synth.init(error));
    std::atomic<bool> in_render(false), render_done(false);
    synth.set_render_hook([&] {
        in_render.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));   // the render side holds its lock for 400 ms
    });
    std::thread render_thread([&] {
        std::vector<int16_t> out(256 * 2);
        synth.render(out.data(), 256);
        render_done.store(true);
    });
    while (!in_render.load()) std::this_thread::yield();
    double worst = 0, total_ms = 0;
    const Clock::time_point t0 = Clock::now();
    const uint8_t note[3] = {0x90, 60, 100};
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        const Clock::time_point c0 = Clock::now();
        if (synth.push(note, 3)) ++accepted;
        if (i % 1000 == 0) synth.discard_pending();
        worst = std::max(worst, ms_since(c0));
    }
    total_ms = ms_since(t0);
    const bool still_rendering = !render_done.load();
    std::printf("  while the render side held its lock: 20000 pushes took %.2f ms, slowest %.3f ms\n", total_ms, worst);
    CHECK(still_rendering);          // the pushes finished while render() was still inside its locked region
    CHECK(worst < 20.0);
    CHECK(total_ms < 200.0);
    CHECK(accepted > 0);
    render_thread.join();
    synth.set_render_hook(nullptr);
    synth.shutdown();
}

static void test_stress_sender_and_renderer()
{
    SonivoxSynth synth(4096);   // small ring: some overflow is expected
    std::string error;
    CHECK(synth.init(error));
    std::atomic<bool> stop(false);
    std::thread renderer([&] {
        std::vector<int16_t> out(256 * 2);
        while (!stop.load()) {
            synth.render(out.data(), 256);
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    });
    uint64_t produced = 0;
    for (int i = 0; i < 100000; ++i) {
        const uint8_t msg[3] = {static_cast<uint8_t>(0x90 | (i & 15)), static_cast<uint8_t>(30 + (i % 60)), static_cast<uint8_t>(i % 2 ? 0 : 90)};
        synth.push(msg, 3);
        produced += 3;
        if (i % 20000 == 19999) synth.discard_pending();
        if (i % 40 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop.store(true);
    renderer.join();
    std::vector<int16_t> out(256 * 2);
    synth.render(out.data(), 256);   // final drain
    SynthStats s = synth.stats();
    std::printf("  stress: produced %llu bytes, accepted %llu, dropped %llu, discarded %llu, rendered %llu, blocks %llu\n",
        static_cast<unsigned long long>(produced), static_cast<unsigned long long>(s.bytes_in),
        static_cast<unsigned long long>(s.overflow_bytes), static_cast<unsigned long long>(s.discarded_bytes),
        static_cast<unsigned long long>(s.bytes_rendered), static_cast<unsigned long long>(s.blocks_rendered));
    CHECK(s.bytes_in + s.overflow_bytes == produced);
    CHECK(s.bytes_rendered + s.discarded_bytes == s.bytes_in);
    CHECK(s.render_errors == 0);
    synth.shutdown();
}

static void test_odd_buffer_sizes_match_one_big_render()
{
    std::string error;
    SonivoxSynth a, b;
    CHECK(a.init(error) && b.init(error));
    const uint8_t chord[] = {0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100};
    a.push(chord, sizeof(chord));
    b.push(chord, sizeof(chord));
    const size_t total = 3000;
    std::vector<int16_t> whole(total * 2);
    b.render(whole.data(), total);   // 3000 = 23 blocks and 56 frames: the carry buffer
    std::vector<int16_t> pieces;
    const size_t sizes[] = {1, 99, 300, 128, 7, 1000, 250, 1215};   // sums to 3000
    size_t sum = 0;
    for (size_t n : sizes) {
        std::vector<int16_t> part(n * 2);
        a.render(part.data(), n);
        pieces.insert(pieces.end(), part.begin(), part.end());
        sum += n;
    }
    CHECK(sum == total);
    CHECK(pieces == whole);
    CHECK(peak(whole) > 500);
    a.shutdown();
    b.shutdown();
}

static void test_sysex_and_reopen()
{
    Rig r;
    CHECK(r.out->open(0).ok());
    const std::vector<uint8_t> gm_reset = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};   // General MIDI System On (the data includes F0, as the JNI layer passes it)
    output::SystemExclusiveEvent e;
    e.status = 0xF0;
    e.data = gm_reset.data();
    e.length = gm_reset.size();
    CHECK(r.out->send_long(e).ok());
    r.out->send_short(pack(0x90, 60, 100));
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    CHECK(r.target->synth().stats().render_errors == 0);
    CHECK(r.target->synth().stats().bytes_rendered == gm_reset.size() + 3);
    // Close and open again: a fresh synthesizer, a clean ring, the sink restarted.
    CHECK(r.out->open(0).ok());
    CHECK(r.sink->start_calls == 2);
    CHECK(peak(r.sink->pump(2205)) == 0);
    CHECK(r.target->synth().stats().bytes_in == 0);
    r.out->send_short(pack(0x90, 60, 100));
    CHECK(peak(r.sink->pump(kHalfSecond)) > 500);
    r.out->close();
    r.out->close();   // twice is fine
}

static void test_sink_failure()
{
    Rig r;
    r.sink->fail_start = true;
    OutputStatus st = r.out->open(0);
    CHECK(!st.ok());
    CHECK(st.message.find("Fake audio failure") != std::string::npos);
    CHECK(!r.out->is_open());
    CHECK(!r.target->synth().is_ready());   // the synthesizer was shut down again
    r.sink->fail_start = false;
    CHECK(r.out->open(0).ok());
    r.out->close();
}

// ---- synth settings (volume, voice limit, reverb) ---------------------------------------------------------------------

static double energy(const std::vector<int16_t>& samples, size_t from = 0)
{
    double e = 0;
    for (size_t i = from; i < samples.size(); ++i) e += static_cast<double>(samples[i]) * samples[i];
    return e;
}

// One sustained note (piano), half a second of sound; returns its peak. `configure` runs before the synth opens.
template <class F>
static int peak_of_note(F configure)
{
    Rig r;
    configure(r.target->synth().settings());
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 0));
    r.out->send_short(pack(0x90, 60, 100));
    r.sink->pump(2205);
    const int p = peak(r.sink->pump(kHalfSecond));
    r.out->close();
    return p;
}

static void test_settings_clamp_and_reset()
{
    SynthSettings s;
    s.set_volume_percent(-5);
    CHECK(s.volume_percent == 0);
    s.set_volume_percent(900);
    CHECK(s.volume_percent == SynthSettings::kVolumeMax);
    s.set_voice_limit(0);
    CHECK(s.voice_limit == 1);
    s.set_reverb_preset(9);
    CHECK(s.reverb_preset == SynthSettings::kPresetCount - 1);
    s.set_reverb_amount(400);
    CHECK(s.reverb_amount == 100);
    const uint32_t before = s.version;
    s.set_reverb_on(true);
    CHECK(s.version != before && s.reverb_on);
    s.reset();
    CHECK(s.voice_limit == SynthSettings::kDefaultVoices && SynthSettings::kDefaultVoices == 64);
    CHECK(s.volume_percent == 100 && !s.reverb_on && s.reverb_preset == 1 && s.reverb_amount == 85);
    CHECK(SonivoxSynth::max_voices() >= 64);   // the build maximum (4096 in the Android build); the synth still opens at 64
    std::printf("  build maximum of voices: %d (default at open: %d)\n", SonivoxSynth::max_voices(), SynthSettings::kDefaultVoices);
    CHECK(&global_synth_settings() == &global_synth_settings());
}

static void test_volume()
{
    const int p100 = peak_of_note([](SynthSettings&) {});
    const int p50 = peak_of_note([](SynthSettings& s) { s.set_volume_percent(50); });
    const int p10 = peak_of_note([](SynthSettings& s) { s.set_volume_percent(10); });
    const int p0 = peak_of_note([](SynthSettings& s) { s.set_volume_percent(0); });
    const int p200 = peak_of_note([](SynthSettings& s) { s.set_volume_percent(200); });
    std::printf("  note peak by volume: 0%%=%d 10%%=%d 50%%=%d 100%%=%d 200%%=%d\n", p0, p10, p50, p100, p200);
    CHECK(p100 > 500);
    CHECK(p0 == 0);                 // set before the synth opened: applies on open
    CHECK(p10 > 0 && p10 < p50);
    CHECK(p50 < p100);
    CHECK(p50 > p100 * 4 / 10 && p50 < p100 * 6 / 10);   // about half
    CHECK(p200 > p100);
    // Live: a sustained note, then the slider goes to 0 and back while the audio runs.
    Rig r;
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 48));   // strings: sustained
    r.out->send_short(pack(0x90, 60, 100));
    r.sink->pump(2205);
    CHECK(peak(r.sink->pump(2205)) > 500);
    r.target->synth().settings().set_volume_percent(0);
    r.sink->pump(512);                       // the render thread picks it up and ramps down within one buffer
    CHECK(peak(r.sink->pump(2205)) == 0);
    r.target->synth().settings().set_volume_percent(100);
    r.sink->pump(512);
    CHECK(peak(r.sink->pump(2205)) > 500);
    CHECK(r.target->synth().stats().render_errors == 0);
    // The ramp: right after a drop to 0 the first buffer fades, it does not cut at once.
    r.target->synth().settings().set_volume_percent(0);
    std::vector<int16_t> fade = r.sink->pump(256);
    CHECK(fade[0] != 0 || fade[1] != 0 || fade[2] != 0 || fade[3] != 0);
    CHECK(fade[2 * 255] == 0 && fade[2 * 255 + 1] == 0);
    // The setting survives close and open (it belongs to the settings object, not to the Sonivox instance).
    r.out->close();
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0x90, 60, 100));
    CHECK(peak(r.sink->pump(kHalfSecond)) == 0);
    r.out->close();
}

static void test_voice_limit()
{
    // A six-note chord on a sustained sound.
    struct Result { uint32_t voices; int peak; };
    auto chord = [](int limit) {
        Rig r;
        if (limit > 0) r.target->synth().settings().set_voice_limit(limit);
        CHECK(r.out->open(0).ok());
        r.out->send_short(pack(0xC0, 0, 48));
        const int notes[6] = {48, 52, 55, 60, 64, 67};
        for (int n : notes) r.out->send_short(pack(0x90, n, 100));
        r.sink->pump(2205);
        Result res;
        res.peak = peak(r.sink->pump(kHalfSecond));
        res.voices = r.target->synth().stats().voices;
        CHECK(r.target->synth().stats().render_errors == 0);
        r.out->close();
        return res;
    };
    const Result all = chord(0);
    const Result one = chord(1);
    const Result two = chord(2);
    const Result huge = chord(100000);   // above the build maximum: clamped, no error
    std::printf("  chord: all=%u voices (peak %d), limit 1=%u (peak %d), limit 2=%u (peak %d), limit 100000=%u\n",
        all.voices, all.peak, one.voices, one.peak, two.voices, two.peak, huge.voices);
    CHECK(all.voices >= 6);
    CHECK(one.voices == 1);
    CHECK(two.voices == 2);
    CHECK(huge.voices >= 6);
    CHECK(one.peak > 0 && one.peak < all.peak);
    // Live: lowering the limit while the chord sounds cuts the voices at once; raising it again allows new notes.
    Rig r;
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 48));
    const int notes[6] = {48, 52, 55, 60, 64, 67};
    for (int n : notes) r.out->send_short(pack(0x90, n, 100));
    r.sink->pump(2205);
    CHECK(r.target->synth().stats().voices >= 6);
    r.target->synth().settings().set_voice_limit(2);
    r.sink->pump(2205);                      // the extra voices fade out (a few milliseconds) and are freed
    CHECK(r.target->synth().stats().voices <= 2);
    r.target->synth().settings().set_voice_limit(64);
    r.out->send_short(pack(0x90, 72, 100));
    r.out->send_short(pack(0x90, 76, 100));
    r.out->send_short(pack(0x90, 79, 100));
    r.sink->pump(2205);
    CHECK(r.target->synth().stats().voices >= 4);
    r.out->close();
}

// The sound after a short chord was released: where reverb shows.
static std::vector<int16_t> release_tail(bool reverb, int preset, int amount)
{
    Rig r;
    SynthSettings& s = r.target->synth().settings();
    s.set_reverb_on(reverb);
    s.set_reverb_preset(preset);
    s.set_reverb_amount(amount);
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 0));
    r.out->send_short(pack(0x90, 60, 110));
    r.out->send_short(pack(0x90, 64, 110));
    r.out->send_short(pack(0x90, 67, 110));
    r.sink->pump(kHalfSecond);
    r.out->send_short(pack(0x80, 60, 0));
    r.out->send_short(pack(0x80, 64, 0));
    r.out->send_short(pack(0x80, 67, 0));
    r.sink->pump(SonivoxSynth::kSampleRate / 2);   // the note itself dies away
    std::vector<int16_t> tail = r.sink->pump(SonivoxSynth::kSampleRate);
    CHECK(r.target->synth().stats().render_errors == 0);
    r.out->close();
    return tail;
}

static void test_reverb()
{
    const std::vector<int16_t> dry = release_tail(false, 1, 100);
    const std::vector<int16_t> wet = release_tail(true, 1, 100);
    const std::vector<int16_t> wet_low = release_tail(true, 1, 20);
    const std::vector<int16_t> wet_zero = release_tail(true, 1, 0);
    const std::vector<int16_t> large = release_tail(true, 0, 100);
    const std::vector<int16_t> room = release_tail(true, 3, 100);
    const double e_dry = energy(dry), e_wet = energy(wet), e_low = energy(wet_low), e_large = energy(large), e_room = energy(room);
    std::printf("  tail energy (1 s after the chord died away): off=%.3g hall100=%.3g hall20=%.3g hall0=%.3g largehall100=%.3g room100=%.3g\n",
        e_dry, e_wet, e_low, energy(wet_zero), e_large, e_room);
    CHECK(e_wet > e_dry * 2 + 1000.0);          // reverb on: a clearly longer tail
    CHECK(wet != dry);
    CHECK(e_low > e_dry && e_low < e_wet);      // a smaller amount: in between
    CHECK(wet_zero == dry);                     // amount 0 = nothing added
    CHECK(large != wet && room != wet && large != room);   // the presets differ
    CHECK(e_large > e_room);                    // a large hall rings longer than a room
    // Live: switch the reverb on while a chord is released, and off again.
    Rig r;
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0x90, 60, 110));
    r.sink->pump(kHalfSecond);
    r.out->send_short(pack(0x80, 60, 0));
    r.sink->pump(SonivoxSynth::kSampleRate / 2);
    r.target->synth().settings().set_reverb_on(true);
    r.target->synth().settings().set_reverb_amount(100);
    r.sink->pump(1024);
    CHECK(r.target->synth().stats().render_errors == 0);
    r.target->synth().settings().set_reverb_on(false);
    r.sink->pump(1024);
    CHECK(r.target->synth().stats().render_errors == 0);
    r.out->close();
}

// ---- clipping and the volume headroom ---------------------------------------------------------------------------------------

struct ClipRun {
    uint64_t clipped_samples;
    uint32_t meter_peak;
    int audio_peak;
    int longest_flat;       // most consecutive samples (one channel) at the same level within 0.1 percent of the peak
};

// A loud six-note chord (velocity 127) on sustained strings, as in a dense passage; half a second after the attack.
static ClipRun clip_run(int percent)
{
    Rig r;
    r.target->synth().settings().set_volume_percent(percent);
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 48));
    const int notes[6] = {48, 52, 55, 60, 64, 67};
    for (int n : notes) r.out->send_short(pack(0x90, n, 127));
    r.sink->pump(2205);
    const SynthStats before = r.target->synth().stats();
    std::vector<int16_t> audio = r.sink->pump(kHalfSecond);
    const SynthStats after = r.target->synth().stats();
    ClipRun run;
    run.clipped_samples = after.clipped_samples - before.clipped_samples;
    run.meter_peak = after.peak;
    run.audio_peak = peak(audio);
    run.longest_flat = 0;
    int streak = 0;
    for (size_t i = 0; i < audio.size(); i += 2) {
        if (run.audio_peak > 0 && std::abs(static_cast<int>(audio[i])) * 1000 >= run.audio_peak * 999) ++streak; else streak = 0;
        run.longest_flat = std::max(run.longest_flat, streak);
    }
    CHECK(after.render_errors == 0);
    r.out->close();
    return run;
}

static void test_clipping_and_headroom()
{
    const ClipRun c200 = clip_run(200);
    const ClipRun c100 = clip_run(100);
    const ClipRun c70 = clip_run(70);
    const ClipRun c50 = clip_run(50);
    const ClipRun c25 = clip_run(25);
    std::printf("  loud chord, clipped samples / audio peak / longest flat top: 200%%=%llu/%d/%d 100%%=%llu/%d/%d 70%%=%llu/%d/%d 50%%=%llu/%d/%d 25%%=%llu/%d/%d\n",
        (unsigned long long)c200.clipped_samples, c200.audio_peak, c200.longest_flat, (unsigned long long)c100.clipped_samples, c100.audio_peak, c100.longest_flat,
        (unsigned long long)c70.clipped_samples, c70.audio_peak, c70.longest_flat, (unsigned long long)c50.clipped_samples, c50.audio_peak, c50.longest_flat,
        (unsigned long long)c25.clipped_samples, c25.audio_peak, c25.longest_flat);
    CHECK(c100.clipped_samples > 0);            // the chord is too loud for the normal level: Sonivox saturates
    CHECK(c100.longest_flat >= 3);              // and the waveform is flat-topped
    CHECK(c200.clipped_samples > c100.clipped_samples);   // clipping returns (and grows) above 100 percent
    // This chord is about 2.4 times full scale at the normal level, so 50 percent is only just short of enough room; 35 is clear.
    CHECK(c50.clipped_samples * 20 < c100.clipped_samples);   // 50 percent: far fewer clipped samples ...
    CHECK(c50.longest_flat < c100.longest_flat);
    const ClipRun c35 = clip_run(35);
    std::printf("  35%%=%llu/%d/%d\n", (unsigned long long)c35.clipped_samples, c35.audio_peak, c35.longest_flat);
    CHECK(c35.clipped_samples == 0);            // ... and with more headroom none at all
    CHECK(c35.audio_peak > 1000 && c35.audio_peak < 32767);
    CHECK(c35.longest_flat < 3);                // the waveform is not flat-topped
    CHECK(c25.clipped_samples == 0 && c25.audio_peak < c35.audio_peak);
    CHECK(c70.clipped_samples < c100.clipped_samples);
    CHECK(c100.meter_peak >= 32000 && c50.meter_peak > 1000 && c50.meter_peak <= 32768);   // the meter follows the audio
    // Counters are per synth session and count buffers.
    Rig r;
    CHECK(r.out->open(0).ok());
    r.sink->pump(2205);
    CHECK(r.target->synth().stats().clipped_buffers == 0 && r.target->synth().stats().clipped_samples == 0);
    CHECK(r.target->synth().stats().peak == 0);
    r.out->send_short(pack(0xC0, 0, 48));
    for (int n : {48, 52, 55, 60, 64, 67}) r.out->send_short(pack(0x90, n, 127));
    r.sink->pump(kHalfSecond);
    CHECK(r.target->synth().stats().clipped_buffers > 0);
    CHECK(r.out->panic().ok());
    for (int i = 0; i < 172; ++i) r.sink->pump(256);   // two seconds in buffers of the real size (the meter is per buffer)
    std::printf("  peak hold two seconds after a panic: %u\n", r.target->synth().stats().peak);
    CHECK(r.target->synth().stats().peak < 100);   // the peak hold falls back
    r.out->close();
    // The volume steps the player uses (Sonivox master volume + fine gain) are continuous: no jump while the slider moves.
    auto largest_step = [](bool sweep) {
        Rig live;
        CHECK(live.out->open(0).ok());
        live.out->send_short(pack(0xC0, 0, 48));
        live.out->send_short(pack(0x90, 60, 60));
        live.sink->pump(4410);
        int worst = 0;
        for (int percent = 100; percent >= 40; --percent) {   // 61 settings, one per buffer: crosses several whole-dB steps
            if (sweep) live.target->synth().settings().set_volume_percent(percent);
            std::vector<int16_t> a = live.sink->pump(256);
            for (size_t i = 2; i + 2 < a.size(); i += 2) worst = std::max(worst, std::abs(static_cast<int>(a[i]) - a[i - 2]));
        }
        CHECK(live.target->synth().stats().render_errors == 0);
        live.out->close();
        return worst;
    };
    const int natural = largest_step(false), swept = largest_step(true);
    std::printf("  volume sweep 100 to 40 percent: largest sample-to-sample step %d (same note, no sweep: %d)\n", swept, natural);
    CHECK(swept <= natural + natural / 4);   // the steps of Sonivox's master volume cause no jump
}

// ---- DLS sound banks ----------------------------------------------------------------------------------------------------------

static void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xffff); put16(v, x >> 16); }
static void putid(std::vector<uint8_t>& v, const char* id) { v.insert(v.end(), id, id + 4); }
static std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> v;
    putid(v, id);
    put32(v, static_cast<uint32_t>(body.size()));
    v.insert(v.end(), body.begin(), body.end());
    if (body.size() & 1) v.push_back(0);
    return v;
}
static std::vector<uint8_t> list(const char* form, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> inner;
    putid(inner, form);
    inner.insert(inner.end(), body.begin(), body.end());
    return chunk("LIST", inner);
}
static void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& more) { v.insert(v.end(), more.begin(), more.end()); }

// A minimal valid DLS bank written by the test (no third-party file): one instrument (bank 0, program 0, all keys), one
// sample (441 Hz sine at 22,050 Hz, a second long, unity note 60), one articulation. `rate_hz` is the pitch of the sine.
static std::vector<uint8_t> make_test_dls(int program = 0, int bank = 0)
{
    std::vector<uint8_t> body;
    std::vector<uint8_t> t;
    put32(t, 1);
    append(body, chunk("colh", t));
    // instrument
    std::vector<uint8_t> ins;
    t.clear(); put32(t, 1); put32(t, static_cast<uint32_t>(bank)); put32(t, static_cast<uint32_t>(program));
    append(ins, chunk("insh", t));
    std::vector<uint8_t> rgn;
    t.clear(); put16(t, 0); put16(t, 127); put16(t, 0); put16(t, 127); put16(t, 0); put16(t, 0);   // rgnh: keys 0-127, velocities 0-127
    append(rgn, chunk("rgnh", t));
    t.clear(); put16(t, 0); put16(t, 0); put32(t, 1); put32(t, 0);   // wlnk: wave number 0
    append(rgn, chunk("wlnk", t));
    append(ins, list("lrgn", list("rgn ", rgn)));
    std::vector<uint8_t> art;
    t.clear(); put32(t, 8); put32(t, 1);                      // art1: one connection: EG1 release time
    put16(t, 0); put16(t, 0); put16(t, 0x0209); put16(t, 0); put32(t, static_cast<uint32_t>(-2000) << 16);
    append(art, chunk("art1", t));
    append(ins, list("lart", art));
    append(body, list("lins", list("ins ", ins)));
    // pool table and wave pool
    t.clear(); put32(t, 8); put32(t, 1); put32(t, 0);
    append(body, chunk("ptbl", t));
    std::vector<uint8_t> wave;
    t.clear(); put16(t, 1); put16(t, 1); put32(t, 22050); put32(t, 44100); put16(t, 2); put16(t, 16);   // fmt: 16-bit mono PCM
    append(wave, chunk("fmt ", t));
    t.clear(); put32(t, 20); put16(t, 60); put16(t, 0); put32(t, 0); put32(t, 0); put32(t, 0);          // wsmp: unity note 60, no loop
    append(wave, chunk("wsmp", t));
    t.clear();
    for (int i = 0; i < 22050; ++i) put16(t, static_cast<unsigned>(static_cast<int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979 * 441.0 * i / 22050.0)))) & 0xffff);
    append(wave, chunk("data", t));
    append(body, list("wvpl", list("wave", wave)));
    // a trailing info list (Sonivox's parser reads a chunk header past the end of some lists)
    t.clear(); putid(t, "INAM"); put32(t, 4); putid(t, "test");
    append(body, list("INFO", std::vector<uint8_t>(t.begin() + 0, t.end())));
    std::vector<uint8_t> file;
    putid(file, "RIFF");
    put32(file, static_cast<uint32_t>(body.size() + 4));
    putid(file, "DLS ");
    append(file, body);
    return file;
}

// Zero crossings of the left channel between 0.05 s and 0.30 s after the key went down: a measure of pitch.
static int crossings(const std::vector<int16_t>& stereo)
{
    int count = 0;
    const size_t from = 1103, to = 6615;   // frames
    for (size_t f = from + 1; f < to && f * 2 < stereo.size(); ++f)
        if ((stereo[(f - 1) * 2] < 0) != (stereo[f * 2] < 0)) ++count;
    return count;
}

static int note_crossings(Rig& r, int note)
{
    r.out->send_short(pack(0x90, note, 100));
    r.sink->pump(256);                   // the render thread installs a changed bank first, then reads the note
    std::vector<int16_t> a;
    {
        std::vector<int16_t> b = r.sink->pump(6615);
        a = b;
    }
    r.out->send_short(pack(0x80, note, 0));
    r.sink->pump(8820);
    return crossings(a);
}

static void test_dls()
{
    const std::vector<uint8_t> bank = make_test_dls();
    Rig r;
    SynthSettings& s = r.target->synth().settings();
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 0));
    std::string error;
    // 1. Without a bank: the built-in piano. (261 Hz: about 130 crossings in the window; plus overtones.)
    CHECK(!s.dls.loaded() && s.dls.name().empty());
    const int builtin60 = note_crossings(r, 60);
    // 2. Load the bank while the synth plays: the render thread installs it.
    CHECK(s.dls.load_memory(bank.data(), bank.size(), "test-bank.dls", error));
    CHECK(error.empty() && s.dls.loaded() && s.dls.name() == "test-bank.dls");
    const int dls60 = note_crossings(r, 60);
    const int dls72 = note_crossings(r, 72);
    std::printf("  zero crossings per 0.25 s: built-in piano note 60 = %d; test bank (441 Hz sine) note 60 = %d, note 72 = %d (expected about 220 and 441)\n",
        builtin60, dls60, dls72);
    CHECK(dls60 > 200 && dls60 < 240);
    CHECK(dls72 > 400 && dls72 < 480);        // an octave up
    CHECK(std::abs(builtin60 - dls60) > 20 || std::abs(builtin60 - 220) > 20);
    // The channel kept its settings through the swap (program 0 was set before the bank came).
    CHECK(r.target->synth().stats().render_errors == 0);
    // 3. Not a DLS file, empty, truncated, too large: plain error, the loaded bank stays.
    std::vector<uint8_t> junk(5000);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 37 + 11);
    CHECK(!s.dls.load_memory(junk.data(), junk.size(), "junk", error));
    CHECK(error == "This file is not a DLS sound bank.");
    CHECK(!s.dls.load_memory(junk.data(), 0, "empty", error) && !error.empty());
    std::vector<uint8_t> wave(bank.begin(), bank.begin() + 12);
    wave[8] = 'W'; wave[9] = 'A'; wave[10] = 'V'; wave[11] = 'E';
    CHECK(!s.dls.load_memory(wave.data(), wave.size(), "wave", error) && error == "This file is not a DLS sound bank.");
    std::vector<uint8_t> cut(bank.begin(), bank.begin() + bank.size() / 2);
    CHECK(!s.dls.load_memory(cut.data(), cut.size(), "cut", error));
    std::printf("  truncated bank: \"%s\"\n", error.c_str());
    CHECK(!error.empty());
    CHECK(!s.dls.load_memory(junk.data(), static_cast<size_t>(DlsBank::kMaxFileBytes) + 1, "huge", error));
    CHECK(error.find("too large") != std::string::npos);
    std::vector<uint8_t> wrong = bank;       // a DLS header with the sample format changed to something Sonivox cannot play
    for (size_t i = 0; i + 8 < wrong.size(); ++i)
        if (std::memcmp(&wrong[i], "fmt ", 4) == 0) { wrong[i + 8] = 3; break; }   // float format
    CHECK(!s.dls.load_memory(wrong.data(), wrong.size(), "float", error));
    CHECK(error.find("cannot use") != std::string::npos);
    CHECK(s.dls.loaded() && s.dls.name() == "test-bank.dls");
    CHECK(std::abs(note_crossings(r, 72) - dls72) < 30);   // still the bank
    // 4. Back to the built-in sounds.
    s.dls.clear();
    CHECK(!s.dls.loaded() && s.dls.name().empty());
    const int back60 = note_crossings(r, 60);
    CHECK(std::abs(back60 - builtin60) < 12);
    CHECK(r.target->synth().stats().render_errors == 0);
    // 5. A bank loaded before the synth opens is used from the first note; closing releases it from the stream.
    CHECK(s.dls.load_memory(bank.data(), bank.size(), "again.dls", error));
    r.out->close();
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 0));
    CHECK(note_crossings(r, 60) > 200);
    r.out->close();
    CHECK(s.dls.loaded());                   // the holder keeps the bank for the next open
    // 6. Through a file descriptor (a temporary file), the way the phone reads the file.
    FILE* f = std::tmpfile();
    if (f) {
        CHECK(std::fwrite(bank.data(), 1, bank.size(), f) == bank.size());
        std::fflush(f);
        s.dls.clear();
        DlsBank::LoadResult res = s.dls.load_fd(fileno(f), "from-fd.dls", error);
        CHECK(res == DlsBank::kLoaded && s.dls.name() == "from-fd.dls");
        // a descriptor that cannot be read at an offset (a pipe) is reported so the caller can copy it first
        int p[2];
        if (pipe(p) == 0) {
            CHECK(s.dls.load_fd(p[0], "pipe", error) == DlsBank::kNotSeekable);
            close(p[0]); close(p[1]);
        }
        std::fclose(f);
    } else {
        std::printf("  (no temporary file here; the file-descriptor load was not tested)\n");
    }
    s.dls.clear();
    // 7. Drum and bank mapping: a bank whose instrument says bank 0x7900 (as Sonivox itself names it) works as well.
    const std::vector<uint8_t> prog5 = make_test_dls(5, 0);
    CHECK(s.dls.load_memory(prog5.data(), prog5.size(), "p5.dls", error));
    CHECK(r.out->open(0).ok());
    r.out->send_short(pack(0xC0, 0, 5));
    CHECK(note_crossings(r, 60) > 200);      // program 5 is in the bank
    r.out->send_short(pack(0xC0, 0, 6));
    CHECK(note_crossings(r, 60) < 200 || note_crossings(r, 60) > 240);   // program 6 is not: the built-in sound
    r.out->close();
    s.dls.clear();
}

// The loader thread publishes and removes banks while the render thread plays notes: no race (run under ThreadSanitizer),
// no render error, and everything is released in the end.
static void test_dls_load_while_playing()
{
    const std::vector<uint8_t> bank = make_test_dls();
    Rig r;
    SynthSettings& s = r.target->synth().settings();
    CHECK(r.out->open(0).ok());
    std::atomic<bool> stop(false);
    std::atomic<int> loads(0);
    std::thread loader([&]() {
        std::string error;
        for (int i = 0; i < 40 && !stop.load(); ++i) {
            if (s.dls.load_memory(bank.data(), bank.size(), "loop.dls", error)) ++loads;
            if (i % 3 == 2) s.dls.clear();
        }
    });
    int rendered = 0;
    while (loads.load() < 40 && rendered < 20000) {
        r.out->send_short(pack(0x90, 48 + rendered % 24, 100));
        r.sink->pump(256);
        r.out->send_short(pack(0x80, 48 + rendered % 24, 0));
        ++rendered;
    }
    stop = true;
    loader.join();
    r.sink->pump(2048);
    std::printf("  loaded %d banks while %d buffers were played\n", loads.load(), rendered);
    CHECK(loads.load() > 0);
    CHECK(r.target->synth().stats().render_errors == 0);
    r.out->close();
    s.dls.clear();
}

// ---- information: how long one buffer takes with many voices sounding ------------------------------------------------------

static void benchmark()
{
    const int frames = 2 * SonivoxSynth::kBlockFrames;   // the buffer the OpenSL ES sink uses: 256 frames
    const double buffer_ms = 1000.0 * frames / SonivoxSynth::kSampleRate;
    std::printf("INFORMATION (never a failure): time to render one %d-frame buffer (%.2f ms of audio at %d Hz), single thread, "
                "sustained notes on 15 channels with different instruments, drums excluded\n", frames, buffer_ms, SonivoxSynth::kSampleRate);
    const int counts[] = {64, 128, 256, 512, 1024, 2048, 4096};
    static const int programs[15] = {19, 48, 49, 52, 53, 54, 56, 57, 61, 68, 71, 73, 80, 81, 89};
    for (int n : counts) {
        if (n > SonivoxSynth::max_voices()) {
            std::printf("  N=%4d: skipped (this build allows at most %d voices)\n", n, SonivoxSynth::max_voices());
            continue;
        }
        Rig r(1 << 17);
        r.target->synth().settings().set_voice_limit(n);
        if (!r.out->open(0).ok()) { std::printf("  N=%4d: could not open the synth\n", n); continue; }
        int channels[15], c = 0;
        for (int ch = 0; ch < 16; ++ch) if (ch != 9) channels[c++] = ch;
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 15; ++i) { bytes.push_back(static_cast<uint8_t>(0xC0 | channels[i])); bytes.push_back(static_cast<uint8_t>(programs[i])); }
        const int wanted = n + n / 3 + 16;
        int sent = 0;
        for (int pass = 0; pass < 3 && sent < wanted; ++pass)
            for (int note = 12; note < 128 && sent < wanted; ++note)
                for (int i = 0; i < 15 && sent < wanted; ++i, ++sent) {
                    bytes.push_back(static_cast<uint8_t>(0x90 | channels[i]));
                    bytes.push_back(static_cast<uint8_t>(note));
                    bytes.push_back(static_cast<uint8_t>(40 + (note + i) % 40));
                }
        for (size_t at = 0; at < bytes.size(); at += 30000)
            r.target->synth().push(bytes.data() + at, std::min<size_t>(30000, bytes.size() - at));
        std::vector<int16_t> out(static_cast<size_t>(frames) * 2);
        for (int i = 0; i < 12; ++i) r.sink->render(out.data(), frames);   // the notes start; warm-up
        const int measured = 100;
        double total = 0, worst = 0;
        for (int i = 0; i < measured; ++i) {
            const Clock::time_point t = Clock::now();
            r.sink->render(out.data(), frames);
            const double ms = ms_since(t);
            total += ms;
            worst = std::max(worst, ms);
        }
        const SynthStats st = r.target->synth().stats();
        const double avg = total / measured;
        std::printf("  N=%4d: %5u voices sounding, average %8.3f ms, worst %8.3f ms per buffer = %7.1f %% (average) and %7.1f %% (worst) of real time%s\n",
            n, st.voices, avg, worst, 100.0 * avg / buffer_ms, 100.0 * worst / buffer_ms, st.render_errors ? "  (render errors!)" : "");
        r.out->close();
    }
    std::printf("  (over 100 %% means this many voices cannot be played in real time on this machine)\n");
}

int main(int argc, char** argv)
{
    bool bench = true, only_bench = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-bench") == 0) bench = false;
        else if (std::strcmp(argv[i], "--bench") == 0) only_bench = true;
    }
    if (only_bench) {
        std::printf("synth benchmark (Sonivox, fake audio)\n");
        benchmark();
        return 0;
    }
    std::printf("synth tests (Sonivox, fake audio)\n");
    test_clipping_and_headroom();
    test_dls();
    test_dls_load_while_playing();
    test_settings_clamp_and_reset();
    test_volume();
    test_voice_limit();
    test_reverb();
    test_device_and_open();
    test_note_on_is_audible();
    test_note_off_decays_to_silence();
    test_panic_silences();
    test_reset_clears_pending_bytes();
    test_ring_overflow_is_counted_not_blocking();
    test_write_while_stopped_is_dropped();
    test_send_side_never_waits_for_the_render_side();
    test_stress_sender_and_renderer();
    test_odd_buffer_sizes_match_one_big_render();
    test_sysex_and_reopen();
    test_sink_failure();
    if (bench) benchmark();
    if (g_failures == 0) {
        std::printf("RESULT: PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("RESULT: FAIL (%d of %d checks)\n", g_failures, g_checks);
    return 1;
}
