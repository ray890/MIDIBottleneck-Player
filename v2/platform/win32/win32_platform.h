// Win32 implementations of the core platform interfaces (XP-compatible APIs only).
#pragma once

#include "midibn/platform.h"

#include <cstdint>
#include <string>

namespace midibn {
namespace win32 {

class Win32FileSystem : public platform::FileSystem {
public:
    std::unique_ptr<platform::ByteSource> open_read(const std::string& path_utf8, std::string& error) override;
};

std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

struct ProcessMemory {
    uint64_t peak_working_set = 0;
    uint64_t peak_private = 0;     // peak pagefile-backed commit
    uint64_t current_private = 0;
};
ProcessMemory process_memory();

// "Windows <major>.<minor> build <n> <service pack> (<arch> process[, WOW64])"
std::string describe_os();

// High-resolution elapsed-time helper for measurements (tools only).
double milliseconds_since(int64_t start_counter);
int64_t performance_counter();

} // namespace win32
} // namespace midibn
