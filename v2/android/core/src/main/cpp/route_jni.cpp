// JNI entry points of the MIDI output routes (Milestone 8, U12). Java side: org.midibottleneck.core.route.RouteNative.
//
// The Java route and the native route both end in the same C++ object, midibn::android::BatchingOutput, which is also
// what the engine sends through (route_targets.h: current_route_output()). The Java classes only open devices and
// attach them here; sends, batching, SysEx, reset and panic all run in BatchingOutput.
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "midibn/midi_output.h"
#include "route_targets.h"

namespace {

using midibn::android::BatchingOutput;
using midibn::platform::OutputStatus;

std::string g_last_error;   // last failure text of a Java-callable operation (UI thread use)

jstring to_java(JNIEnv* env, const std::string& text) { return env->NewStringUTF(text.c_str()); }

// null when the status is ok, else the message text.
jstring status_text(JNIEnv* env, const OutputStatus& status)
{
    if (status.ok()) return nullptr;
    return to_java(env, status.message);
}

std::string from_java(JNIEnv* env, jstring text)
{
    if (!text) return std::string();
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(text, chars);
    return result;
}

jstring finish_attach(JNIEnv* env, std::unique_ptr<midibn::android::RouteTarget> target, const std::string& error)
{
    if (!target) return to_java(env, error.empty() ? "The MIDI route could not be attached." : error);
    std::shared_ptr<BatchingOutput> output(new BatchingOutput(std::move(target)));
    OutputStatus status = output->open(0);
    if (!status.ok()) return to_java(env, status.message);
    midibn::android::set_current_route_output(output);
    return nullptr;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeAmidiCompiled(JNIEnv*, jclass)
{
    return midibn::android::amidi_route_compiled() ? JNI_TRUE : JNI_FALSE;
}

// Attaches the Java route (an opened or not yet opened MidiRoute). Returns null or an error text.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeAttachJava(JNIEnv* env, jclass, jobject route)
{
    std::unique_ptr<midibn::android::RouteTarget> target = midibn::android::make_java_route_target(env, route);
    return finish_attach(env, std::move(target), "The Java MIDI route does not have the methods the native side needs.");
}

// Attaches an android.media.midi.MidiDevice the Java side opened to the native (AMidi) route.
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeAttachAMidi(
    JNIEnv* env, jclass, jobject midiDevice, jint portNumber)
{
#ifdef MIDIBN_ROUTE_AMIDI
    std::string error;
    std::unique_ptr<midibn::android::RouteTarget> target =
        midibn::android::make_amidi_route_target(env, midiDevice, portNumber, error);
    return finish_attach(env, std::move(target), error);
#else
    (void)midiDevice; (void)portNumber;
    return to_java(env, "This build has no native MIDI (AMidi) route.");
#endif
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeDetach(JNIEnv*, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    midibn::android::set_current_route_output(nullptr);
    if (output) output->close();
}

JNIEXPORT jboolean JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeIsAttached(JNIEnv*, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    return output && output->is_open() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeSetLost(JNIEnv* env, jclass, jstring message)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (output) output->set_lost(from_java(env, message));
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeSendShort(JNIEnv* env, jclass, jint packed)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (!output) return to_java(env, "No MIDI output device is open.");
    OutputStatus failure;
    if (output->try_send_short(static_cast<uint32_t>(packed), failure)) return nullptr;
    return status_text(env, failure);
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeSendLong(
    JNIEnv* env, jclass, jbyteArray data, jint offset, jint length)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (!output) return to_java(env, "No MIDI output device is open.");
    if (!data || offset < 0 || length < 0 || offset + length > env->GetArrayLength(data))
        return to_java(env, "Invalid System Exclusive buffer.");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (length > 0) env->GetByteArrayRegion(data, offset, length, reinterpret_cast<jbyte*>(bytes.data()));
    midibn::output::SystemExclusiveEvent event;
    event.status = (length > 0 && bytes[0] == 0xF0) ? 0xF0 : 0xF7;
    event.data = bytes.data();
    event.length = bytes.size();
    return status_text(env, output->send_long(event));
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeBeginBatch(JNIEnv*, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (output) output->begin_send_batch();
}

JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeEndBatch(JNIEnv*, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (output) output->end_send_batch();
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeReset(JNIEnv* env, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (!output) return nullptr;
    return status_text(env, output->reset());
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativePanic(JNIEnv* env, jclass)
{
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    if (!output) return nullptr;
    return status_text(env, output->panic());
}

// Timing of the real engine path on the attached route: `bursts` bursts of `perBurst` short messages, each burst
// bracketed by begin/end_send_batch exactly as the engine does. audible = note on/off pairs, else a silent controller
// (CC 20). Returns {sent, failed, totalNs, minBurstNs, medianBurstNs, p99BurstNs, maxBurstNs, targetWrites}; the first
// failure text is kept for nativeBenchError().
JNIEXPORT jlongArray JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeBench(
    JNIEnv* env, jclass, jint bursts, jint perBurst, jboolean audible)
{
    jlongArray result = env->NewLongArray(8);
    std::shared_ptr<BatchingOutput> output = midibn::android::current_route_output();
    g_last_error.clear();
    jlong values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (!output || bursts <= 0 || perBurst <= 0) {
        g_last_error = output ? "Nothing to send." : "No MIDI output device is open.";
        values[1] = 1;
        env->SetLongArrayRegion(result, 0, 8, values);
        return result;
    }
    using clock = std::chrono::steady_clock;
    std::vector<int64_t> times;
    times.reserve(static_cast<size_t>(bursts));
    int64_t sent = 0, failed = 0;
    const uint64_t writesBefore = output->write_count();
    const clock::time_point start = clock::now();
    for (int b = 0; b < bursts; ++b) {
        const clock::time_point t0 = clock::now();
        output->begin_send_batch();
        for (int i = 0; i < perBurst; ++i) {
            uint32_t packed;
            if (audible) {
                const uint32_t key = 60u + static_cast<uint32_t>((i / 2) % 12);
                packed = (i % 2 == 0) ? (0x90u | (key << 8) | (20u << 16)) : (0x80u | (key << 8));
            } else {
                packed = 0xB0u | (20u << 8) | (static_cast<uint32_t>(i & 127) << 16);
            }
            OutputStatus failure;
            if (output->try_send_short(packed, failure)) ++sent;
            else { ++failed; if (g_last_error.empty()) g_last_error = failure.message; }
        }
        output->end_send_batch();
        times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count());
    }
    const int64_t total = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    std::vector<int64_t> sorted(times);
    std::sort(sorted.begin(), sorted.end());
    values[0] = sent;
    values[1] = failed;
    values[2] = total;
    values[3] = sorted.front();
    values[4] = sorted[sorted.size() / 2];
    values[5] = sorted[std::min(sorted.size() - 1, static_cast<size_t>(static_cast<double>(sorted.size()) * 0.99))];
    values[6] = sorted.back();
    values[7] = static_cast<jlong>(output->write_count() - writesBefore);
    env->SetLongArrayRegion(result, 0, 8, values);
    return result;
}

// Write trace (route_output.h): switch, and the ring as {route, startNs, durationNs, bytes, result} per record, oldest first.
JNIEXPORT void JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeTraceEnable(JNIEnv*, jclass, jboolean on)
{
    midibn::android::write_trace_enable(on == JNI_TRUE);
}

JNIEXPORT jlongArray JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeTraceSnapshot(JNIEnv* env, jclass)
{
    const std::vector<midibn::android::WriteTraceRecord> records = midibn::android::write_trace_snapshot();
    std::vector<jlong> flat;
    flat.reserve(records.size() * 5);
    for (size_t i = 0; i < records.size(); ++i) {
        flat.push_back(records[i].route);
        flat.push_back(records[i].start_ns);
        flat.push_back(records[i].duration_ns);
        flat.push_back(records[i].bytes);
        flat.push_back(records[i].result);
    }
    jlongArray result = env->NewLongArray(static_cast<jsize>(flat.size()));
    if (result && !flat.empty()) env->SetLongArrayRegion(result, 0, static_cast<jsize>(flat.size()), flat.data());
    return result;
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_route_RouteNative_nativeBenchError(JNIEnv* env, jclass)
{
    return g_last_error.empty() ? nullptr : to_java(env, g_last_error);
}

} // extern "C"
