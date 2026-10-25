// The Android platform layer (Milestone 8, U11; ARCHITECTURE.md section 4).
//
// The portable parts (clock, wake event, waiter, threads, files, memory) are the Linux layer's POSIX code
// (platform/linux/linux_platform.cpp) compiled unchanged with Android's C library (Bionic); the namespace is still
// midibn::posix and the Win32 class-name aliases of platform/linux/win32_platform.h apply, so the tests build unchanged.
// This file adds only what is Android specific. MIDI output routes live in platform/android/route/ (empty at U11).
#pragma once

#include <string>

namespace midibn {
namespace android {

// "Android 14 (API 34), Pixel 7, arm64-v8a" (system properties through __system_property_get; the ABI is the
// one this binary was built for).
std::string describe_device();

// The ABI this binary was compiled for: "arm64-v8a", "armeabi-v7a", "x86_64" or "x86".
const char* compiled_abi();

} // namespace android
} // namespace midibn
