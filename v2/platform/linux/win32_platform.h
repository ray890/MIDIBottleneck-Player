// Non-Windows builds only: the Win32 class names the tests and tools use, mapped onto the Linux platform layer
// (linux_platform.h). The Windows build uses platform/win32/win32_platform.h instead.
#pragma once

#include "linux_platform.h"

namespace midibn {
namespace win32 {

using Win32FileSystem = posix::PosixFileSystem;
using ProcessMemory = posix::ProcessMemory;
using posix::describe_os;
using posix::milliseconds_since;
using posix::performance_counter;
using posix::process_memory;

} // namespace win32
} // namespace midibn
