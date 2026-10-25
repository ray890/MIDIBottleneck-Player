# MIDIBottleneck Player v2 (private).
#
# Modern Windows:  cmake -S v2 -B v2/out/build-x64 -G "Visual Studio 17 2022" -A x64 -T v143
#                  cmake -S v2 -B v2/out/build-x86 -G "Visual Studio 17 2022" -A Win32 -T v143
# Windows XP:      cmake -S v2 -B v2/out/build-xp -G "Visual Studio 17 2022" -A Win32 -T v141_xp -DMIDIBN_XP=ON
# XP, no SSE2:     cmake -S v2 -B v2/out/build-xp-ia32 -G "Visual Studio 17 2022" -A Win32 -T v141_xp -DMIDIBN_XP=ON -DMIDIBN_XP_IA32=ON
# See BUILDING.md.
cmake_minimum_required(VERSION 3.20)
if(WIN32)
  project(midibn_v2 LANGUAGES CXX RC)
else()
  project(midibn_v2 LANGUAGES CXX)   # Linux: the portable core and its tests (see BUILDING.md)
endif()

option(MIDIBN_XP "Build the Windows XP SP3 x86 variant (requires the v141_xp toolset)" OFF)
option(MIDIBN_XP_IA32 "XP variant for processors without SSE2: compile with /arch:IA32 (only with MIDIBN_XP=ON)" OFF)
option(MIDIBN_PORTABLE_LINUX "Linux: link libstdc++ and libgcc statically so the programs run on other distributions (the self-test package, v2/tools/package-linux-selftest.sh). No effect on Windows" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static runtime so executables are self-contained (required for XP).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32 AND NOT MSVC)
  message(FATAL_ERROR "On Windows only MSVC is supported.")
endif()

if(NOT WIN32)
  # Linux (GCC or Clang): the portable core, the virtual platform, the engine harness and the tests and tools that
  # need no Win32. The Windows-only targets are not defined.
  if(MIDIBN_XP OR MIDIBN_XP_IA32)
    message(FATAL_ERROR "The XP options are only valid on Windows.")
  endif()
  add_compile_options(-Wall -Wextra)
  if(MIDIBN_PORTABLE_LINUX)
    # glibc, GTK 3 and ALSA stay dynamic (the distribution supplies them); only the C++ runtime is carried along.
    add_link_options(-static-libstdc++ -static-libgcc)
  endif()
  find_package(Threads REQUIRED)
  set(MIDIBN_PLATFORM midibn_platform_linux)
else()
  set(MIDIBN_PLATFORM midibn_platform_win32)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX PSAPI_VERSION=1)
endif()

if(NOT WIN32)
  # (no Windows version macros)
elseif(MIDIBN_XP)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The XP build is x86 only (-A Win32).")
  endif()
  add_compile_definitions(_WIN32_WINNT=0x0501 WINVER=0x0501 MIDIBN_XP_BUILD=1)
  # XP lacks the support MSVC's thread-safe static initialisation needs.
  add_compile_options(/Zc:threadSafeInit-)
  if(MIDIBN_XP_IA32)
    # Plain 32-bit x86 code generation: no SSE or SSE2 instructions in our code (x87 floating point).
    add_compile_options(/arch:IA32)
    add_compile_definitions(MIDIBN_XP_IA32_BUILD=1)
    # Whole-program optimisation (/GL + /LTCG) for the XP release build only: about 6 % faster loading and playback
    # processing in this build, none in x64 (prompt 010 measurement; DD register).
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  endif()
else()
  if(MIDIBN_XP_IA32)
    message(FATAL_ERROR "MIDIBN_XP_IA32 is only valid together with MIDIBN_XP=ON.")
  endif()
  add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

