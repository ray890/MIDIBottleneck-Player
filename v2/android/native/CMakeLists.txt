# MIDIBottleneck Player v2, Android native build (Milestone 8, U11).
#
# One CMake project for two uses:
#   1. Gradle (module :core): builds libmidibn_android.so (the portable core + the Android platform layer + the JNI bridge)
#      for each ABI of the app.
#   2. v2/android/run-native-tests.ps1: configures it with the NDK toolchain and -DMIDIBN_ANDROID_TESTS=ON, which also
#      builds the unit tests, the v2 baseline (parity) program and the engine harness as native executables that run on
#      the emulator / a phone under /data/local/tmp.
#
# The root v2/CMakeLists.txt is NOT edited or included: the source lists of the portable core and of the engine harness
# are read from it at configure time (so a source added there is picked up here without a second list to keep in step).
cmake_minimum_required(VERSION 3.22)
project(midibn_android LANGUAGES C CXX)   # C only for the vendored Sonivox synthesizer

option(MIDIBN_ANDROID_TESTS "Also build the native test executables (not part of the Gradle build)" OFF)
option(MIDIBN_ROUTE_AMIDI "Build the native MIDI (AMidi) route; needs API 29 (the 'modern' flavor)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra)
# No fused multiply-add: clang on arm64 contracts a*b+c by default, which changes the last bit of double results.
# Results must match Windows and Linux bit for bit (the ui-tests "effective-speed" rows), so contraction is off everywhere.
add_compile_options(-ffp-contract=off)
# Android 15+ devices may use 16 KB memory pages: align the segments of every binary for them.
add_link_options(-Wl,-z,max-page-size=16384)

get_filename_component(V2 "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ---- source lists read from the root CMakeLists.txt ----------------------------------------------------------------
file(READ "${V2}/CMakeLists.txt" ROOT_CMAKE)
function(midibn_root_sources library out_var)
  string(REGEX MATCH "add_library\\(${library} STATIC([^)]*)\\)" matched "${ROOT_CMAKE}")
  if(NOT matched)
    message(FATAL_ERROR "add_library(${library} STATIC ...) not found in ${V2}/CMakeLists.txt")
  endif()
  string(REGEX REPLACE "add_library\\(${library} STATIC([^)]*)\\)" "\\1" list_text "${matched}")
  string(REGEX REPLACE "[ \t\r\n]+" ";" list_text "${list_text}")
  set(result "")
  foreach(item ${list_text})
    if(NOT item STREQUAL "")
      list(APPEND result "${V2}/${item}")
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

midibn_root_sources(midibn_core CORE_SOURCES)
midibn_root_sources(midibn_engine_harness HARNESS_SOURCES)

add_library(midibn_core STATIC ${CORE_SOURCES})
target_include_directories(midibn_core PUBLIC "${V2}/core/include")
# Clang-only notes about core sources that GCC and MSVC /W4 accept (an '&&' inside '||' without parentheses, two unused
# helper functions). Not changed in the core; silenced for this target so the Android build log shows real problems.
target_compile_options(midibn_core PRIVATE -Wno-logical-op-parentheses -Wno-unused-function)

# ---- Android platform layer ----------------------------------------------------------------------------------------
# Clock, wake event, waiter, threads, files and memory are the Linux layer's POSIX code, compiled unchanged with
# Bionic. platform/android adds what is Android only (device description, routes). The ALSA files are not used.
file(GLOB ROUTE_SOURCES CONFIGURE_DEPENDS "${V2}/platform/android/route/*.cpp")
file(GLOB SYNTH_SOURCES CONFIGURE_DEPENDS "${V2}/platform/android/synth/*.cpp")   # built-in synthesizer route (tests/ is a subfolder)

# ---- Sonivox, Android's own software synthesizer (Apache 2.0), vendored in third_party/sonivox ----------------------
# Built exactly as the Android platform builds libsonivoxwithoutjet (arm-wt-22k/Android.bp): wavetable synth, 22050 Hz,
# stereo, 64 voices, 16-bit, filter and reverb on. Differences, all in the build only (no vendored file is changed): no
# JET, no XMF/OTA/iMelody/RTTTL parsers (this player only feeds a raw MIDI stream), plain C for the sample kernels on
# every ABI (the ARM assembly of the platform build is not used), and stand-in headers for the platform's <log/log.h>.
set(SONIVOX_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/sonivox")
file(GLOB SONIVOX_SOURCES CONFIGURE_DEPENDS "${SONIVOX_DIR}/lib_src/*.c" "${SONIVOX_DIR}/host_src/*.c")
# sonivox_midi_stream.c is the player's own file: the open-source release declares EAS_OpenMIDIStream/EAS_WriteMIDIStream/
# EAS_CloseMIDIStream but does not contain them, so the player implements them on Sonivox's internal MIDI parser.
add_library(midibn_sonivox STATIC ${SONIVOX_SOURCES} "${V2}/platform/android/synth/sonivox_midi_stream.c")
target_include_directories(midibn_sonivox PUBLIC "${SONIVOX_DIR}/host_src")
target_include_directories(midibn_sonivox PRIVATE "${SONIVOX_DIR}/lib_src" "${SONIVOX_DIR}/stubs")
target_compile_definitions(midibn_sonivox PRIVATE
  UNIFIED_DEBUG_MESSAGES EAS_WT_SYNTH NUM_OUTPUT_CHANNELS=2 _SAMPLE_RATE_22050 MAX_SYNTH_VOICES=4096 _16_BIT_SAMPLES
  _FILTER_ENABLED DLS_SYNTHESIZER _REVERB_ENABLED)
# Optimised even in debug builds (the synthesizer renders 64 voices in real time); its own warnings are silenced for its files only.
target_compile_options(midibn_sonivox PRIVATE -O2 -w)
target_link_libraries(midibn_sonivox PUBLIC log)

add_library(midibn_platform_android STATIC
  "${V2}/platform/linux/linux_platform.cpp"
  "${V2}/platform/android/android_platform.cpp"
  ${ROUTE_SOURCES}
  ${SYNTH_SOURCES})
target_include_directories(midibn_platform_android PUBLIC
  "${V2}/platform/linux" "${V2}/platform/android" "${V2}/platform/android/route" "${V2}/platform/android/synth")
target_link_libraries(midibn_platform_android PUBLIC midibn_core midibn_sonivox OpenSLES)   # OpenSL ES: the synthesizer's audio output (API 23 has it)
if(MIDIBN_ROUTE_AMIDI)
  target_compile_definitions(midibn_platform_android PUBLIC MIDIBN_ROUTE_AMIDI=1)
  target_link_libraries(midibn_platform_android PUBLIC amidi)   # libamidi.so, API 29+ (the modern flavor only)
endif()
# 32-bit ABIs may need libatomic for 64-bit atomics; the NDK linker ignores it where not needed.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
  target_link_libraries(midibn_core PUBLIC atomic)
endif()

# ---- the JNI library loaded by the apps ----------------------------------------------------------------------------
file(GLOB JNI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../core/src/main/cpp/*.cpp")
add_library(midibn_android SHARED ${JNI_SOURCES})
target_link_libraries(midibn_android PRIVATE midibn_platform_android log)

# ---- native test executables ---------------------------------------------------------------------------------------
if(MIDIBN_ANDROID_TESTS)
  add_library(midibn_decimal_oracle STATIC "${V2}/core/src/net_decimal.cpp")
  target_include_directories(midibn_decimal_oracle PUBLIC "${V2}/core/include")

  add_library(midibn_platform_virtual STATIC "${V2}/platform/virtual/lockstep_services.cpp")
  target_include_directories(midibn_platform_virtual PUBLIC "${V2}/platform/virtual")
  target_link_libraries(midibn_platform_virtual PUBLIC midibn_core)

  add_library(midibn_engine_harness STATIC ${HARNESS_SOURCES})
  target_include_directories(midibn_engine_harness PUBLIC "${V2}/harness/engine")
  target_link_libraries(midibn_engine_harness PUBLIC midibn_core)

  function(midibn_test name)   # name, then sources relative to v2/, then optional LIBS libs...
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    set(sources "")
    foreach(s ${T_SOURCES})
      list(APPEND sources "${V2}/${s}")
    endforeach()
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE "${V2}/tools")
    target_link_libraries(${name} PRIVATE midibn_platform_android ${T_LIBS})
  endfunction()

  midibn_test(midibn-tests SOURCES tests/unit_tests.cpp LIBS midibn_decimal_oracle)
  midibn_test(midibn-ui-tests SOURCES tests/ui_tests.cpp)
  midibn_test(midibn-channels-app-tests SOURCES tests/channels_app_tests.cpp)
  midibn_test(midibn-app-tests SOURCES tests/app_tests.cpp)
  midibn_test(midibn-analysis-app-tests SOURCES tests/analysis_app_tests.cpp)
  midibn_test(midibn-routing-app-tests SOURCES tests/routing_app_tests.cpp)
  midibn_test(midibn-engine-tests SOURCES tests/engine_tests.cpp LIBS midibn_platform_virtual midibn_engine_harness)
  midibn_test(midibn-linux-platform-tests SOURCES tests/linux_platform_tests.cpp)
  midibn_test(midibn-route-tests SOURCES platform/android/route/tests/route_tests.cpp)
  midibn_test(midibn-synth-tests SOURCES platform/android/synth/tests/synth_tests.cpp)
  midibn_test(midibn-parity SOURCES tools/parity_main.cpp tools/engine_compare.cpp tools/difference_rules.cpp
    LIBS midibn_decimal_oracle midibn_engine_harness)
  midibn_test(midibn-engine-harness SOURCES tools/engine_harness_main.cpp tools/engine_compare.cpp tools/difference_rules.cpp
    LIBS midibn_engine_harness)
endif()
