#include "dls_bank.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "eas.h"   // Sonivox public header: result numbers, handles

// The player's own DLS functions in sonivox_midi_stream.c (compiled with the Sonivox library).
extern "C" {
void* MIDIBN_ParseDLS(int (*readAt)(void*, void*, int, int), int (*sizeOf)(void*), void* handle, EAS_RESULT* pResult);
void MIDIBN_AddRefDLS(void* dls);
void MIDIBN_ReleaseDLS(void* dls);
EAS_RESULT MIDIBN_SetStreamDLS(EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle, void* dls);
}

namespace midibn {
namespace android {

namespace {

// ---- readers handed to Sonivox's parser. The parser reads a few bytes at a time, so a file is read through a window. -----

struct FdReader {
    int fd;
    int size;
    std::vector<uint8_t> window;
    int64_t window_at;
    int window_len;
    FdReader(int f, int s) : fd(f), size(s), window(256 * 1024), window_at(-1), window_len(0) {}
};

int fd_size(void* handle) { return static_cast<FdReader*>(handle)->size; }

int fd_read(void* handle, void* buffer, int offset, int count)
{
    FdReader* r = static_cast<FdReader*>(handle);
    if (offset < 0 || count < 0 || offset >= r->size) return 0;
    if (count > r->size - offset) count = r->size - offset;
    uint8_t* out = static_cast<uint8_t*>(buffer);
    int done = 0;
    while (done < count) {
        const int64_t at = static_cast<int64_t>(offset) + done;
        if (r->window_at < 0 || at < r->window_at || at >= r->window_at + r->window_len) {
            int64_t start = at;
            ssize_t n;
            do { n = pread(r->fd, r->window.data(), r->window.size(), static_cast<off_t>(start)); } while (n < 0 && errno == EINTR);
            if (n <= 0) { r->window_at = -1; r->window_len = 0; break; }
            r->window_at = start;
            r->window_len = static_cast<int>(n);
        }
        const int in_window = static_cast<int>(at - r->window_at);
        const int n = std::min(count - done, r->window_len - in_window);
        std::memcpy(out + done, r->window.data() + in_window, static_cast<size_t>(n));
        done += n;
    }
    return done;
}

struct MemReader {
    const uint8_t* data;
    int size;
};

int mem_size(void* handle) { return static_cast<MemReader*>(handle)->size; }

int mem_read(void* handle, void* buffer, int offset, int count)
{
    MemReader* r = static_cast<MemReader*>(handle);
    if (offset < 0 || count < 0 || offset >= r->size) return 0;
    if (count > r->size - offset) count = r->size - offset;
    std::memcpy(buffer, r->data + offset, static_cast<size_t>(count));
    return count;
}

// "RIFF" .... "DLS " ?
bool looks_like_dls(int (*read)(void*, void*, int, int), void* handle)
{
    uint8_t head[12];
    return read(handle, head, 0, 12) == 12 && std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "DLS ", 4) == 0;
}

const char* const kNotDls = "This file is not a DLS sound bank.";
const char* const kTooLarge = "This sound bank is too large to load on this phone.";
const char* const kUnsupported = "The built-in synth cannot use this DLS bank (it has more sounds, larger samples or features than the synth supports).";

const char* error_text(EAS_RESULT result)
{
    if (result == EAS_ERROR_MALLOC_FAILED) return kTooLarge;
    return kUnsupported;
}

} // namespace

DlsBank::DlsBank() : generation_(0), generation_loaded_(false) {}

DlsBank::~DlsBank()
{
    std::lock_guard<std::mutex> guard(lock_);
    MIDIBN_ReleaseDLS(bank_);
    bank_ = nullptr;
}

bool DlsBank::publish(void* bank, const std::string& name)
{
    void* old;
    {
        std::lock_guard<std::mutex> guard(lock_);
        old = bank_;
        bank_ = bank;
        name_ = bank ? name : std::string();
        MIDIBN_ReleaseDLS(old);   // the holder's reference; a synth that still shows the old bank keeps its own until it swaps
        generation_loaded_.store(bank != nullptr, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
    }
    return true;
}

DlsBank::LoadResult DlsBank::load_fd(int fd, const std::string& name, std::string& error)
{
    error.clear();
    const off_t end = lseek(fd, 0, SEEK_END);   // a pipe (document providers) cannot seek
    if (end < 0) return kNotSeekable;
    if (lseek(fd, 0, SEEK_SET) < 0) return kNotSeekable;
    if (static_cast<uint64_t>(end) > kMaxFileBytes) { error = kTooLarge; return kFailed; }
    if (end < 12) { error = kNotDls; return kFailed; }
    FdReader reader(fd, static_cast<int>(end));
    uint8_t probe;
    if (pread(fd, &probe, 1, 0) < 0) return kNotSeekable;
    if (!looks_like_dls(fd_read, &reader)) { error = kNotDls; return kFailed; }
    EAS_RESULT result = EAS_SUCCESS;
    void* bank = MIDIBN_ParseDLS(fd_read, fd_size, &reader, &result);
    if (!bank) { error = error_text(result); return kFailed; }
    publish(bank, name);
    return kLoaded;
}

bool DlsBank::load_memory(const uint8_t* data, size_t length, const std::string& name, std::string& error)
{
    error.clear();
    if (length > kMaxFileBytes) { error = kTooLarge; return false; }
    MemReader reader = {data, static_cast<int>(length)};
    if (length < 12 || !looks_like_dls(mem_read, &reader)) { error = kNotDls; return false; }
    EAS_RESULT result = EAS_SUCCESS;
    void* bank = MIDIBN_ParseDLS(mem_read, mem_size, &reader, &result);
    if (!bank) { error = error_text(result); return false; }
    publish(bank, name);
    return true;
}

void DlsBank::clear() { publish(nullptr, std::string()); }

std::string DlsBank::name() const
{
    std::lock_guard<std::mutex> guard(lock_);
    return name_;
}

uint32_t DlsBank::install(void* eas, void* stream)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (bank_) MIDIBN_AddRefDLS(bank_);
    if (MIDIBN_SetStreamDLS(static_cast<EAS_DATA_HANDLE>(eas), static_cast<EAS_HANDLE>(stream), bank_) != EAS_SUCCESS)
        MIDIBN_ReleaseDLS(bank_);
    return generation_.load(std::memory_order_acquire);
}

void DlsBank::uninstall(void* eas, void* stream)
{
    std::lock_guard<std::mutex> guard(lock_);
    MIDIBN_SetStreamDLS(static_cast<EAS_DATA_HANDLE>(eas), static_cast<EAS_HANDLE>(stream), nullptr);
}

} // namespace android
} // namespace midibn