# DD-059: code that is not on a hot path (the window code, the application layer behind it, text formatting) is compiled
# for size (/O1); the engine, the models, the loader, the indexes, the Analysis scan and track routing keep /O2. The later
# /O1 overrides the /O2 of the Release configuration (compiler note D9025, silenced).
set(MIDIBN_SIZE_OPTIMISED_SOURCES
  core/src/app/channel_catalog.cpp
  core/src/app/channel_editor.cpp
  core/src/app/channel_grid.cpp
  core/src/app/channel_session.cpp
  core/src/app/effective_speed.cpp
  core/src/app/settings_store.cpp
  core/src/app/help_text.cpp
  core/src/app/player_controller.cpp
  core/src/app/rate_model.cpp
  core/src/app/routing_dialog_model.cpp
  core/src/app/statistics.cpp
  core/src/app/ui_mappings.cpp
  core/src/format/display_format.cpp
  core/src/format/net_format.cpp
  core/src/overrides/override_snapshot.cpp
  core/src/routing/route_plan_view.cpp
  frontends/win32/analysis_window.cpp
  frontends/win32/channel_grid.cpp
  frontends/win32/channels_window.cpp
  frontends/win32/controls.cpp
  frontends/win32/controls_input.cpp
  frontends/win32/dialogs.cpp
  frontends/win32/display_pacer.cpp
  frontends/win32/glue.cpp
  frontends/win32/lane_grid.cpp
  frontends/win32/main.cpp
  frontends/win32/main_window.cpp
  frontends/win32/routing_dialog.cpp
  frontends/win32/selftest.cpp
  frontends/win32/ui_common.cpp
  frontends/win32/workload_graph.cpp)
if(MSVC)
  set_source_files_properties(${MIDIBN_SIZE_OPTIMISED_SOURCES} PROPERTIES COMPILE_OPTIONS "/O1;/wd9025")
endif()

# Portable core: standard C++ only, no OS calls.
add_library(midibn_core STATIC
  core/src/app/player_controller.cpp
  core/src/app/rate_model.cpp
  core/src/channel_index.cpp
  core/src/engine/channel_state.cpp
  core/src/engine/engine_ordered.cpp
  core/src/engine/engine_types.cpp
  core/src/engine/engine_worker.cpp
  core/src/engine/ordered_adapters.cpp
  core/src/engine/playback_engine.cpp
  core/src/event_store.cpp
  core/src/midi_loader.cpp
  core/src/model_trace.cpp
  core/src/app/channel_catalog.cpp
  core/src/app/channel_editor.cpp
  core/src/app/channel_grid.cpp
  core/src/app/channel_session.cpp
  core/src/models/delayed_note_off_gate.cpp
  core/src/models/model_base.cpp
  core/src/models/ordered_admission.cpp
  core/src/models/per_note_gate.cpp
  core/src/models/priority.cpp
  core/src/models/queue.cpp
  core/src/models/rate.cpp
  core/src/models/rate_gates.cpp
  core/src/models/replay_gates.cpp
  core/src/models/replay_analysis.cpp
  core/src/analysis/workload_analysis.cpp
  core/src/app/effective_speed.cpp
  core/src/app/settings_store.cpp
  core/src/app/help_text.cpp
  core/src/app/statistics.cpp
  core/src/app/statistics_tips.cpp
  core/src/app/ui_mappings.cpp
  core/src/format/display_format.cpp
  core/src/format/net_format.cpp
  core/src/overrides/override_snapshot.cpp
  core/src/app/analysis_cache.cpp
  core/src/app/analysis_format.cpp
  core/src/app/analysis_graph.cpp
  core/src/app/analysis_resolution.cpp
  core/src/app/analysis_summary.cpp
  core/src/app/analysis_view.cpp
  core/src/app/analysis_window_model.cpp
  core/src/routing/route_plan_view.cpp
  core/src/routing/track_routing.cpp
  core/src/app/routing_dialog_model.cpp
  core/src/tempo_clock.cpp
  core/src/output/midi_output.cpp
  core/src/output/sysex_assembler.cpp
  core/src/sha256.cpp
  core/src/trace.cpp
  core/src/virtual_platform.cpp)
target_include_directories(midibn_core PUBLIC core/include)

# v1's .NET decimal arithmetic, emulated bit for bit. Not part of the product: event times use exact integer
# arithmetic (DD-020). Kept only as the test oracle that measures the new clock against the .NET decimal vectors.
add_library(midibn_decimal_oracle STATIC core/src/net_decimal.cpp)
target_include_directories(midibn_decimal_oracle PUBLIC core/include)

