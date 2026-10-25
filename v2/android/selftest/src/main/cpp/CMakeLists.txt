# MIDIBottleneck Self-test (Android), native build (U14). Own CMake; v2/android/native/CMakeLists.txt is not touched.
#
# Builds, for each ABI of the Gradle build:
#   libselftest_jni.so      the small JNI library the app loads (runs the test programs, loads a picked MIDI file)
#   libst_<name>.so         one shared library per test program (core unit tests, UI/app tests, engine tests, platform
#                           tests, the v2 baseline "parity" program, the engine harness, the capability report). Each is the
#                           program's own source, unchanged; its main() is simply exported from the library and the JNI
#                           library calls it (dlopen/dlsym) in a forked child process whose output goes to a file.
# The source lists of the portable core and of the engine harness are read from the root v2/CMakeLists.txt, the same way
# v2/android/native/CMakeLists.txt does, so a source added there is picked up here without a second list.
cmake_minimum_required(VERSION 3.22)
project(midibn_selftest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra)
# Same results as the desktop builds: no fused multiply-add (see native/CMakeLists.txt).
add_compile_options(-ffp-contract=off)
add_link_options(-Wl,-z,max-page-size=16384)

get_filename_component(V2 "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

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
target_compile_options(midibn_core PRIVATE -Wno-logical-op-parentheses -Wno-unused-function)
if(ANDROID_ABI STREQUAL "armeabi-v7a")
  target_link_libraries(midibn_core PUBLIC atomic)
endif()

# Clock, wake event, waiter, threads, files and memory: the Linux layer's POSIX code, unchanged, plus the Android part.
add_library(midibn_platform_android STATIC
  "${V2}/platform/linux/linux_platform.cpp"
  "${V2}/platform/android/android_platform.cpp")
target_include_directories(midibn_platform_android PUBLIC "${V2}/platform/linux" "${V2}/platform/android")
target_link_libraries(midibn_platform_android PUBLIC midibn_core)

add_library(midibn_decimal_oracle STATIC "${V2}/core/src/net_decimal.cpp")
target_include_directories(midibn_decimal_oracle PUBLIC "${V2}/core/include")

add_library(midibn_platform_virtual STATIC "${V2}/platform/virtual/lockstep_services.cpp")
target_include_directories(midibn_platform_virtual PUBLIC "${V2}/platform/virtual")
target_link_libraries(midibn_platform_virtual PUBLIC midibn_core)

add_library(midibn_engine_harness STATIC ${HARNESS_SOURCES})
target_include_directories(midibn_engine_harness PUBLIC "${V2}/harness/engine")
target_link_libraries(midibn_engine_harness PUBLIC midibn_core)

# ---- the test programs, one shared library each (libst_<name>.so) -----------------------------------------------------
function(selftest_program name)   # name, SOURCES (relative to v2/ unless absolute), LIBS
  cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
  set(sources "")
  foreach(s ${T_SOURCES})
    if(IS_ABSOLUTE "${s}")
      list(APPEND sources "${s}")
    else()
      list(APPEND sources "${V2}/${s}")
    endif()
  endforeach()
  add_library(st_${name} SHARED ${sources})
  target_include_directories(st_${name} PRIVATE "${V2}/tools")
  target_link_libraries(st_${name} PRIVATE midibn_platform_android ${T_LIBS})
endfunction()

selftest_program(unit      SOURCES tests/unit_tests.cpp LIBS midibn_decimal_oracle)
selftest_program(ui        SOURCES tests/ui_tests.cpp)
selftest_program(channels  SOURCES tests/channels_app_tests.cpp)
selftest_program(app       SOURCES tests/app_tests.cpp)
selftest_program(analysis  SOURCES tests/analysis_app_tests.cpp)
selftest_program(routing   SOURCES tests/routing_app_tests.cpp)
selftest_program(engine    SOURCES tests/engine_tests.cpp LIBS midibn_platform_virtual midibn_engine_harness)
selftest_program(platform  SOURCES tests/linux_platform_tests.cpp)
selftest_program(parity    SOURCES tools/parity_main.cpp tools/engine_compare.cpp tools/difference_rules.cpp
  LIBS midibn_decimal_oracle midibn_engine_harness)
selftest_program(harness   SOURCES tools/engine_harness_main.cpp tools/engine_compare.cpp tools/difference_rules.cpp
  LIBS midibn_engine_harness)
selftest_program(capability SOURCES "${CMAKE_CURRENT_LIST_DIR}/android_capability_main.cpp")

# tests/linux_platform_tests.cpp calls pthread_getname_np, which Bionic declares from API 26. The app itself supports
# API 23; the Java side runs this one program only on API 26+. Compiled with the API 26 declarations so the same source is
# used unchanged (the symbol is looked up when the program starts, which never happens below API 26).
if(DEFINED ANDROID_PLATFORM_LEVEL AND ANDROID_PLATFORM_LEVEL LESS 26)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(ST_TRIPLE "aarch64-none-linux-android26")
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(ST_TRIPLE "armv7-none-linux-androideabi26")
  else()
    set(ST_TRIPLE "x86_64-none-linux-android26")
  endif()
  target_compile_options(st_platform PRIVATE "--target=${ST_TRIPLE}")
  # The API 26 symbol is resolved at run time, so the link must accept it as undefined at API 23.
  target_link_options(st_platform PRIVATE -Wl,-z,undefs)
endif()

# ---- the JNI library loaded by the app ---------------------------------------------------------------------------------
add_library(selftest_jni SHARED "${CMAKE_CURRENT_LIST_DIR}/selftest_jni.cpp")
target_link_libraries(selftest_jni PRIVATE midibn_platform_android log dl)
