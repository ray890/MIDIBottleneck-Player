// JNI bridge of the shared Android library (module :core). Java side: org.midibottleneck.core.NativeCore.
//
// U11 state: version and a trivial model call (load a tiny in-memory MIDI file through the portable core), plus the
// route registration entry point as a stub. Later helpers add entry points in their own files in this folder (every
// *.cpp here is compiled automatically); keep the Java declarations in NativeCore.java.
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "android_platform.h"
#include "linux_platform.h"
#include "midibn/midi_loader.h"
#include "midibn/platform.h"

namespace {

// A MIDI file of one track: middle C for one quarter note at 480 ticks per quarter (default tempo 120 per minute).
const uint8_t kDemoFile[] = {
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
    'M', 'T', 'r', 'k', 0, 0, 0, 13,
    0x00, 0x90, 0x3C, 0x64,
    0x83, 0x60, 0x80, 0x3C, 0x40,
    0x00, 0xFF, 0x2F, 0x00};

class MemorySource : public midibn::platform::ByteSource {
public:
    MemorySource(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    int64_t size() const override { return static_cast<int64_t>(size_); }
    size_t read_at(int64_t offset, uint8_t* buffer, size_t count) override {
        if (offset < 0 || static_cast<size_t>(offset) >= size_) return 0;
        size_t n = std::min(count, size_ - static_cast<size_t>(offset));
        std::memcpy(buffer, data_ + offset, n);
        return n;
    }

private:
    const uint8_t* data_;
    size_t size_;
};

jstring to_java(JNIEnv* env, const std::string& text) { return env->NewStringUTF(text.c_str()); }

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_NativeCore_nativeCoreVersion(JNIEnv* env, jclass) {
    return to_java(env, std::string("MIDIBottleneck core v2 (native, ") + midibn::android::compiled_abi() + ")");
}

JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_NativeCore_nativeDeviceDescription(JNIEnv* env, jclass) {
    return to_java(env, midibn::android::describe_device());
}

// The trivial model call: loads the demo file with the portable loader and reports what it found, plus the clock the
// platform layer provides (proves the core, the loader and the Android platform layer work in the app process).
JNIEXPORT jstring JNICALL Java_org_midibottleneck_core_NativeCore_nativeModelDemo(JNIEnv* env, jclass) {
    MemorySource source(kDemoFile, sizeof(kDemoFile));
    midibn::LoadOptions options;
    midibn::Song song;
    midibn::LoadError error;
    if (!midibn::load_song(source, options, song, error)) {
        return to_java(env, "Load failed: " + error.message);
    }
    midibn::posix::MonotonicClock clock;
    char text[256];
    std::snprintf(text, sizeof(text), "Demo file: %d track, %lld note, %.3f s; clock %lld ticks per second",
        song.track_count, static_cast<long long>(song.note_count), static_cast<double>(song.duration_us) / 1e6,
        static_cast<long long>(clock.frequency()));
    return to_java(env, text);
}

// Stub for the output-route helper: registers the Java MidiRoute the native engine will send through. U11 only
// remembers nothing and reports that no route is wired yet (returns false).
JNIEXPORT jboolean JNICALL Java_org_midibottleneck_core_NativeCore_nativeSetMidiRoute(JNIEnv*, jclass, jobject) {
    return JNI_FALSE;
}

} // extern "C"
