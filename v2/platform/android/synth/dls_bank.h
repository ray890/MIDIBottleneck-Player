// A DLS sound bank for the built-in synthesizer (Sonivox): loaded from a file descriptor or from memory on any thread,
// used by the render thread. No Android classes here, so the tests run on any host.
//
// Why a holder and not a direct call. Sonivox is not thread-safe, and loading a bank means reading and converting the whole
// file (seconds for a big one). So the parsing happens on the caller's thread into memory of its own (Sonivox's parser only
// needs its own temporary host data), and the finished bank is then *handed to the render thread*: the render thread notices
// the new generation number before its next buffer and installs the bank itself (brief: it frees the sounding notes, swaps
// one pointer and looks every channel's instrument up again). Rendering is never paused and never waits for the file. The
// only lock is a mutex taken for those few microseconds by the loader (swap the pointer) and by the render thread (install);
// it protects Sonivox's reference count, which is not atomic. Nothing is saved: the bank lives in memory for as long as the
// process (or the DlsBank object, in tests) and is gone with it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace midibn {
namespace android {

class DlsBank {
public:
    // Largest file accepted (bytes). The bank is held in memory in Sonivox's own format, about as large as the file.
    static const uint64_t kMaxFileBytes = 192ull * 1024 * 1024;

    DlsBank();
    ~DlsBank();
    DlsBank(const DlsBank&) = delete;
    DlsBank& operator=(const DlsBank&) = delete;

    // ---- loader side (any thread, one at a time is enough) --------------------------------------------------------
    enum LoadResult { kLoaded, kNotSeekable, kFailed };
    // Reads a bank from an open, seekable file (pread; the descriptor is not closed and its position is not changed).
    // `name` is shown on the page. On kFailed `error` is one plain sentence and the previous bank stays in use.
    // kNotSeekable: the descriptor cannot be read at an offset (a pipe from a document provider); copy it to a file first.
    LoadResult load_fd(int fd, const std::string& name, std::string& error);
    // Same from memory (tests).
    bool load_memory(const uint8_t* data, size_t length, const std::string& name, std::string& error);
    // Back to the built-in sounds.
    void clear();

    // ---- readers ---------------------------------------------------------------------------------------------------
    bool loaded() const { return generation_loaded_.load(std::memory_order_acquire); }
    std::string name() const;
    // Changes whenever the bank changes (load or clear). Cheap; the render thread compares it before every buffer.
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }

    // ---- render thread ---------------------------------------------------------------------------------------------
    // Installs the current bank into a Sonivox stream (or the built-in sounds when none). eas = EAS_DATA_HANDLE,
    // stream = EAS_HANDLE. Takes the mutex; returns the generation that was installed.
    uint32_t install(void* eas, void* stream);
    // Takes the bank out of the stream before the stream is closed, so the reference count is touched under the mutex.
    void uninstall(void* eas, void* stream);

private:
    bool publish(void* bank, const std::string& name);

    mutable std::mutex lock_;
    void* bank_ = nullptr;          // the holder's own reference (Sonivox S_DLS*)
    std::string name_;
    std::atomic<uint32_t> generation_;
    std::atomic<bool> generation_loaded_;
};

} // namespace android
} // namespace midibn
