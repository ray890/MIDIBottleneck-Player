# Android build: developer note (Milestone 8, U11)

Private notes for the helpers who work on the Android apps. Nothing here is published.

## What is here

Two apps, each built in two flavors from the same source:

| App | Flavor `modern` (main build) | Flavor `android6` (private "Android 6+" build) |
|---|---|---|
| Player (`:player`) | "MIDIBottleneck Player", `org.midibottleneck.player`, Android 10+ (minSdk 29), native MIDI (AMidi) route | "MIDIBottleneck Player (Android 6+)", `org.midibottleneck.player.android6`, Android 6+ (minSdk 23), Java MIDI (`android.media.midi`) route with batching |
| Self-test (`:selftest`) | "MIDIBottleneck Self-test", `org.midibottleneck.selftest` | "MIDIBottleneck Self-test (Android 6+)", `org.midibottleneck.selftest.android6` |

The `.android6` application-id suffix lets both builds be installed side by side. The self-test is a separate app from
the player (own package, own name). ABIs: `arm64-v8a`, `armeabi-v7a`, `x86_64`. Java only (no Kotlin), plain Android
framework classes, no AndroidX, **no library is bundled in any APK**.

## Module and folder layout (who works where)

```
v2/android/                     Gradle project (settings.gradle, build.gradle, gradlew, build.cmd)
  core/                         :core, shared Android library (both apps depend on it)
    src/main/java/org/midibottleneck/core/
      NativeCore.java           JNI entry points (shared; add methods, do not rename)
      InfoScreen.java           the minimal U11 screen helper
      route/                    OUTPUT-ROUTES helper: MidiRoute.java (the shared interface) + the Java route
    src/main/cpp/               JNI bridge: every *.cpp here is compiled automatically (jni_bridge.cpp is the U11 stub set)
  native/CMakeLists.txt         the native build (Gradle and run-native-tests.ps1 both use it)
  player/                       :player  -> PLAYER-UI helper owns src/main/java/org/midibottleneck/player/ and the
                                 res/ folders of this module
  selftest/                     :selftest -> SELF-TEST helper owns src/main/java/org/midibottleneck/selftest/ and
                                 the res/ folders of this module
v2/platform/android/            Android platform layer (C++)
  android_platform.h/.cpp       device description, compiled ABI
  route/                        OUTPUT-ROUTES helper: native routes (AMidi); every *.cpp here is compiled automatically
```

Flavor-specific files go in `src/modern/` and `src/android6/` of a module (the app name strings are there already).
Routes belong in `core`, not in an app: the player and the self-test use the same ones.

### The shared interface

* Java: `org.midibottleneck.core.route.MidiRoute` (name, availability, device list, open/close, `sendShort(int packed)`,
  `sendLong(byte[],off,len)`, `beginBatch()/endBatch()`, `panic()`). Stub at U11; the routes helper implements it and may
  extend it (additive changes only, so the other two helpers are not broken).
* JNI: `NativeCore` (`coreVersion`, `deviceDescription`, `modelDemo`, `setMidiRoute` stub). The native engine side is
  the portable core; its output interface is `midibn::MidiOutput` (`core/include/midibn/midi_output.h`). A native route
  (AMidi) implements `MidiOutput` directly; the Java route is reached by a `MidiOutput` wrapper that calls `MidiRoute`
  through JNI (the routes helper writes it in `v2/platform/android/route/`).
* The CMake option `MIDIBN_ROUTE_AMIDI` is ON for `modern` and OFF for `android6` (also defines `MIDIBN_ROUTE_AMIDI=1`
  for C++; link `amidi` in `native/CMakeLists.txt` when the AMidi route is added).

## MIDI output routes (U12)

Output goes to Android MIDI devices (USB, Bluetooth once connected by the system, and synth apps that provide a MIDI device) and to the built-in Sonivox synth (see "Built-in synth (Sonivox)"); `None` stays for analysis.

**Classes** (`org.midibottleneck.core.route`, module `:core`):

| Class | Role |
|---|---|
| `MidiRoutes.available(Context)` | the routes this build offers: Java route first, native route second (modern flavor, API 29+); `preferred(routes)` picks native when present |
| `MidiRoute` | the shared interface (below) |
| `JavaMidiRoute` | `android.media.midi` route, any Android 6+ build |
| `NativeMidiRoute` | AMidi route, `modern` flavor, Android 10+ |
| `FakeMidiRoute` | stand-in: two pretend devices, records writes, `unplug()`; no Android classes |
| `RouteDevice` | device details: `index, deviceId, name, manufacturer, product, typeName` ("USB", "Virtual", "Bluetooth"), `portNumber, portName, portCount`, `displayName()`, `details()` |
| `RouteBench` | send timing for the self-test (below) |
| `MidiCodec`, `MidiBatcher`, `ByteSink` | pure-Java encoding and batching (tested on a plain JVM) |
| `RouteNative`, `AndroidMidiHost`, `AndroidRouteBase` | JNI entry points; MidiManager wrapper; shared route code |

