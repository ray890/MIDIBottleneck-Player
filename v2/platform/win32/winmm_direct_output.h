// Direct WinMM driver access, part 2 (register entry DD-085): a MIDI output that skips midiOutShortMsg /
// midiOutLongMsg and calls the installed user-mode MIDI driver's own modMessage entry point.
//
// open() finds the driver that serves the selected WinMM device (midi_driver_discovery.h), asks it to open that
// device with MODM_OPEN (a callback this program controls receives MOM_DONE for SysEx buffers), and the sends are
// MODM_DATA (short messages) and MODM_PREPARE / MODM_LONGDATA / MODM_UNPREPARE (SysEx; prepare and unprepare are done
// here when the driver answers "not supported", as winmm does). close() sends MODM_RESET and MODM_CLOSE.
//
// A device that cannot be driven this way (Windows' own driver, the MIDI Mapper, an unknown driver, a driver that
// refuses) makes open() fail with OutputErrorKind::NotSupported and a plain-language reason; the player then uses the
// standard WinMM output and says so (PlayerController). A failure while playing is an ordinary output error.
//
// Threading: sends, reset and panic come from one thread at a time (the engine's worker, or the caller when no worker
// runs); open and close come from the owner (the player's window thread, a tool's main thread). Nothing serialises the
// two in winmm here, so this class does (DD-094): a close never sends MODM_RESET / MODM_CLOSE while another thread is
// still inside the driver for this device (a driver can block inside MODM_DATA while its queue is full; resetting or
// closing underneath that call could hang the driver). close() waits a bounded time for that call; if it has not
// returned, close() reports so at once and the thread that is inside the driver resets and closes the device itself as
// soon as its call returns. From the moment a close is asked for, nothing more is sent (sends fail, reset and panic do
// nothing) and a busy-wait retry gives up; open() refuses while such a close is still pending. The gate has no lock.
// Per-message cost (DD-110): send_short, the call made for every MIDI message, costs ONE interlocked operation (the flag
// that says "a send is inside the driver" is set with an exchange, which is also the barrier that orders it before the
// look at the closed flag) and a plain store on the way out; the earlier form used two interlocked operations, which are
// several times dearer on a Pentium 4 than on a current processor. Every other public call (reset, panic, SysEx) uses
// the counted form, two interlocked operations, as before.
//
// Keeping the device open (user decision 5, DD-111): open() for the device that is already open does not close and reopen
// it; the player's Stop sends the usual reset and note-off messages and leaves the device open, and the next Play finds
// it ready. The device closes on a different device, a different path (the player calls close()), at exit, or when a
// driver call did not return in time (DD-094, below).
//
// Time-limited reset and close (DD-094, second part): the device's reset (MODM_RESET) and MODM_CLOSE run on a
// short-lived helper thread; the calling thread waits at most 1.5 s (so an engine Stop, which waits 2 s, still ends with
// a clear message). A call that does not return in time is abandoned: the helper thread is left inside the driver (it
// keeps the driver loaded), nothing more is sent to this output, and the driver is recorded as having stopped answering
// (mark_driver_unresponsive): it is never called again in this process, so the next Play uses standard WinMM and the
// status line says why. The real OmniMIDI driver was seen to hang in this close step about once every 30-150 cycles of the
// stress test. XP-compatible.
#pragma once

#include "midi_driver_discovery.h"
#include "midibn/midi_output.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace win32 {

struct LongBuffer;
struct DirectCallbackState;

class WinMmDirectOutput : public platform::MidiOutput {
public:
    typedef std::function<DirectResolution(uint32_t winmmDeviceId)> Resolver;
    typedef std::function<std::wstring(uint32_t code)> ErrorText;

    WinMmDirectOutput();   // resolves with resolve_direct_device (the real thing)
    // A test boundary: the resolver supplies a (fake) modMessage, nothing is loaded.
    explicit WinMmDirectOutput(Resolver resolver, ErrorText errorText = ErrorText());
    ~WinMmDirectOutput() override;
    WinMmDirectOutput(const WinMmDirectOutput&) = delete;
    WinMmDirectOutput& operator=(const WinMmDirectOutput&) = delete;