if(NOT WIN32)
# Linux platform layer (Milestone 8, stages 3 and 4): file system, clock, events, waiter and threads over POSIX; the
# ALSA sequencer output and the capability report when the ALSA development files are installed (libasound2-dev).
find_package(ALSA)
add_library(midibn_platform_linux STATIC platform/linux/linux_platform.cpp)
target_include_directories(midibn_platform_linux PUBLIC platform/linux)
target_link_libraries(midibn_platform_linux PUBLIC midibn_core Threads::Threads)
target_link_libraries(midibn_core PUBLIC Threads::Threads)
if(ALSA_FOUND)
  target_sources(midibn_platform_linux PRIVATE
    platform/linux/alsa_midi_output.cpp
    platform/linux/alsa_seq.cpp
    platform/linux/linux_capability.cpp)
  target_include_directories(midibn_platform_linux PUBLIC ${ALSA_INCLUDE_DIRS})
  target_link_libraries(midibn_platform_linux PUBLIC ${ALSA_LIBRARIES})
  target_compile_definitions(midibn_platform_linux PUBLIC MIDIBN_HAVE_ALSA=1)
endif()
else()
# Win32 platform layer.
add_library(midibn_platform_win32 STATIC
  platform/win32/kdmapi_midi_output.cpp
  platform/win32/midi_driver_discovery.cpp
  platform/win32/win32_platform.cpp
  platform/win32/thread_report.cpp
  platform/win32/win32_timing.cpp
  platform/win32/winmm_direct_output.cpp
  platform/win32/winmm_midi_output.cpp)
target_include_directories(midibn_platform_win32 PUBLIC platform/win32)
target_link_libraries(midibn_platform_win32 PUBLIC midibn_core psapi winmm version advapi32)
endif()

# Deterministic lock-step engine services for tests (standard C++ threads;
# no OS calls of its own).
add_library(midibn_platform_virtual STATIC
  platform/virtual/lockstep_services.cpp)
target_include_directories(midibn_platform_virtual PUBLIC platform/virtual)
target_link_libraries(midibn_platform_virtual PUBLIC midibn_core)

# Engine comparison harness (Milestone 5, Group E): the C++ twin of the v1 reference
# harness's rendezvous spike (lock-step services, scenario parser/binder/runner). It measures every difference
# from v1's recordings; differences must be explained by the improvement register's rules.
add_library(midibn_engine_harness STATIC
  harness/engine/lockstep_scheduler.cpp
  harness/engine/scenario_binder.cpp
  harness/engine/scenario_file.cpp
  harness/engine/scenario_runner.cpp)
target_include_directories(midibn_engine_harness PUBLIC harness/engine)
target_link_libraries(midibn_engine_harness PUBLIC midibn_core)

function(midibn_tool name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE tools)
  target_link_libraries(${name} PRIVATE ${MIDIBN_PLATFORM})
  if(MIDIBN_XP)
    # Mark the executable as runnable on Windows XP (subsystem version 5.01).
    target_link_options(${name} PRIVATE /SUBSYSTEM:CONSOLE,5.01)
  endif()
endfunction()

midibn_tool(midibn-dump tools/dump_main.cpp)
if(WIN32)
# Direct WinMM driver access check (DD-085): the Windows 11 check package and the XP self-test run this.
midibn_tool(midibn-direct-check tools/direct_check_main.cpp)
target_link_libraries(midibn-direct-check PRIVATE winmm advapi32)
if(MIDIBN_XP)
  # The XP self-test package carries this program next to the self-test programs.
  if(MIDIBN_XP_IA32)
    set_target_properties(midibn-direct-check PROPERTIES OUTPUT_NAME v2-xp-direct-check-ia32)
  else()
    set_target_properties(midibn-direct-check PROPERTIES OUTPUT_NAME v2-xp-direct-check)
  endif()
endif()
# Large-address check (DD-114): fills the low 2 GB of address space, then opens a chosen output and sends silent messages
# from memory above 2 GB. Default path "none" and "fake-direct" open no device. Marked large-address-aware below.
midibn_tool(midibn-large-address-probe tools/large_address_probe_main.cpp)
target_link_libraries(midibn-large-address-probe PRIVATE winmm advapi32)
# Thread report (read-only; opens an output with only silent messages): which threads an output path creates in our
# process and at which priority. The XP self-test package carries it.
midibn_tool(midibn-thread-report tools/thread_report_main.cpp)
# Help text dump (one platform's markup to a file; for before/after hashes of the Help wording).
midibn_tool(midibn-help-dump tools/help_dump_main.cpp)
target_link_libraries(midibn-thread-report PRIVATE winmm advapi32)
if(MIDIBN_XP)
  if(MIDIBN_XP_IA32)
    set_target_properties(midibn-thread-report PROPERTIES OUTPUT_NAME v2-xp-thread-report-ia32)
  else()
    set_target_properties(midibn-thread-report PROPERTIES OUTPUT_NAME v2-xp-thread-report)
  endif()
