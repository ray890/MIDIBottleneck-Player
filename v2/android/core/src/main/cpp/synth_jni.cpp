// JNI entry points of the built-in synthesizer route (Sonivox). Java side: org.midibottleneck.core.route.SynthMidiRoute.
//
// Like the other routes it ends in the shared midibn::android::BatchingOutput; attaching makes it the output the engine
// sends through (route_targets.h: current_route_output()). Sends, batching, SysEx, reset and panic then run through
// RouteNative (nativeSendShort ... nativePanic) exactly as for the USB routes.
#include <jni.h>

#include <algorithm>
#include <memory>
#include <string>

#include "midibn/midi_output.h"
#include "route_targets.h"
#include "sonivox_route_target.h"

namespace {

jstring to_java(JNIEnv* env, const std::string& text) { return env->NewStringUTF(text.c_str()); }

} // namespace

extern "C" {

// Starts the synthesizer and its audio output and attaches it to the engine. Returns null or an error text.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeAttach(JNIEnv* env, jclass)
{
    std::unique_ptr<midibn::android::RouteTarget> target = midibn::android::make_sonivox_route_target();
    if (!target) return to_java(env, "The built-in synthesizer is not available in this build.");
    std::shared_ptr<midibn::android::BatchingOutput> output(new midibn::android::BatchingOutput(std::move(target)));
    midibn::platform::OutputStatus status = output->open(0);
    if (!status.ok()) return to_java(env, status.message);
    midibn::android::set_current_route_output(output);
    return nullptr;
}

// Latency and counters of the open synthesizer as one sentence; null when it is not open.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeStatus(JNIEnv* env, jclass)
{
    const std::string text = midibn::android::sonivox_route_status_text();
    return text.empty() ? nullptr : to_java(env, text);
}

// ---- Synth settings (the player's "Synth settings" page). Any thread: they only write atomics; the audio thread applies them
// to Sonivox before its next buffer. They belong to the process, not to an open synth, so they can be set while the synth is
// closed and apply when it opens. Nothing is saved.

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeSetVolume(JNIEnv*, jclass, jint percent)
{
    midibn::android::global_synth_settings().set_volume_percent(percent);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeSetVoiceLimit(JNIEnv*, jclass, jint voices)
{
    midibn::android::global_synth_settings().set_voice_limit(voices);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeSetReverbOn(JNIEnv*, jclass, jboolean on)
{
    midibn::android::global_synth_settings().set_reverb_on(on != JNI_FALSE);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeSetReverbPreset(JNIEnv*, jclass, jint preset)
{
    midibn::android::global_synth_settings().set_reverb_preset(preset);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeSetReverbAmount(JNIEnv*, jclass, jint percent)
{
    midibn::android::global_synth_settings().set_reverb_amount(percent);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeResetSettings(JNIEnv*, jclass)
{
    midibn::android::global_synth_settings().reset();
}

// The voice maximum the library was compiled with (MAX_SYNTH_VOICES); the voice limit slider's upper end.
JNIEXPORT jint JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeMaxVoices(JNIEnv*, jclass)
{
    return midibn::android::SonivoxSynth::max_voices();
}

// {volume percent, voice limit (never above the build maximum), reverb on (0/1), reverb preset, reverb amount percent, build maximum of voices}
JNIEXPORT jintArray JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeGetSettings(JNIEnv* env, jclass)
{
    const midibn::android::SynthSettings& s = midibn::android::global_synth_settings();
    const int maxv = midibn::android::SonivoxSynth::max_voices();
    jint values[6] = {s.volume_percent.load(), std::min(s.voice_limit.load(), maxv), s.reverb_on.load() ? 1 : 0,
                      s.reverb_preset.load(), s.reverb_amount.load(), maxv};
    jintArray result = env->NewIntArray(6);
    if (result) env->SetIntArrayRegion(result, 0, 6, values);
    return result;
}

// ---- DLS sound bank. The bank is read on the calling (background) thread into memory of its own and handed to the audio thread,
// which installs it before its next buffer (see dls_bank.h); the audio is never paused. Not saved: gone when the process ends.

// Reads a DLS bank from an open file descriptor (not closed here). Null on success; "NOT_SEEKABLE" when the descriptor cannot be
// read at an offset (the caller copies the document to a cache file and tries again); otherwise one plain sentence.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeLoadDls(JNIEnv* env, jclass, jint fd, jstring name)
{
    std::string label;
    if (name) {
        const char* chars = env->GetStringUTFChars(name, nullptr);
        if (chars) { label = chars; env->ReleaseStringUTFChars(name, chars); }
    }
    std::string error;
    const midibn::android::DlsBank::LoadResult result = midibn::android::global_synth_settings().dls.load_fd(fd, label, error);
    if (result == midibn::android::DlsBank::kLoaded) return nullptr;
    if (result == midibn::android::DlsBank::kNotSeekable) return to_java(env, "NOT_SEEKABLE");
    return to_java(env, error.empty() ? std::string("The sound bank could not be loaded.") : error);
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeUseBuiltInSounds(JNIEnv*, jclass)
{
    midibn::android::global_synth_settings().dls.clear();
}

// File name of the loaded bank; null when the built-in sounds are in use.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeDlsName(JNIEnv* env, jclass)
{
    midibn::android::DlsBank& bank = midibn::android::global_synth_settings().dls;
    if (!bank.loaded()) return nullptr;
    return to_java(env, bank.name());
}

// {peak 0..100 percent of full scale, buffers with clipped samples, clipped samples}; null when the synth is not open.
JNIEXPORT jintArray JNICALL Java_org_midibottleneck_core_route_SynthMidiRoute_nativeMeter(JNIEnv* env, jclass)
{
    midibn::android::SynthStats stats;
    if (!midibn::android::sonivox_route_stats(stats)) return nullptr;
    const unsigned long long cap = 2000000000ull;
    jint values[3] = {static_cast<jint>(std::min<unsigned long long>(100, (static_cast<unsigned long long>(stats.peak) * 100 + 16383) / 32768)),
                      static_cast<jint>(std::min<unsigned long long>(cap, stats.clipped_buffers)),
                      static_cast<jint>(std::min<unsigned long long>(cap, stats.clipped_samples))};
    jintArray result = env->NewIntArray(3);
    if (result) env->SetIntArrayRegion(result, 0, 3, values);
    return result;
}

} // extern "C"
