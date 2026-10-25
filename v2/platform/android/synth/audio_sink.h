// Where the built-in synthesizer's audio goes. The real sink is OpenSL ES (opensles_audio_sink.cpp, Android only);
// the tests use a fake that pulls audio by hand. The sink owns the audio thread: it calls `render` from there, once per
// buffer, and stop() returns only after the last call has finished.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace midibn {
namespace android {

struct AudioConfig {
    int sample_rate = 22050;
    int channels = 2;
    size_t frames_per_buffer = 256;   // one buffer of the queue
    int buffer_count = 3;             // buffers queued ahead
};

class AudioSink {
public:
    typedef std::function<void(int16_t* out, size_t frames)> RenderFn;   // interleaved 16-bit

    virtual ~AudioSink() {}
    virtual const char* name() const = 0;
    // Starts playback; `render` is called from the sink's audio thread. False with a plain sentence in `error`.
    virtual bool start(const AudioConfig& config, RenderFn render, std::string& error) = 0;
    // Stops playback and waits until `render` is no longer running. Safe to call twice.
    virtual void stop() = 0;
};

#ifdef __ANDROID__
// OpenSL ES output (API 23 and newer, both flavors).
std::unique_ptr<AudioSink> make_opensles_audio_sink();
#endif

} // namespace android
} // namespace midibn
