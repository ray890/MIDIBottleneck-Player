Built-in software synthesizer for Android (Sonivox). Every *.cpp in this folder is compiled into the Android platform layer
(v2/android/native/CMakeLists.txt globs it; tests/ is a subfolder and is not globbed). sonivox_midi_stream.c is C and is compiled
into the Sonivox library target instead (it needs Sonivox's internal headers).

  sonivox_synth.h/.cpp        SonivoxSynth: wraps Sonivox; lock-free byte ring from the engine's output thread to the audio thread
  sonivox_midi_stream.c       EAS_OpenMIDIStream / EAS_WriteMIDIStream / EAS_CloseMIDIStream, which the open-source Sonivox lacks
  dls_bank.h/.cpp             DlsBank: a DLS sound bank loaded on any thread, installed by the render thread
  sonivox_route_target.h/.cpp SonivoxRouteTarget: a RouteTarget (route_output.h), so BatchingOutput applies unchanged
  audio_sink.h                AudioSink interface (real: OpenSL ES; tests: a fake that pulls audio by hand)
  opensles_audio_sink.cpp     OpenSL ES output, Android only (empty elsewhere)
  tests/synth_tests.cpp       unit tests (target midibn-synth-tests)

Sonivox itself is vendored in v2/android/third_party/sonivox (Apache 2.0). JNI: v2/android/core/src/main/cpp/synth_jni.cpp;
Java: v2/android/core/src/main/java/org/midibottleneck/core/route/SynthMidiRoute.java.
See v2/android/README-dev.md, section "Built-in synth (Sonivox)".
