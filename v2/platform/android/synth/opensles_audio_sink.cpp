// OpenSL ES audio output for the built-in synthesizer (Android only; OpenSL ES exists from API 9, so one code path serves
// both flavors). A PCM buffer queue with a few small buffers: every time the system finishes one, the callback fills
// it with the next stretch of audio and queues it again. The callback runs on a thread the system owns and never waits
// for the player's output thread.
#ifdef __ANDROID__

#include "audio_sink.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace midibn {
namespace android {

namespace {

class OpenSlesSink : public AudioSink {
public:
    ~OpenSlesSink() override { stop(); }

    const char* name() const override { return "OpenSL ES"; }

    bool start(const AudioConfig& config, RenderFn render, std::string& error) override
    {
        stop();
        config_ = config;
        render_ = std::move(render);
        const size_t samples = config.frames_per_buffer * static_cast<size_t>(config.channels);
        buffers_.assign(static_cast<size_t>(config.buffer_count), std::vector<int16_t>(samples, 0));
        next_ = 0;
        stopping_.store(false);

        if (slCreateEngine(&engineObject_, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return fail("the engine could not be created", error);
        if ((*engineObject_)->Realize(engineObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return fail("the engine could not be started", error);
        if ((*engineObject_)->GetInterface(engineObject_, SL_IID_ENGINE, &engine_) != SL_RESULT_SUCCESS) return fail("the engine interface is missing", error);
        if ((*engine_)->CreateOutputMix(engine_, &mixObject_, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return fail("the output mix could not be created", error);
        if ((*mixObject_)->Realize(mixObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return fail("the output mix could not be started", error);

        SLDataLocator_AndroidSimpleBufferQueue queue = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, static_cast<SLuint32>(config.buffer_count)};
        SLDataFormat_PCM format;
        std::memset(&format, 0, sizeof(format));
        format.formatType = SL_DATAFORMAT_PCM;
        format.numChannels = static_cast<SLuint32>(config.channels);
        format.samplesPerSec = static_cast<SLuint32>(config.sample_rate) * 1000u;   // milliHertz
        format.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
        format.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
        format.channelMask = config.channels == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
        format.endianness = SL_BYTEORDER_LITTLEENDIAN;
        SLDataSource source = {&queue, &format};
        SLDataLocator_OutputMix mix = {SL_DATALOCATOR_OUTPUTMIX, mixObject_};
        SLDataSink sink = {&mix, nullptr};
        const SLInterfaceID ids[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
        const SLboolean required[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
        if ((*engine_)->CreateAudioPlayer(engine_, &playerObject_, &source, &sink, 2, ids, required) != SL_RESULT_SUCCESS)
            return fail("the audio player could not be created", error);

        // Ask for the low-latency path; the system may refuse (older Android, or a sample rate it must convert), which is fine.
        SLAndroidConfigurationItf configuration = nullptr;
        if ((*playerObject_)->GetInterface(playerObject_, SL_IID_ANDROIDCONFIGURATION, &configuration) == SL_RESULT_SUCCESS && configuration) {
            SLuint32 mode = SL_ANDROID_PERFORMANCE_LATENCY;
            (*configuration)->SetConfiguration(configuration, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode, sizeof(mode));
        }
        if ((*playerObject_)->Realize(playerObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return fail("the audio player could not be started", error);
        if ((*playerObject_)->GetInterface(playerObject_, SL_IID_PLAY, &play_) != SL_RESULT_SUCCESS) return fail("the play interface is missing", error);
        if ((*playerObject_)->GetInterface(playerObject_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue_) != SL_RESULT_SUCCESS) return fail("the buffer queue is missing", error);
        if ((*queue_)->RegisterCallback(queue_, &OpenSlesSink::on_buffer_done, this) != SL_RESULT_SUCCESS) return fail("the buffer callback could not be set", error);

        // Fill and queue every buffer once, then play: the callback keeps the queue full from here on.
        for (int i = 0; i < config.buffer_count; ++i) fill_and_enqueue();
        if ((*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) return fail("playback could not be started", error);
        started_ = true;
        return true;
    }

    void stop() override
    {
        stopping_.store(true);
        if (play_) (*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED);
        // Destroying the player waits for a running callback to finish and guarantees none starts afterwards.
        if (playerObject_) { (*playerObject_)->Destroy(playerObject_); playerObject_ = nullptr; }
        play_ = nullptr;
        queue_ = nullptr;
        if (mixObject_) { (*mixObject_)->Destroy(mixObject_); mixObject_ = nullptr; }
        if (engineObject_) { (*engineObject_)->Destroy(engineObject_); engineObject_ = nullptr; }
        engine_ = nullptr;
        started_ = false;
    }

private:
    bool fail(const char* what, std::string& error)
    {
        error = std::string("The audio output for the built-in synthesizer failed: ") + what + ".";
        stop();
        return false;
    }

    void fill_and_enqueue()
    {
        std::vector<int16_t>& buffer = buffers_[next_];
        next_ = (next_ + 1) % buffers_.size();
        render_(buffer.data(), config_.frames_per_buffer);
        (*queue_)->Enqueue(queue_, buffer.data(), static_cast<SLuint32>(buffer.size() * sizeof(int16_t)));
    }

    static void on_buffer_done(SLAndroidSimpleBufferQueueItf, void* context)
    {
        OpenSlesSink* self = static_cast<OpenSlesSink*>(context);
        if (self->stopping_.load(std::memory_order_acquire)) return;
        self->fill_and_enqueue();
    }

    AudioConfig config_;
    RenderFn render_;
    std::vector<std::vector<int16_t>> buffers_;
    size_t next_ = 0;
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    SLObjectItf engineObject_ = nullptr, mixObject_ = nullptr, playerObject_ = nullptr;
    SLEngineItf engine_ = nullptr;
    SLPlayItf play_ = nullptr;
    SLAndroidSimpleBufferQueueItf queue_ = nullptr;
};

} // namespace

std::unique_ptr<AudioSink> make_opensles_audio_sink() { return std::unique_ptr<AudioSink>(new OpenSlesSink()); }

} // namespace android
} // namespace midibn

#endif // __ANDROID__