endif()
# KDMAPI provider search check (DD-119): read-only; says WHERE OmniMIDI.dll would be found (nothing is loaded).
midibn_tool(midibn-kdmapi-check tools/kdmapi_check_main.cpp)
target_link_libraries(midibn-kdmapi-check PRIVATE winmm advapi32)
if(MIDIBN_XP)
  if(MIDIBN_XP_IA32)
    set_target_properties(midibn-kdmapi-check PROPERTIES OUTPUT_NAME v2-xp-kdmapi-check-ia32)
  else()
    set_target_properties(midibn-kdmapi-check PROPERTIES OUTPUT_NAME v2-xp-kdmapi-check)
  endif()
endif()
endif()
midibn_tool(midibn-parity tools/parity_main.cpp)
# Capability probe (measurements only): needs the multimedia and version libraries.
if(WIN32)
target_sources(midibn-parity PRIVATE tools/capability_probe.cpp)
target_link_libraries(midibn-parity PRIVATE winmm version)
endif()
target_link_libraries(midibn-parity PRIVATE midibn_decimal_oracle)
# The bit-exact engine comparison (also part of the XP self-test).
target_sources(midibn-parity PRIVATE tools/engine_compare.cpp tools/difference_rules.cpp)
target_link_libraries(midibn-parity PRIVATE midibn_engine_harness)
midibn_tool(midibn-engine-harness tools/engine_harness_main.cpp)
target_sources(midibn-engine-harness PRIVATE tools/engine_compare.cpp tools/difference_rules.cpp)
target_link_libraries(midibn-engine-harness PRIVATE midibn_engine_harness)
if(WIN32)   # uses the Windows sampling profiler
midibn_tool(midibn-perf tools/perf_main.cpp)
target_link_libraries(midibn-perf PRIVATE midibn_platform_virtual)
endif()
# The load as the player runs it (inspection pass, then parse), with the time of each (DD-055).
if(WIN32)   # times with QueryPerformanceFrequency
midibn_tool(midibn-load-flow tools/load_flow_main.cpp)
endif()
# How playback behaves on a PC with one or two logical processors, with and without the few-processor options (P10-THR).
if(WIN32)
  midibn_tool(midibn-few-cpu-measure tools/few_cpu_measure_main.cpp)
  if(MIDIBN_XP)   # also built for the XP self-test package (--auto: the silent few-processor measurement, P12-WINPLAT)
    if(MIDIBN_XP_IA32)
      set_target_properties(midibn-few-cpu-measure PROPERTIES OUTPUT_NAME v2-xp-few-cpu-ia32)
    else()
      set_target_properties(midibn-few-cpu-measure PROPERTIES OUTPUT_NAME v2-xp-few-cpu)
    endif()
  endif()
endif()
if(MIDIBN_XP)
  if(MIDIBN_XP_IA32)
    set_target_properties(midibn-parity PROPERTIES OUTPUT_NAME v2-xp-selftest-ia32)
  else()
    set_target_properties(midibn-parity PROPERTIES OUTPUT_NAME v2-xp-selftest)
  endif()
endif()

if(WIN32)
# The Windows player (Milestone 6): plain Win32, common controls v6 through the manifest.
add_executable(midibn-win32 WIN32
  frontends/win32/main.cpp
  frontends/win32/main_window.cpp
  frontends/win32/settings_file.cpp
  frontends/win32/controls.cpp
  frontends/win32/controls_input.cpp
  frontends/win32/dialogs.cpp
  frontends/win32/glue.cpp
  frontends/win32/selftest.cpp
  frontends/win32/analysis_window.cpp
  frontends/win32/workload_graph.cpp
  frontends/win32/channels_window.cpp
  frontends/win32/channel_grid.cpp
  frontends/win32/routing_dialog.cpp
  frontends/win32/lane_grid.cpp
  frontends/win32/display_pacer.cpp
  frontends/win32/ui_common.cpp
  frontends/win32/midibn-win32.rc
  frontends/win32/midibn-win32.manifest)