    const char* backend_name() const override { return "WinMM (direct driver access)"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override;
    platform::OutputStatus open(uint32_t deviceId) override;
    platform::OutputStatus close() override;
    bool is_open() const override { return open_.load(std::memory_order_acquire); }
    platform::OutputStatus send_short(uint32_t packed) override;
    bool try_send_short(uint32_t packed, platform::OutputStatus& failure) override;   // no status object on success
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    // DD-110: a burst of send_short calls under one entry into the call gate (zero interlocked operations per
    // message). A close asked for during the batch is waited for until the batch ends (milliseconds); from that moment
    // every send_short of the batch is refused without reaching the driver.
    void begin_send_batch() override;
    void end_send_batch() override;
    platform::OutputStatus reset() override;
    platform::OutputStatus panic() override;
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { sourceFile_ = path_utf8; }
    std::string path_description() const override { return pathDescription_; }

    // State for tests, diagnostics and the check packages.
    const std::string& output_identity() const { return outputIdentity_; }
    const std::string& driver_file() const { return driverFile_; }
    bool driver_verified() const { return verified_; }
    const std::string& lifecycle() const { return lifecycle_; }
    size_t outstanding_long_messages() const { return longBuffers_.size(); }
    // MOM_DONE callbacks received for SysEx buffers since open (the driver-independent delivery check).
    uint32_t long_completions() const;
    // Sends that found the driver busy (MIDIERR_NOTREADY: its queue was full) and waited, and the total wait.
    uint64_t busy_waits() const { return busyWaits_; }
    double busy_wait_milliseconds() const { return busyWaitTicks_ * 1000.0 / ticksPerSecond(); }
    // DD-094: how long close() / open() wait for a call another thread has inside the driver (default 2000 ms, the
    // engine's worker-stop wait); the number of such calls right now; whether a close is waiting for one of them.
    void set_close_wait_milliseconds(int value) { closeWaitMilliseconds_ = value < 0 ? 0 : value; }
    uint32_t calls_in_progress() const;
    // DD-111: how many open() calls found the same device still open and did nothing (no MODM_CLOSE, no MODM_OPEN).
    uint32_t kept_open_count() const { return keptOpenCount_; }
    bool close_pending() const { return closePending_.load(std::memory_order_acquire); }

    // DD-094 time-limited reset and close.
    bool driver_stopped_answering() const override { return abandoned_.load(std::memory_order_acquire); }
    void set_control_wait_milliseconds(int value) { controlWaitMilliseconds_ = value < 1 ? 1 : value; }
    // How the device is reset (at Stop, Seek, Pause and before MODM_CLOSE). The player always uses DriverReset; the
    // other styles exist for the stress tool's comparison on a real driver (--close-style):
    //   DriverReset: MODM_RESET (winmm's midiOutReset), after `settle` ms of waiting when settle > 0;
    //   NotesOff:    no MODM_RESET; the 48 controller messages of Panic (all sound off, all notes off, sustain off) instead.
    enum class ResetStyle { DriverReset, NotesOff };
    void set_reset_style(ResetStyle style, int settleMilliseconds) { resetStyle_ = style; settleMilliseconds_ = settleMilliseconds < 0 ? 0 : settleMilliseconds; }
    // Diagnostics (the stress tool's watchdog and summary); any thread. Message ids are the MODM_* values of drv::.
    static const char* call_name(uint32_t message);
    uint32_t control_call_in_progress() const { return controlCall_.load(std::memory_order_acquire); }
    uint32_t control_call_elapsed_milliseconds() const;
    // Milliseconds from the return of the last MODM_DATA to the start of the current (or the abandoned) control call;
    // -1 when unknown. Only measured after set_track_data_time(true) (one GetTickCount per message).
    void set_track_data_time(bool value) { trackDataTime_ = value; }
    int32_t control_call_after_data_milliseconds() const { return controlAfterDataMs_.load(std::memory_order_acquire); }
    int64_t milliseconds_since_last_data() const;
    uint32_t stopped_answering_call() const { return abandonedCall_.load(std::memory_order_acquire); }
    int32_t stopped_answering_after_data_milliseconds() const { return abandonedAfterDataMs_.load(std::memory_order_acquire); }

private:
    bool control_call(uint32_t message, std::function<uint32_t()> body, uint32_t& result);
    bool reset_device();
    void abandon(uint32_t message);
    void release_abandoned();
    platform::OutputStatus stopped_answering_status() const;
    bool enter();
    void leave();
    // send_short's cheap form of enter/leave (see the top of this file): 0 = refused, 1 = the single-owner flag (one
    // interlocked operation in, a plain store out), 2 = the counted form (another thread was inside a send at that moment).
    uint32_t enter_send();
    void leave_send(uint32_t how);
    void settle_deferred_close();
    bool no_call_inside() const;
    void claim_deferred_close();
    bool wait_for_calls_to_finish();
    void finish_close();
    void release_driver();
    platform::OutputStatus shut_down();
    platform::OutputStatus still_closing_status() const;
    platform::OutputStatus error_status(uint32_t code, const std::string& action) const;
    uint32_t message_when_busy(uint32_t message, uintptr_t param1, uintptr_t param2);
    static double ticksPerSecond();
    platform::OutputStatus long_error(uint32_t code, const char* operation, const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent, const void* headerCopy) const;
    platform::OutputStatus send_long_packet(const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent);
    uint32_t prepare_buffer(LongBuffer* buffer);
    void unprepare_buffer(LongBuffer* buffer);
    void reclaim_completed_long_messages();
    void reclaim_all_long_messages();

    Resolver resolver_;
    ErrorText errorText_;
    ModMessageFn mod_ = nullptr;
    std::shared_ptr<void> keepAlive_;
    uint32_t local_ = 0;
    uintptr_t instance_ = 0;       // the driver's per-open value from MODM_OPEN
    std::atomic<bool> open_{ false };
    // DD-094 call gate: bit 31 = closed (no new driver call may start), bit 30 = a close waits for the call in progress,
    // bits 0..29 = public calls in progress. Starts closed.
    std::atomic<uint32_t> gate_{ 0x80000000u };
    std::atomic<uint32_t> sending_{ 0 };   // 1 while the thread that set it is inside send_short's driver call (single owner)
    uint32_t batchHow_ = 0;                // the sending thread only: how begin_send_batch entered (0 = not in a batch)
    uint32_t keptOpenCount_ = 0;          // open() calls answered by the device that was already open (tests, diagnostics)
    uint32_t openedDeviceId_ = 0;         // the WinMM device id the open device was opened for (keep-open, DD-111)
    std::atomic<bool> closePending_{ false };
    int closeWaitMilliseconds_ = 2000;
    int controlWaitMilliseconds_ = 1500;
    ResetStyle resetStyle_ = ResetStyle::DriverReset;
    int settleMilliseconds_ = 0;
    std::atomic<bool> abandoned_{ false };
    std::atomic<uint32_t> abandonedCall_{ 0 };
    std::atomic<int32_t> abandonedAfterDataMs_{ -1 };
    std::atomic<uint32_t> controlCall_{ 0 };
    std::atomic<uint32_t> controlCallStartTick_{ 0 };
    std::atomic<int32_t> controlAfterDataMs_{ -1 };
    bool trackDataTime_ = false;
    std::atomic<uint32_t> lastDataTick_{ 0 };   // GetTickCount when the last MODM_DATA returned; 0 = none yet
    std::string deviceName_;
    std::string driverFile_;
    std::string lifecycle_;
    bool verified_ = false;
    std::string outputIdentity_;
    std::string pathDescription_;
    std::string sourceFile_;
    std::shared_ptr<DirectCallbackState> callbackState_;
    uintptr_t callbackToken_ = 0;
    std::vector<LongBuffer*> longBuffers_;
    output::SystemExclusiveAssembler assembler_;
    uint64_t busyWaits_ = 0;
    int64_t busyWaitTicks_ = 0;
};

} // namespace win32
} // namespace midibn
