// Internal to platform/win32: a prepared System Exclusive buffer shared by the
// WinMM and KDMAPI backends. Port of the LongBuffer class and
// NativeMidiHeader.cs (v1) over the SDK MIDIHDR.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace midibn {
namespace win32 {

// v1 NativeMidiHeader.cs: the multimedia structures use the packed (1-byte)
// ABI, so MIDIHDR is 112 bytes on x64 and 64 on x86; the SDK header declares it
// the same way. The check keeps both in step with v1's explicit layout.
#if defined(_WIN64)
static_assert(sizeof(MIDIHDR) == 112, "MIDIHDR must match v1 NativeMidiHeader on x64");
#else
static_assert(sizeof(MIDIHDR) == 64, "MIDIHDR must match v1 NativeMidiHeader on x86");
#endif

const DWORD kMhDone = 0x00000001;   // v1 MhDone

// v1 LongBuffer (+ KdmApiMidiOutput's Prepared flag).
struct LongBuffer {
    uint8_t* data = nullptr;
    MIDIHDR* header = nullptr;
    bool prepared = false;
};

// v1 NativeMidiHeader.CreateOutput over freshly allocated native memory
// (v1 Marshal.AllocHGlobal; here operator new). Returns null when out of memory.
inline LongBuffer* allocate_long_buffer(const uint8_t* bytes, size_t length)
{
    LongBuffer* buffer = new (std::nothrow) LongBuffer();
    if (!buffer) return nullptr;
    buffer->data = new (std::nothrow) uint8_t[length];
    buffer->header = new (std::nothrow) MIDIHDR();
    if (!buffer->data || !buffer->header) {
        delete[] buffer->data;
        delete buffer->header;
        delete buffer;
        return nullptr;
    }
    std::memcpy(buffer->data, bytes, length);
    std::memset(buffer->header, 0, sizeof(MIDIHDR));
    buffer->header->lpData = reinterpret_cast<LPSTR>(buffer->data);
    buffer->header->dwBufferLength = static_cast<DWORD>(length);
    buffer->header->dwBytesRecorded = 0;
    return buffer;
}

// v1 FreeLongBuffer.
inline void free_long_buffer(LongBuffer* buffer)
{
    if (!buffer) return;
    delete buffer->header;
    delete[] buffer->data;
    delete buffer;
}

// True when the driver has set MHDR_DONE (v1 reads the flags back through
// Marshal.PtrToStructure; the driver writes them, so read them volatilely).
inline bool long_buffer_done(const LongBuffer& buffer)
{
    return (static_cast<volatile const MIDIHDR*>(buffer.header)->dwFlags & kMhDone) != 0;
}

} // namespace win32
} // namespace midibn