target_include_directories(midibn-win32 PRIVATE frontends/win32)
if(MIDIBN_XP)
  # The Windows 7.1A SDK's objbase.h (IID_PPV_ARGS_Helper) does not compile under strict two-phase lookup.
  target_compile_options(midibn-win32 PRIVATE /Zc:twoPhase-)
endif()
# (the window smoke tests below need the same option on the XP toolset; they are defined further down)
set(MIDIBN_WINDOW_SMOKE_TARGETS midibn-analysis-window-smoke midibn-channels-window-smoke midibn-routing-dialog-smoke)
target_link_libraries(midibn-win32 PRIVATE midibn_platform_win32 comdlg32 comctl32 shell32 ole32 oleaut32 user32 gdi32)
if(MIDIBN_XP)
  target_link_options(midibn-win32 PRIVATE /SUBSYSTEM:WINDOWS,5.01)
endif()

# DD-060: the 32-bit player and the two tools that load big files are large-address-aware: on a 64-bit Windows they get
# a 4 GiB address space instead of 2 GiB (on 32-bit Windows, 3 GiB with the /3GB boot option, else unchanged), so a
# bigger MIDI file fits. The self-test (midibn-parity) is left as it is: its capability report measures the plain case.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  foreach(large_file_target midibn-win32 midibn-perf midibn-dump midibn-load-flow midibn-large-address-probe)
    target_link_options(${large_file_target} PRIVATE /LARGEADDRESSAWARE)
  endforeach()
endif()
endif()

if(NOT WIN32)
# The Linux player (Milestone 8, stage 5): GTK 3, linked dynamically (not shipped). Built only when GTK 3 is found.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
endif()
if(GTK3_FOUND)
  add_executable(midibn-gtk
    frontends/gtk/main.cpp
    frontends/gtk/main_window.cpp
    frontends/gtk/settings_file.cpp
    frontends/gtk/gtk_util.cpp
    frontends/gtk/help_window.cpp
    frontends/gtk/analysis_window.cpp
    frontends/gtk/workload_graph.cpp
    frontends/gtk/channels_window.cpp
    frontends/gtk/routing_dialog.cpp
    frontends/gtk/companion_windows.cpp)
  target_include_directories(midibn-gtk PRIVATE frontends/gtk platform/linux)
  target_link_libraries(midibn-gtk PRIVATE midibn_platform_linux PkgConfig::GTK3)
  message(STATUS "GTK 3 ${GTK3_VERSION} found: building midibn-gtk")
else()
  message(STATUS "GTK 3 not found: midibn-gtk is not built")
endif()
endif()