**`MidiRoute` methods** (the first group is the U11 interface, unchanged; the second is new and all `default`):
`name()`, `isAvailable()`, `listDevices()`, `open(int)`, `close()`, `isOpen()`, `sendShort(int packed)`,
`sendLong(byte[],int,int)`, `beginBatch()`, `endBatch()`, `panic()`; new: `describeDevices()` (RouteDevice[], same order
as `listDevices()`), `sendBytes(byte[],int,int)`, `lastError()`, `discardPending()` (the core's reset), `isLost()`,
`setListener(Listener)` (`onDevicesChanged()`, `onDeviceLost(String)`, called on a background thread).
`open(index)` means the entry of the most recent `listDevices()` / `describeDevices()`; one entry per output port.
`open` blocks up to 5 s (Android opens devices asynchronously; a private looper thread serves the callback), so call it
off the UI thread. Nothing throws: a failed send returns false and `lastError()` has a plain sentence; opening a route
closes the other one (the engine has one output).

**USB permission: none.** Android opens a class-compliant USB MIDI device itself when it is plugged in and offers it to
all apps through `MidiManager`; there is no USB permission dialog and no manifest permission. The core manifest declares
`android.software.midi` and `android.hardware.usb.host` as optional features. The phone needs USB host mode (OTG cable
or adapter); devices that use a vendor driver instead of the USB MIDI class do not appear. A device another app has
open reports "already in use". Bluetooth MIDI devices appear once the system has connected them (pairing is done in the
system's Bluetooth settings, not by the player).

**Unplugging.** `MidiManager.DeviceCallback.onDeviceRemoved` marks the open route lost: later sends return false with the
text "The MIDI device ... was unplugged. Choose the output again.", the native output reports an `OutputStatus` failure
with the same text, nothing throws; a write error from the port (IOException, AMidi error code) does the same.

**Native side** (`v2/platform/android/route/`): `BatchingOutput` (a `midibn::platform::MidiOutput`) encodes messages,
collects a burst between `begin_send_batch()` and `end_send_batch()` in one buffer (at most 1000 bytes; whole messages are
never split) and does one write per burst to a `RouteTarget`: `AMidiRouteTarget` (`AMidiInputPort_send`, only with
`MIDIBN_ROUTE_AMIDI`), `JavaRouteTarget` (one JNI call to `MidiRoute.sendBytes`, which does one `MidiInputPort.send`) or
`FakeRouteTarget` (tests). `try_send_short` is overridden lean (no string work on success). SysEx uses the core
assembler and is cut into 1000-byte writes; `reset()` drops the buffer, the assembler and calls the device flush;
`panic()` is the 48 core panic messages in one write. For the engine bridge: `midibn::android::current_route_output()`
(`route_targets.h`) returns the `shared_ptr<BatchingOutput>` of the open route (null when none); the JNI side is
`route_jni.cpp` (`RouteNative`). `NativeCore.setMidiRoute` (U11 stub) is not touched; the bridge should use
`current_route_output()` and the Java side `MidiRoutes` / `route.open(i)`, which attaches the route to the engine.

**Timing for the self-test** (`RouteBench`, call from a background thread with a device open):
`RouteBench.run(route, bursts, messagesPerBurst, audible)` times the real engine path (C++ output with batching, then
JNI/Java or AMidi to the device); `RouteBench.runDirect(...)` times the Java API (`beginBatch`, `sendShort` x N,
`endBatch`). Both return `RouteBench.Result` (`sent, failed, writes, totalNs, minBurstNs, medianBurstNs, p99BurstNs,
maxBurstNs, error`, `summary()`). `audible=false` sends controller 20 (silent); `true` sends low-velocity note on/off.

**Tests.** Native: `platform/android/route/tests/route_tests.cpp` (target `midibn-route-tests`, built by
`run-native-tests.ps1`, runs on the emulator/phone; no device or sound). Java: `host-tests\run-host-tests.cmd` compiles the
pure classes with the kit's JDK and runs `RouteHostTest` (plain `main`, no library).

## Build commands

Everything needs the Android build kit folder (variable `ANDROID_KIT`) (JDK 17, SDK, NDK 27.3.13750724, CMake 3.22.1, Gradle cache). The scripts set
`JAVA_HOME`, `ANDROID_HOME`, `GRADLE_USER_HOME`, `ANDROID_USER_HOME`, `ANDROID_AVD_HOME` for their own process only.
Set `ANDROID_KIT` to use another location.

```
v2\android\build.cmd android6      rebuild the "Android 6+" player and self-test APKs (debug-signed)
v2\android\build.cmd modern        the main build (both APKs)
v2\android\build.cmd all           all four
v2\android\build.cmd clean         delete the Gradle build output
v2\android\build.cmd <Gradle args> anything else is passed to gradlew (e.g. :player:assembleModernDebug)
```

APKs: `v2\android\<module>\build\outputs\apk\<flavor>\debug\<module>-<flavor>-debug.apk`. Versions: Android Gradle plugin
9.4.1, Gradle 9.6.0 (wrapper, downloaded from services.gradle.org, checksum pinned), compileSdk/targetSdk 36,
build-tools 36.0.0, JDK 17.

Native tests on the emulator (builds with the NDK, pushes to `/data/local/tmp/midibn`, runs, stops the emulator):

```
powershell -File v2\android\run-native-tests.ps1                 # x86_64 (run) + arm64-v8a (built only); full run
powershell -File v2\android\run-native-tests.ps1 -Quick          # without the 3,956 engine runs
```

Installing in the emulator: `adb install -r <apk>`, start with `adb shell am start -n <applicationId>/org.midibottleneck.<app>.MainActivity`
(the activity class is in the namespace package, not the `.android6` suffix package). Screenshot:
`adb exec-out screencap -p > shot.png`. Start the emulator headless with
`emulator -avd midibn-api36 -no-window -no-audio -no-boot-anim` and stop it with `adb emu kill`. The AVD
`midibn-api36` lives in the `avd` folder of the Android build kit.

## Native layout

`native/CMakeLists.txt` reads the source list of the portable core (and of the engine harness) out of the root
`v2/CMakeLists.txt` at configure time, so a source added there is compiled here as well; the root file is not edited.
The Android platform layer compiles the Linux layer's `linux_platform.cpp` unchanged (clock, wake event, waiter,
threads, files) plus `android_platform.cpp`; the test sources find the Win32 class names through
`platform/linux/win32_platform.h`. The C++ runtime is linked statically (`c++_static`), segments are aligned for
16 KB pages.

## Libraries and licences

One third-party component is bundled in the APKs: the Sonivox synthesizer (first table). Everything else in the apps is
our Java, our C++ and the Android framework on the device.

| Bundled component | Version | Licence |
|---|---|---|
| Sonivox EAS, Android's software synthesizer (`third_party/sonivox`, built into `libmidibn_android.so`; the notice and licence text are in the APK as the asset `sonivox-NOTICE.txt`) | AOSP `external/sonivox` main, downloaded 2026-10-24 | Apache 2.0 (user-approved for Android only, V2_RULES.md section 2) |

Used by the build only (not shipped):

| Tool | Version | Licence |
|---|---|---|
| Android Gradle plugin | 9.4.1 | Apache 2.0 |
| Gradle (wrapper) | 9.6.0 | Apache 2.0 |
| Android SDK / build-tools / platform-tools | 36 / 36.0.0 / 37.0.1 | Android SDK licence (accepted) |
| Android NDK (clang, libc++, Bionic) | 27.3.13750724 | Apache 2.0 with LLVM exception; the NDK's libc++ is linked statically into our .so (same licence) |
| CMake, Ninja | 3.22.1 | BSD 3-clause, Apache 2.0 |
| Eclipse Temurin JDK | 17.0.20 | GPL 2 with Classpath Exception |

## Player app (U13, PLAYER-UI)

Files: `player/src/main/java/org/midibottleneck/player/`
* `MainActivity` the main screen (views built in code; a top bar with the app name and a menu button of three drawn bars, transport pinned at the bottom, the rest in a ScrollView). The menu button opens a `PopupWindow` (no AndroidX): the six toggles of the desktop system menu as check rows (state and enabled read from the bridge each time it opens; same `toggle*` bridge calls as before), "Synth settings..." and "Help". There is no "More settings" section any more.
* **Look (P12-AND11).** `Palette` (pure Java, light and dark colours in one place, `pairs()` lists every text/background pair with its WCAG minimum) and `Ui` (8 dp spacing grid, corner radii, text sizes, and helpers that dress plain views: primary/tonal buttons with pressed/focused/disabled states, outlined fields and spinners with a drawn chevron, tinted check boxes and sliders, status/navigation bar colours). The main screen is a top bar in the accent colour (also under the status bar), one rounded card per section (statistics cells are tiles inside their card) and a pinned transport bar in the surface colour. Dark mode follows the system setting: the manifest theme `@style/AppTheme` (res/values/styles.xml, values-v27 for the light navigation bar icons, values-night/styles.xml for dark; parents `Theme.DeviceDefault.Light.NoActionBar` / `Theme.DeviceDefault.NoActionBar`, no AppCompat) carries window background, bars, accent, control and button colours from `res/values*/colors.xml`, so any other screen (Synth settings, dialogs) picks them up without code; `uiMode` is no longer in the main activity's `configChanges`, so the screen is rebuilt when the system switches light/dark. `host-tests/PaletteHostTest.java` (run by `run-host-tests.cmd`) checks all contrast ratios in both modes and that colors.xml equals `Palette`. To change a colour: edit `Palette.java` and the matching line in both colors.xml files (the test fails when they differ).
* `SynthSettingsActivity` the page for the built-in synth: volume, voice limit, reverb on/off, reverb type, reverb amount, Reset, and the synth's status line (refreshed twice a second). Calls the static setters of `SynthMidiRoute`; nothing is saved.
* `PlayerHost` process-wide owner of the bridge: refresh, file opening through a descriptor, rescan of the outputs, foreground service control.
* `PlayerService` foreground service (media playback type, notification with Stop, partial wake lock while playing or paused).
* `HelpActivity` the shared Help text as a scrollable page (markup drawn with spans) plus an Android note.
* `PlayerRoutes` builds the output catalog from `MidiRoutes.available(context)`: of the hardware routes (`MidiRoute.isHardware()`, default true) only `MidiRoutes.preferred(...)` (native AMidi on the modern flavor, the Java route on android6), so a device is never listed twice, plus every non-hardware route (the built-in synthesizer returns false). Output list = None, the hardware devices, then each non-hardware route's devices.
* `RouteCatalog` (plain Java, host-tested) is the `PlayerBridge.OutputCatalog` the native player calls to list and to open: `routeCount()`, `list(route)`, `open(route, index, name)` (finds the device again by name when the list changed; opening a device that is already open returns at once), `close(route)`. It hooks every route's `setListener` and tells `PlayerHost` about device changes and a lost device.

**Output path.** `core/src/main/cpp/routed_output.h` (header only, no JNI) is the `MidiOutput` of the player. Device ids are `(route << 16) | device index`. `enumerate()` and `open()` call the catalog (`JavaCatalog` in `player_jni.cpp`, a few JNI calls per list or open); after `open()` it keeps the route's shared `BatchingOutput` (`current_route_output()`) and forwards `try_send_short`, `send_short`, `send_long`, `reset`, `panic` and the batch calls to it directly: no JNI call per message (the Java route target adds one JNI call per burst, the AMidi route none).

**Opening a device.** The controller opens the output synchronously on the main thread when Play is tapped (and when the output is changed during playback), and a device can take up to 5 s to open. `PlayerHost.warmSelected()` therefore opens the selected hardware device on a worker thread while stopped (at start, on selection, after a rescan or device change), so Play finds it open and returns at once; a failure there is ignored and Play shows it. If the worker is still opening when Play is tapped, Play waits for it (same lock). Switching the output during playback still opens on the main thread (a core design point). The built-in synthesizer route is not opened ahead.

**Device changes.** A plugged or unplugged device (`onDevicesChanged`) and a lost open device (`onDeviceLost`) set a "rescan wanted" flag in `PlayerHost`; while stopped it runs `bridge.refreshDevices()` at once (the selection stays by name, a vanished output becomes None with the controller's warning), while playing it waits for Stop. A lost device also makes every later send fail with the route's plain sentence, which the engine reports.

Bridge: `core/.../PlayerBridge.java` and `core/src/main/cpp/player_jni.cpp` (a native `PlayerController`; one `snapshot()` per refresh into `long[]`/`String[]`; the `N_*`/`T_*` indices in the Java class and the enums in the C++ file must be kept in step). Permissions are in `player/src/main/AndroidManifest.xml` (foreground service, media playback type, notifications, wake lock).

**Statistics (P12-AND10).** The Statistics area shows the cells of the desktop Analysis window's Playback statistics, with its captions and units and in its order: Playback, Timeline / output, Queue now / maximum, Events sent / dropped, Output rate, Effective speed, Lag current / maximum; then Maximum rate (the main window's own cell, kept because the Analysis list has none; it is still in the compact form). The compact Maximum lag and Current lag cells are folded into the one Lag cell, and the separate state line is the Playback cell. `nativeSnapshot` builds the texts the way `AnalysisWindowModel::update_playback_state` does (`format_timeline_output`, `format_analysis_queue`, `format_event_counts`, `format_analysis_lag`, `output_rate_text()`, `effective_speed_text()`, from one extra `engine().snapshot()`), so no core code changed. Layout: a caption over a bold value per cell, the first two cells a whole row, the rest two to a row.

**Live updates (P12-AND10).** No 100 ms poll any more. While playing or loading, `MainActivity` runs the update inside a `Choreographer` frame callback, arms the next one only after the update ended, and `PlayerBridge.RefreshPacing` (pure Java, host-tested in `host-tests/PacingHostTest.java`) is the Android twin of the desktop's `DisplayPacer` plus `display_tier.h` (the exact rule is in its class comment): tier 0 an update per display frame, tier 1 half of the statistics cells per update, tier 2 and up also 60/30/15/10 updates per second; 0.5 s windows judged against a budget of 8 / 12 / 25 % of the main thread by processor count; frames passed while an update runs are skipped, never queued. `PlayerHost` coalesces the engine's wake calls (one posted pump-and-refresh at a time). Idle states update on events only. Counters of the latest run (frames, updates, frames skipped by the rate cap, late frames, update cost mean and maximum, tier) are shown nowhere by default: a long press on the app name in the top bar shows them, and each run's end writes the same line to the log with tag `MIDIBN-pace` (`adb logcat -s MIDIBN-pace`).

Not yet: Analysis, Channels and Track routing pages. See DD-138 in `v2/docs/DIFFERENCES_FROM_V1.md`.

## The self-test app (U14)

"MIDIBottleneck Self-test" (`:selftest`, both flavors) is separate from the player. Sources: Java in
`selftest/src/main/java/org/midibottleneck/selftest/`, native in `selftest/src/main/cpp/` (its own `CMakeLists.txt`; it reads
the core and harness source lists from the root `v2/CMakeLists.txt` like `native/CMakeLists.txt`, which is not touched).
A separate hand-test sheet for users is kept outside the published tree.

* **How the test programs run on the phone.** Each program (`tests/unit_tests.cpp`, `ui_tests`, `channels_app_tests`,
  `app_tests`, `analysis_app_tests`, `routing_app_tests`, `engine_tests`, `linux_platform_tests`, `tools/parity_main.cpp`
  = the v2 baseline, `tools/engine_harness_main.cpp`, plus `android_capability_main.cpp`) is compiled **unchanged** into a
  shared library `libst_<name>.so` (its `main()` stays an exported symbol). `libselftest_jni.so` forks a child process, which
  redirects stdout/stderr to the result file, `chdir`s to the data folder, `dlopen`s the library, calls `dlsym("main")` and
  `_exit`s with the result. Chosen over "executables packaged as lib*.so run from nativeLibraryDir" because that needs
  extracted libraries and exec rights that Android 10+ restricts; chosen over calling main in-process because a crash or a
  hang then fails only that program (the parent kills it at its time limit). Release speed is forced
  (`-DCMAKE_BUILD_TYPE=Release`) even in the debug-signed APK.
* **Data.** `packTestData` (Gradle Zip task) packs `v2/harness/{fixtures,expected,expected-v2,expected-v2-ui,engine-scenarios.txt}`
  into the asset `selftest-data.zip` (about 10 MB); the app unpacks it to `files/data/v2/harness/...` (once per install) and
  runs the programs with that folder as the working directory, with the same arguments as `run-native-tests.ps1`.
* **linux_platform_tests** is compiled with API 26 declarations (`pthread_getname_np`) and run only on API 26+; it also
  needs a writable `/tmp`, which apps do not have, so it is reported as SKIP until the test accepts `TMPDIR` (needed change in
  `tests/linux_platform_tests.cpp`, outside this module).
* **Results.** Run folder `<date_time>_<model>` in the app's external files folder, copied after every stage to
  `Downloads/MIDIBottleneck self-test/<run>/` (`Exporter`: MediaStore Downloads on API 29+; public Downloads folder with the
  WRITE_EXTERNAL_STORAGE permission, asked at run time, on API 23-28). Files: `summary.txt` (last line `OVERALL PASS`/`OVERALL FAIL`;
  INFO and SKIP lines do not count, FAIL does), `progress.txt`, `capability-java.txt`, `capability-native.txt`, `midi-devices.txt`,
  `tests/<program>.txt`, `timing-longrun.txt`, and after the optional checks `send-check.txt`, `file-check.txt`.
* **Routes.** The self-test reaches the routes by reflection (`RouteAccess`: `MidiRoutes.available(Context)` and
  `MidiRoute.describeDevices()`), so it builds and runs while those classes are missing (no routes = an INFORMATION line).
  `SendCheck` uses only the `MidiRoute` interface (open, sendShort batched and unbatched, a paced run, sendLong, panic, close).
* Run it on a phone with `adb install -r selftest-modern-debug.apk` and start `org.midibottleneck.selftest/org.midibottleneck.selftest.MainActivity`
  (not run on a device or emulator at the time of writing: no phone or emulator was available to the work item).


## Built-in synth (Sonivox)

The player can make the sound itself: output "Built-in synth (Sonivox)" (route `SynthMidiRoute`, in both flavors, always
the last route of `MidiRoutes.available(Context)`; never the default of `MidiRoutes.preferred`). It needs no MIDI device
and no permission. Windows and Linux are unchanged (no synthesizer there); on Android this is a user decision (V2_RULES.md
section 2, 2026-10-24).

**Synthesizer.** Sonivox EAS, Android's own wavetable synth (Apache 2.0), vendored in `v2/android/third_party/sonivox/` (not included in the public 2.0 source; fetch it from android.googlesource.com/platform/external/sonivox to build the Android app)
(README.txt there: origin, archive SHA-256, files kept, what is not Sonivox). Fixed at build time: 22050 Hz, stereo, 16 bit,
64 voices, filter and reverb on, General MIDI sound set (about 0.5 MB of sample data per ABI inside `libmidibn_android.so`).
The open-source release declares but does not contain `EAS_OpenMIDIStream` / `EAS_WriteMIDIStream` / `EAS_CloseMIDIStream`;
`v2/platform/android/synth/sonivox_midi_stream.c` implements them on Sonivox's internal MIDI parser (about 80 lines).

**Native layout** (`v2/platform/android/synth/`, compiled into `midibn_platform_android`):

| File | Role |
|---|---|
| `sonivox_synth.h/.cpp` | `SonivoxSynth`: the only code that touches Sonivox, always from the audio thread; a lock-free single-producer single-consumer byte ring (64 KiB) between the engine's output thread and the audio thread; counters |
| `sonivox_route_target.h/.cpp` | `SonivoxRouteTarget`, a `RouteTarget` like the Java and AMidi ones: `BatchingOutput` (encoding, bursts, SysEx assembly, reset, panic) sits on top unchanged |
| `audio_sink.h` | `AudioSink`: where the audio goes (fake in the tests) |
| `opensles_audio_sink.cpp` | OpenSL ES output: PCM buffer queue, 22050 Hz stereo 16 bit, low-latency mode requested; OpenSL ES exists on API 23, so one code path serves both flavors |
| `tests/synth_tests.cpp` | target `midibn-synth-tests` (153 checks, with the settings tests) |

JNI: `core/src/main/cpp/synth_jni.cpp` (`SynthMidiRoute.nativeAttach` builds the target and the `BatchingOutput` and sets
`current_route_output()`, as the other routes do; `nativeStatus` returns the counters). Java: `SynthMidiRoute.open(0)`
claims the route (closing any other), attaches, and the engine then sends through the shared native output; `status()` gives
the latency and counters as one sentence.

**Threads and timing.** Sonivox is not thread-safe. The engine's output thread only copies bytes into the ring (`write`) or
moves a "discard up to here" mark (`discard_pending`, i.e. reset); it never takes a lock and never waits for audio. The audio
thread (owned by OpenSL ES) drains the ring into `EAS_WriteMIDIStream` before each 128-frame block (5.8 ms) and renders.
Output is 3 buffers of 256 frames (11.6 ms) at 22050 Hz: about 35 ms queued ahead of the speaker, plus the phone's own audio
delay (22050 Hz is not a native rate on most phones, so the system resamples and the fast path is not used). Note timing
is therefore quantized to the buffer period (11.6 ms) and shifted by a constant delay; the engine's timing is unaffected.
The constants are `config_` in `SonivoxRouteTarget`'s constructor.

**Reset, panic, overflow.** `reset` (`discard_pending`) makes the audio thread skip every byte queued before it. Panic is the
shared 48 CC messages; Sonivox fades the voices out within about 11 ms of reading them (measured in the test), but audio already
queued in OpenSL ES (up to ~35 ms) still plays. A full ring (64 KiB, about 21,000 messages: only if the audio thread stops
for seconds) drops the whole write, counts it (`overflow_writes`, `overflow_bytes`) and playback goes on; writes while the audio
is not running are dropped and counted too (`not_running_writes`). Counters, with slow buffers (render took longer than the
audio it made), longest buffer and render errors: `SynthMidiRoute.status()` returns them as one sentence.

**Settings (volume, voice limit, reverb).** `SynthSettings` (in `sonivox_synth.h`) is a struct of atomics plus a version counter; the real synth uses the process-wide `global_synth_settings()`, the tests give each synth its own. Setters (any thread; JNI `SynthMidiRoute.setVolume`, `setVoiceLimit`, `setReverbOn`, `setReverbPreset`, `setReverbAmount`, `resetSettings`; getters `settings()` and `maxVoices()`, the latter the compiled `MAX_SYNTH_VOICES` via `SonivoxSynth::max_voices()`) only store the value and bump the version. `SonivoxSynth::render()` compares the version before each buffer and applies the changes on the audio thread, so Sonivox is still touched by one thread only. Settings made while the synth is closed apply when it opens (a new Sonivox instance applies everything on its first buffer); nothing is saved between runs.
* Volume 0 to 200 percent (100 = normal), in two parts. Sonivox saturates its 32-bit mix to 16 bits inside `EAS_Render` (`SynthMasterGain` in `eas_mixer.c`: guard bits, master gain, `SATURATE`), so a gain applied to the finished buffer only turns clipped audio down. The part that is a whole number of decibels therefore goes through **Sonivox's own master volume** (`EAS_SetVolume(data, NULL, v)`, `MIDIBN_SetMasterVolume`): range 0 to 100, **1 dB per step**, 90 is the library's default after `EAS_Init` and is the player's 100 percent, 100 is +10 dB, 0 is no sound (gain 0), and the gain is applied to the mix before the saturation, so a lower volume gives headroom. `master = 90 + ceil(20 log10(percent / 100))` (200 percent = 97, 50 percent = 84, 35 percent = 80), rounded up so what is left is a small reduction (about 0 to 1 dB) and never an increase. That remainder is a gain on the finished buffer, ramped across the buffer (no click). When the master step changes, the buffer gain is rescaled in the same buffer so the total does not jump. At 0 percent the master stays where it is and the ramped gain makes exact silence. Above 100 percent clipping returns (more master volume, same saturation).
* Level meter and clip counter (`SynthStats`: `peak`, `clipped_buffers`, `clipped_samples`; JNI `SynthMidiRoute.meter()`): after the render loop, before the fine gain, samples at the 16-bit limits (32767, -32768) are counted (that is Sonivox's saturation showing; the fine gain is counted after, so it cannot hide it); the peak of the audio sent to the speaker is held and falls 10 percent per buffer. The page shows a bar, a CLIP light that stays lit about a second after any clipped buffer, and the count of clipped buffers.
* Voice limit 1 to `max_voices()`, default 64 (`SynthSettings::kDefaultVoices`; the build maximum is `MAX_SYNTH_VOICES=4096` in `native/CMakeLists.txt`): `MIDIBN_SetStreamPolyphony` in `sonivox_midi_stream.c` calls Sonivox's `VMSetPolyphony` and then mutes the oldest sounding voices above a lower limit (Sonivox's own `VMSetPolyphony` looks the voices up by `nextChannel`, which is cleared once a voice has started, so it never cuts one that already sounds).
* Reverb: Sonivox starts with it bypassed (kept as the default). On/off is `EAS_PARAM_REVERB_BYPASS`, type is `EAS_PARAM_REVERB_PRESET` (large hall, hall, chamber, room), amount is `EAS_PARAM_REVERB_WET` (0 to 32767 for 0 to 100 percent, default 85). Sonivox reloads the preset's own wet level inside the first buffer after a preset change, so the wet level is set again right after that buffer. Sonivox has no separate dry control (the dry value is only a pass-through test), so there is one "amount". The reverb acts on the whole mix (this build has no per-channel reverb send).

**Voice maximum 4096.** `MAX_SYNTH_VOICES=4096` (Google ships 64); the synth still opens at 64 and the page lets the user raise it. Checked in the vendored source (nothing changed there): every voice index is `EAS_INT`/`EAS_I32`, the counters in the voice manager and the synth are `EAS_U16` (`numActiveVoices`, `activeVoices`, `maxPolyphony`, voice `age`: fine to 65,535), the voice tables are plain arrays (`voices[]`, `wtVoices[]`) inside one `malloc` of `S_VOICE_MGR` (heap, not stack), the stealing scans are linear loops over all voices (cost grows with the limit but are only run at note start), the sample mix is `long` per sample (no overflow at 4096 voices) and saturates at the end. Findings above 255: `S_SYNTH.poolCount[]` and `poolAlloc[]` are `EAS_U8`. `poolCount[0]` counts the voices of the stream, so it wraps at 256; consequences: `poolAlloc` is only read in SP-MIDI mode (never used here), and the test `poolCount != 0` before key-group and same-note polyphony limiting in `VMStartNote` is skipped when the voice count is exactly 256, 512, ... (the second note of the same key on a channel then does not cut the oldest one of that key). Cosmetic; nothing indexes with these values. One more Sonivox rule shapes dense music: at most 2 voices per key per channel (`DEFAULT_CHANNEL_POLYPHONY_LIMIT`), so non-drum sounds reach about 15 channels x 116 keys x 2 = 3,480 key voices (more only with layered instruments). Memory: `S_VOICE_MGR` is 5,448 bytes at 64 voices and 328,008 bytes at 4096 (80 bytes per voice: 16 for `S_SYNTH_VOICE` + 64 for `S_WT_VOICE`), allocated once when the synth opens (`v2/out/prompt-012/P12-AND9/sizes.c` prints them; arm64 has the same sizes).

**DLS sound bank.** `SynthSettings::dls` (`dls_bank.h/.cpp`, class `DlsBank`) holds the bank. Open-source Sonivox has the parser (`DLSParser`) but not `EAS_LoadDLSCollection`, so `sonivox_midi_stream.c` has the player's own `MIDIBN_ParseDLS` / `MIDIBN_SetStreamDLS` (the same steps as Android's `EAS_IntSetStrmParam(PARSER_DATA_DLS_COLLECTION)`). The host layer (`eas_hostmm.c`) opens "files" through an `EAS_FILE` locator of three callbacks (`readAt`, `size`, handle), so no host change is needed: the player passes a file descriptor reader (`pread` through a 256 KiB window, because the parser reads a few bytes at a time) or a memory reader (tests). **Threads:** parsing runs on the caller's background thread with its own temporary host data (`EAS_HWInit`), touching no shared Sonivox state, and may take seconds; the finished bank is published (generation counter) and the **render thread installs it itself** before its next buffer: it frees the stream's sounding voices (they point into the old bank), swaps one pointer, releases the old bank and looks every channel's instrument up again (channels keep their programs and controllers). The audio is never paused and never waits for the file; the only lock is a mutex held for microseconds by the loader (swap) and the render thread (install), because Sonivox's reference count is not atomic. A synth that opens later installs the bank on its first buffer; closing the synth takes it out of the stream first. **Banks:** bank numbers are mapped (`MIDIBN_RemapPrograms`) because a channel starts in bank 0x79 (melodic) or 0x78 (rhythm) and Sonivox finds DLS sounds by exact bank: melodic bank 0 becomes the default melodic bank, instruments with the drum flag (bit 31, which Sonivox's parser drops) become the rhythm bank; other bank numbers are kept. Instruments the bank lacks keep the built-in sounds. **Limits (Sonivox's):** 1024 samples, 2048 regions, 256 instruments, 1 MiB per sample, PCM 8 or 16 bit mono, 8/16-bit looped samples; the player adds a 192 MiB file limit. Errors are one sentence each: not a DLS file / cannot use this bank (more or larger than the synth supports) / too large. Android side: `ACTION_OPEN_DOCUMENT` with type `*/*` and no MIME list; `openFileDescriptor` on a background thread; a provider that gives a pipe is copied to `cacheDir` (`dls-load-*.tmp`, deleted right after loading, stale copies deleted on page start). Nothing is stored: no preference, no path; the bank is gone when the process ends.

