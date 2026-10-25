// Factories of the two real route targets and the registry of the one output the app currently uses.
// The JNI entry points (core/src/main/cpp/route_jni.cpp) attach a route here; the engine bridge asks for the output.
#pragma once

#include "route_output.h"

#include <jni.h>

#include <memory>
#include <string>

namespace midibn {
namespace android {

// Java route: every write is one call of org.midibottleneck.core.route.MidiRoute.sendBytes(byte[], int, int) (one JNI
// call, one MidiInputPort.send for the whole buffer). Works from any thread (attaches the thread to the VM on demand).
std::unique_ptr<RouteTarget> make_java_route_target(JNIEnv* env, jobject route);

#ifdef MIDIBN_ROUTE_AMIDI
// Native route (API 29+): `midiDevice` is an android.media.midi.MidiDevice the Java side opened. Returns null and sets
// `error` when AMidi cannot take it.
std::unique_ptr<RouteTarget> make_amidi_route_target(JNIEnv* env, jobject midiDevice, int port_number, std::string& error);
#endif

// True when this build has the AMidi route.
bool amidi_route_compiled();

// The output the engine sends through. Set by the Java side (RouteNative.attach*), read by the engine bridge on the
// engine thread. The shared pointer keeps the output alive while a burst is running when the route is detached.
std::shared_ptr<BatchingOutput> current_route_output();
void set_current_route_output(std::shared_ptr<BatchingOutput> output);

} // namespace android
} // namespace midibn