add_executable(midibn-tests tests/unit_tests.cpp)
target_link_libraries(midibn-tests PRIVATE midibn_core midibn_decimal_oracle)
if(MIDIBN_XP)
  target_link_options(midibn-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()

# Playback engine tests (Milestone 5, Group D): lock-step virtual time with
# the Null and recording outputs only; no device is opened.
add_executable(midibn-engine-tests tests/engine_tests.cpp)
target_link_libraries(midibn-engine-tests PRIVATE midibn_platform_virtual midibn_engine_harness ${MIDIBN_PLATFORM})
if(MIDIBN_XP)
  target_link_options(midibn-engine-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()

# Main-window behaviour (Milestone 6): Rate model state machine, load, transport on None.
add_executable(midibn-app-tests tests/app_tests.cpp)
target_link_libraries(midibn-app-tests PRIVATE ${MIDIBN_PLATFORM})
if(MIDIBN_XP)
  target_link_options(midibn-app-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()

# Analysis window behaviour (Milestone 6): the app-layer tests against v1's own output
# (harness/expected/analysis-ui, written by harness/analysis-ui-reference).
add_executable(midibn-analysis-app-tests tests/analysis_app_tests.cpp)
target_link_libraries(midibn-analysis-app-tests PRIVATE ${MIDIBN_PLATFORM})
if(MIDIBN_XP)
  target_link_options(midibn-analysis-app-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()

if(WIN32)
# The Analysis window itself (Milestone 6): a PlayerController on a recording output, a real window, a click,
# a screenshot (WM_PRINT; the screen is never captured). No device is opened.
add_executable(midibn-analysis-window-smoke
  tests/analysis_window_smoke.cpp
  frontends/win32/analysis_window.cpp
  frontends/win32/workload_graph.cpp
  frontends/win32/display_pacer.cpp
  frontends/win32/ui_common.cpp
  frontends/win32/controls.cpp
  frontends/win32/controls_input.cpp
  frontends/win32/midibn-win32.rc
  frontends/win32/midibn-win32.manifest)
target_include_directories(midibn-analysis-window-smoke PRIVATE frontends/win32)
target_link_libraries(midibn-analysis-window-smoke PRIVATE midibn_platform_win32 comdlg32 comctl32 shell32 ole32 oleaut32 user32 gdi32)
if(MIDIBN_XP)
  target_link_options(midibn-analysis-window-smoke PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
endif()

# Channels window, portable layer (Milestone 6): grid cells, editor and request flow against
# the tables v1's own form wrote (harness/expected/channels-ui, argv[1]).
add_executable(midibn-channels-app-tests tests/channels_app_tests.cpp)
target_link_libraries(midibn-channels-app-tests PRIVATE midibn_core)
if(MIDIBN_XP)
  target_link_options(midibn-channels-app-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()

if(WIN32)
# Channels window, Win32 layer (Milestone 6): the real window on a PlayerController with a recording output
# (hidden unless --show; screenshots come from the window's own paint code). No device is opened.
add_executable(midibn-channels-window-smoke tests/channels_window_smoke.cpp
  frontends/win32/channels_window.cpp
  frontends/win32/channel_grid.cpp
  frontends/win32/ui_common.cpp
  frontends/win32/midibn-win32.rc
  frontends/win32/midibn-win32.manifest)
target_include_directories(midibn-channels-window-smoke PRIVATE frontends/win32)
target_link_libraries(midibn-channels-window-smoke PRIVATE midibn_platform_win32 comctl32 user32 gdi32)
if(MIDIBN_XP)
  target_link_options(midibn-channels-window-smoke PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
endif()

# ---- Track routing editor (Milestone 6): dialog model vs v1's TrackRoutingForm -
# midibn-routing-app-tests: replays harness/expected/routing-ui (argv[1] fixtures dir, argv[2] tables dir).
add_executable(midibn-routing-app-tests tests/routing_app_tests.cpp)
target_link_libraries(midibn-routing-app-tests PRIVATE ${MIDIBN_PLATFORM})
if(MIDIBN_XP)
  target_link_options(midibn-routing-app-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
if(WIN32)
# midibn-routing-dialog-smoke: opens the real dialog, drives it with messages from a second thread,
# checks the controller afterwards (recording outputs only; no device, no provider).
add_executable(midibn-routing-dialog-smoke
  tests/routing_dialog_smoke.cpp
  frontends/win32/routing_dialog.cpp
  frontends/win32/lane_grid.cpp
  frontends/win32/controls.cpp
  frontends/win32/controls_input.cpp
  frontends/win32/ui_common.cpp
  frontends/win32/midibn-win32.rc
  frontends/win32/midibn-win32.manifest)
target_include_directories(midibn-routing-dialog-smoke PRIVATE frontends/win32)
target_link_libraries(midibn-routing-dialog-smoke PRIVATE midibn_platform_win32 comdlg32 comctl32 shell32 ole32 oleaut32 user32 gdi32)
if(MIDIBN_XP)
  target_link_options(midibn-routing-dialog-smoke PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
endif()
# ---- end Track routing editor tests ------------------------------------------

if(WIN32)   # tests the Win32 timing classes themselves
add_executable(midibn-platform-tests tests/platform_tests.cpp)
target_link_libraries(midibn-platform-tests PRIVATE midibn_platform_win32)
if(MIDIBN_XP)
  target_link_options(midibn-platform-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
endif()

# MIDI output backends (Milestone 6). Neither program opens a device or loads
# a provider: the first runs the device-free unit checks, the second only
# enumerates WinMM devices and tests for the existence of OmniMIDI.dll.
if(WIN32)
add_executable(midibn-midi-output-tests tests/midi_output_tests.cpp)
target_link_libraries(midibn-midi-output-tests PRIVATE midibn_platform_win32)
add_executable(midibn-midi-enum tests/midi_enum_tests.cpp)
target_link_libraries(midibn-midi-enum PRIVATE midibn_platform_win32)
# Direct WinMM driver access (DD-085): path policy, matching, start-up sequence and the output on a fake modMessage.
add_executable(midibn-direct-output-tests tests/direct_output_tests.cpp)
target_link_libraries(midibn-direct-output-tests PRIVATE midibn_platform_win32)
if(MIDIBN_XP)
  target_link_options(midibn-direct-output-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
  target_link_options(midibn-midi-output-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
  target_link_options(midibn-midi-enum PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
endif()

# Linux platform layer tests and the ALSA output (Milestone 8, stages 3 and 4). None opens a MIDI port: the output tests
# use a fake sequencer (plus a client-to-client loopback when /dev/snd/seq exists); the send-check tool is for the
# main session's own Linux machine and sends only silent controller messages.
if(NOT WIN32)
add_executable(midibn-linux-platform-tests tests/linux_platform_tests.cpp)
target_link_libraries(midibn-linux-platform-tests PRIVATE midibn_platform_linux)
if(ALSA_FOUND)
add_executable(midibn-alsa-output-tests tests/alsa_output_tests.cpp)
target_link_libraries(midibn-alsa-output-tests PRIVATE midibn_platform_linux)
midibn_tool(midibn-alsa-ports tools/alsa_ports_main.cpp)
midibn_tool(midibn-alsa-send-check tools/alsa_send_check_main.cpp)
midibn_tool(midibn-linux-capability tools/linux_capability_main.cpp)
endif()
endif()

# ---- real-time engine property checks (Milestone 5 exit test) --------------
# midibn-engine-realtime: the v2 side of tools/engine-properties.ps1 (real clock,
# real threads, a recording output that sends nowhere; no device is opened).
if(WIN32)
midibn_tool(midibn-engine-realtime tools/engine_realtime_main.cpp)
endif()
# ---- end real-time engine property checks -----------------------------------

# ---- UI layer tests (portable format/app layers vs v1 reference tables) -----
# midibn-ui-tests: reads the tables in harness/expected/ui (argv[1], default v2/harness/expected/ui).
add_executable(midibn-ui-tests tests/ui_tests.cpp)
target_link_libraries(midibn-ui-tests PRIVATE midibn_core)
if(MIDIBN_XP)
  target_link_options(midibn-ui-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
endif()
# ---- end UI layer tests ------------------------------------------------------

# Remembered settings (prompt 013, Step 4): file format, load / save rules, the few-processor default (portable).
add_executable(midibn-settings-tests tests/settings_tests.cpp)
target_link_libraries(midibn-settings-tests PRIVATE midibn_core)
if(WIN32)
  target_sources(midibn-settings-tests PRIVATE frontends/win32/settings_file.cpp)
  target_include_directories(midibn-settings-tests PRIVATE frontends/win32)
  target_link_libraries(midibn-settings-tests PRIVATE shell32)
endif()
if(MIDIBN_XP)
  target_link_options(midibn-settings-tests PRIVATE /SUBSYSTEM:CONSOLE,5.01)
  target_compile_options(midibn-settings-tests PRIVATE /Zc:twoPhase-)   # the Windows 7.1A SDK's objbase.h (shlobj.h), as for midibn-win32
endif()

if(MIDIBN_XP)
  foreach(target ${MIDIBN_WINDOW_SMOKE_TARGETS})
    target_compile_options(${target} PRIVATE /Zc:twoPhase-)
  endforeach()
endif()

# ---- real-device measurements (prompt 007 Step 2) ---------------------------
# midibn-device-measure OPENS REAL MIDI OUTPUTS (WinMM, KDMAPI). Never run it directly: use
# v2/tools/device/run-device-measurements.ps1, which mutes the playback endpoints first.
# Built for the normal x64 and x86 builds only (not the XP variants).
if(WIN32 AND NOT MIDIBN_XP)
  midibn_tool(midibn-device-measure tools/device_measure_main.cpp)
  target_sources(midibn-device-measure PRIVATE tools/device_analysis.cpp)
endif()
# ---- end real-device measurements -------------------------------------------
