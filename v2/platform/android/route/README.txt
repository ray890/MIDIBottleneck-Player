MIDI output routes (C++) for Android. Every *.cpp in this folder is compiled into the Android platform layer
automatically (v2/android/native/CMakeLists.txt globs it; tests/ is a subfolder and is not globbed).

  route_output.h/.cpp        BatchingOutput: the midibn::platform::MidiOutput of both routes (encoding, one write per burst,
                             SysEx, reset, panic, failure handling) over a RouteTarget
  java_route_target.cpp      Java route target (one JNI call to MidiRoute.sendBytes per burst) + the registry of the open output
  amidi_route_target.cpp     native route target (AMidiInputPort_send); compiled only with MIDIBN_ROUTE_AMIDI (modern flavor)
  fake_route_target.h        stand-in target for tests
  route_targets.h            factories and current_route_output() for the engine bridge
  tests/route_tests.cpp      unit tests (target midibn-route-tests)

The JNI entry points are v2/android/core/src/main/cpp/route_jni.cpp; the Java side is
v2/android/core/src/main/java/org/midibottleneck/core/route/. See v2/android/README-dev.md, section "MIDI output routes".