**Benchmark.** `midibn-synth-tests` ends with an INFORMATION section (never a failure): for N = 64 ... 4096 voices sounding at once (sustained notes on 15 channels with different instruments, no drums) it prints the average and worst time to render one 256-frame buffer (11.61 ms of audio) and that as a percentage of real time, single thread. On the phone: `adb shell "cd /data/local/tmp/midibn && ./bin/midibn-synth-tests --bench"` (only the benchmark) or run the whole program (`run-native-tests.ps1` prints it in `midibn-synth-tests.log`); `--no-bench` skips it.

**Build.** `native/CMakeLists.txt`: target `midibn_sonivox` (C; the platform's `libsonivoxwithoutjet` settings, `-O2` even in
debug, `-w` for its files only, plain C kernels on every ABI), `midibn_platform_android` links it and `OpenSLES`. The
`-ffp-contract=off` flag and all other flags are unchanged. APK size: see DD-140 in `v2/docs/DIFFERENCES_FROM_V1.md`.

**Tests.** `midibn-synth-tests` runs through `run-native-tests.ps1` like `midibn-route-tests` (device or emulator, no sound).
On a host with g++ (WSL Ubuntu): compile the vendored `lib_src/*.c`, `host_src/*.c` and `sonivox_midi_stream.c` with the
defines above, then `core/src/output/midi_output.cpp`, `sysex_assembler.cpp`, `route_output.cpp`, `synth/*.cpp` (not the
OpenSL file; it is empty off Android) and `tests/synth_tests.cpp`, `-pthread`. Also run clean under ThreadSanitizer
(`setarch x86_64 -R` is needed on WSL2).