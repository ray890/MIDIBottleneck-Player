// Direct WinMM driver access, part 2 (register entry DD-085). Structure follows winmm_midi_output.cpp (v1
// WindowsMidiOutput.cs); the native calls are the driver's modMessage instead of midiOut*. Nothing runs at load time.
#include "winmm_direct_output.h"

#include "midi_long_buffer.h"
#include "win32_platform.h"
#include "winmm_midi_output.h"

#include <process.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace midibn {
namespace win32 {

using platform::OutputDeviceInfo;
using platform::OutputErrorKind;
using platform::OutputStatus;

// The completion counts the callback fills in (the driver calls back on its own thread).
struct DirectCallbackState {
    std::atomic<uint32_t> done{ 0 };
    std::atomic<uint32_t> opened{ 0 };
    std::atomic<uint32_t> closed{ 0 };
};

namespace {

const uint32_t kCallbackFunction = 0x00030000;   // CALLBACK_FUNCTION
const uint32_t kMomOpen = 0x3C7, kMomClose = 0x3C8, kMomDone = 0x3C9;
const uint32_t kNotSupported = 8;                // MMSYSERR_NOTSUPPORTED
const uint32_t kNotReady = 67;                   // MIDIERR_NOTREADY: the driver's queue is full, try again shortly
const int kBusyLimitMilliseconds = 2000;         // a driver that stays busy this long is reported as an error
// DD-094 call gate (gate_): see the header.
const uint32_t kGateClosed = 0x80000000u;
const uint32_t kGateDeferred = 0x40000000u;
const uint32_t kGateCount = 0x3FFFFFFFu;
const uint32_t kClosedWhileBusy = 0xFFFF0001u;   // not a driver code: a busy-wait retry given up because of a close

#pragma pack(push, 1)
struct OpenStreamId { DWORD stream; UINT device; };
struct OpenDescription {   // MIDIOPENDESC (mmddk.h)
    HMIDI midi;
    DWORD_PTR callback;
    DWORD_PTR instance;
    DWORD_PTR devNode;
    DWORD ids;
    OpenStreamId streams[1];
};
#pragma pack(pop)

// The callback registry: the driver gets a number (never a pointer), so a late callback after close finds nothing.
std::mutex g_callback_mutex;
std::map<uintptr_t, std::shared_ptr<DirectCallbackState> >& callback_table()
{
    static std::map<uintptr_t, std::shared_ptr<DirectCallbackState> > table;
    return table;
}
uintptr_t g_next_token = 0x4D420001;

uintptr_t register_callback(const std::shared_ptr<DirectCallbackState>& state)
{
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    uintptr_t token = g_next_token++;
    callback_table()[token] = state;
    return token;
}

void unregister_callback(uintptr_t token)
{
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    callback_table().erase(token);
}

void CALLBACK direct_callback(HMIDIOUT, UINT message, DWORD_PTR instance, DWORD_PTR, DWORD_PTR)
{
    std::shared_ptr<DirectCallbackState> state;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        std::map<uintptr_t, std::shared_ptr<DirectCallbackState> >::iterator it = callback_table().find(instance);
        if (it == callback_table().end()) return;
        state = it->second;
    }
    if (message == kMomDone) state->done.fetch_add(1);
    else if (message == kMomOpen) state->opened.fetch_add(1);
    else if (message == kMomClose) state->closed.fetch_add(1);
}

// DD-094: a reset or close of the driver, run on a short-lived helper thread so the caller can stop waiting. The job
// holds everything the call needs by value (and keeps the driver loaded), so an abandoned helper never touches the
// output object, which may be gone by the time the driver returns, if it ever does.
struct ControlJob {
    std::function<uint32_t()> body;
    std::shared_ptr<void> keepAlive;
    uint32_t result = 0;
};

unsigned __stdcall control_job_entry(void* parameter)
{
    std::shared_ptr<ControlJob>* holder = static_cast<std::shared_ptr<ControlJob>*>(parameter);
    std::shared_ptr<ControlJob> job = *holder;
    delete holder;
    job->result = job->body();
    return 0;
}

// True when the call returned within the limit (job->result is set); false when the helper is still inside the driver.
bool run_with_time_limit(const std::shared_ptr<ControlJob>& job, int limitMilliseconds)
{
    std::shared_ptr<ControlJob>* holder = new std::shared_ptr<ControlJob>(job);
    uintptr_t thread = _beginthreadex(nullptr, 0, &control_job_entry, holder, 0, nullptr);
    if (thread == 0) {   // no thread could be started: make the call here, as before DD-094
        delete holder;
        job->result = job->body();
        return true;
    }
    const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(thread), static_cast<DWORD>(limitMilliseconds));
    CloseHandle(reinterpret_cast<HANDLE>(thread));
    return wait == WAIT_OBJECT_0;
}

// The NotesOff reset style: the 48 controller messages of Panic, waiting (bounded) while the driver says it is busy.
uint32_t send_notes_off(ModMessageFn fn, uint32_t local, uintptr_t instance)
{
    for (int i = 0; i < platform::kPanicMessageCount; ++i) {
        const uint32_t packed = platform::panic_message(i).packed;
        uint32_t result = guarded_mod_message(fn, local, drv::kModData, instance, packed, 0);
        for (DWORD start = GetTickCount(); result == kNotReady && GetTickCount() - start < static_cast<DWORD>(kBusyLimitMilliseconds);) {
            Sleep(1);
            result = guarded_mod_message(fn, local, drv::kModData, instance, packed, 0);
        }
        if (result != 0) return result;
    }
    return 0;
}

std::wstring native_error_text(uint32_t code)
{
    wchar_t text[256] = {};
    midiOutGetErrorTextW(code, text, 256);
    return std::wstring(text);
}

} // namespace

WinMmDirectOutput::WinMmDirectOutput()
    : resolver_([](uint32_t id) { return resolve_direct_device(id); }), errorText_(native_error_text)
{
}

WinMmDirectOutput::WinMmDirectOutput(Resolver resolver, ErrorText errorText)
    : resolver_(resolver), errorText_(errorText ? errorText : ErrorText(native_error_text))
{
}

// The owner keeps the object alive while any thread can still be inside one of its calls (the engine holds its output
// until its worker returns), so a close that is still pending here has its own thread to finish it.
WinMmDirectOutput::~WinMmDirectOutput() { shut_down(); }

std::vector<OutputDeviceInfo> WinMmDirectOutput::enumerate() const { return WinMmMidiOutput::enumerate_devices(); }

uint32_t WinMmDirectOutput::long_completions() const { return callbackState_ ? callbackState_->done.load() : 0; }

OutputStatus WinMmDirectOutput::error_status(uint32_t code, const std::string& action) const
{
    if (code == 0) return OutputStatus::success();
    if (code == kModFaulted)
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "The MIDI driver " + driverFile_ +
            " stopped working with a program error while " + action + ".", static_cast<int32_t>(code));
    if (code == kClosedWhileBusy)
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "The MIDI output was closed while the driver "
            "was still busy; the message was not sent (" + action + ").");
    std::string text = wide_to_utf8(errorText_(code));
    return OutputStatus::failure(OutputErrorKind::Win32, "MIDI error while " + action + " (native code " +
        std::to_string(code) + "): " + text, static_cast<int32_t>(code));
}

OutputStatus WinMmDirectOutput::long_error(uint32_t code, const char* operation, const output::SystemExclusivePacket& packet,
    const output::SystemExclusiveEvent& finalEvent, const void* headerCopy) const
{
    if (code == 0) return OutputStatus::success();
    const MIDIHDR& header = *static_cast<const MIDIHDR*>(headerCopy);
    std::string nativeText = code == kModFaulted ? std::string("the driver raised a program error")
                                                 : wide_to_utf8(errorText_(code));
    std::string detail = output::describe_sysex_failure(operation, code, nativeText, sourceFile_, &packet, finalEvent,
        static_cast<int32_t>(sizeof(MIDIHDR)), header.dwBufferLength, header.dwBytesRecorded, header.dwFlags,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(header.lpData)), static_cast<int>(sizeof(void*)));
    return OutputStatus::failure(OutputErrorKind::Win32, detail, static_cast<int32_t>(code));
}

OutputStatus WinMmDirectOutput::open(uint32_t deviceId)
{
    // DD-111 (keep the device open across Stop): the same device is still open from the last playback, healthy (not closing,
    // not abandoned): nothing to close and reopen. What a new playback needs is a clean SysEx state and no stale buffers.
    if (open_.load(std::memory_order_acquire) && !closePending_.load(std::memory_order_acquire) &&
        !abandoned_.load(std::memory_order_acquire) && openedDeviceId_ == deviceId && enter()) {
        assembler_.reset();
        if (resetStyle_ == ResetStyle::DriverReset) reclaim_all_long_messages();
        else reclaim_completed_long_messages();
        ++keptOpenCount_;
        leave();
        return OutputStatus::success();
    }
    // DD-094: while an earlier close still waits for a driver call, the device is not opened a second time.
    OutputStatus previous = shut_down();
    if (!previous.ok()) return previous;
    DirectResolution resolution = resolver_(deviceId);
    if (!resolution.ok || resolution.mod_message == nullptr)
        return OutputStatus::failure(OutputErrorKind::NotSupported,
            resolution.reason.empty() ? std::string("no driver was found for this device") : resolution.reason);
    std::string hung;
    if (driver_unresponsive(utf8_to_wide(resolution.driver_file), &hung))   // DD-094: never called again in this process
        return OutputStatus::failure(OutputErrorKind::NotSupported, "the synthesizer driver " + resolution.driver_file +
            " stopped answering earlier in this session (" + hung + "), so this program does not call it again until it is restarted");

    mod_ = resolution.mod_message;
    keepAlive_ = resolution.driver;
    local_ = resolution.local_index;
    deviceName_ = resolution.device_name;
    driverFile_ = resolution.driver_file;
    verified_ = resolution.verified;
    lifecycle_ = resolution.lifecycle;
    std::string identity = (deviceName_.empty() ? std::string("MIDI output ") + std::to_string(deviceId) : deviceName_) +
        " (device " + std::to_string(deviceId) + ", direct through " + (driverFile_.empty() ? std::string("a MIDI driver") : driverFile_) + ")";

    callbackState_ = std::make_shared<DirectCallbackState>();
    callbackToken_ = register_callback(callbackState_);
    OpenDescription description;
    std::memset(&description, 0, sizeof description);
    description.midi = reinterpret_cast<HMIDI>(static_cast<uintptr_t>(callbackToken_));   // stands in for winmm's handle
    description.callback = reinterpret_cast<DWORD_PTR>(&direct_callback);
    description.instance = static_cast<DWORD_PTR>(callbackToken_);
    uintptr_t instance = 0;
    uint32_t result = guarded_mod_message(mod_, local_, drv::kModOpen, reinterpret_cast<uintptr_t>(&instance),
        reinterpret_cast<uintptr_t>(&description), kCallbackFunction);
    if (result != 0) {
        OutputStatus status;
        if (result == kModFaulted) status = error_status(result, "opening " + identity);
        else {
            std::string text = wide_to_utf8(errorText_(result));
            status = OutputStatus::failure(OutputErrorKind::NotSupported, "the driver " + driverFile_ +
                " did not open the device when asked directly (native code " + std::to_string(result) + ": " + text +
                "); if another program is using the device, close it", static_cast<int32_t>(result));
        }
        unregister_callback(callbackToken_);
        callbackToken_ = 0;
        callbackState_.reset();
        keepAlive_.reset();
        mod_ = nullptr;
        return status;
    }
    instance_ = instance;
    outputIdentity_ = identity;
    pathDescription_ = "Direct WinMM driver access: " + (driverFile_.empty() ? std::string("MIDI driver") : driverFile_) +
        (verified_ ? " (tested driver)" : " (unverified driver: not tested)");
    abandoned_.store(false, std::memory_order_release);
    abandonedCall_.store(0, std::memory_order_release);
    abandonedAfterDataMs_.store(-1, std::memory_order_release);
    lastDataTick_.store(0, std::memory_order_relaxed);
    openedDeviceId_ = deviceId;
    open_.store(true, std::memory_order_release);
    sending_.store(0, std::memory_order_release);
    gate_.store(0, std::memory_order_release);   // calls may start now (they see every field written above)
    return OutputStatus::success();
}

// ---------------------------------------------------------------- DD-094 call gate

uint32_t WinMmDirectOutput::calls_in_progress() const
{
    return (gate_.load(std::memory_order_acquire) & kGateCount) + sending_.load(std::memory_order_acquire);
}

bool WinMmDirectOutput::no_call_inside() const { return calls_in_progress() == 0; }

// Every public call that may reach the driver is bracketed by enter() / leave(). enter() fails once a close was asked
// for; the thread whose leave() ends the last call during a deferred close finishes that close.
bool WinMmDirectOutput::enter()
{
    uint32_t prior = gate_.fetch_add(1, std::memory_order_acq_rel);
    if ((prior & kGateClosed) == 0) return true;
    leave();
    return false;
}

void WinMmDirectOutput::leave()
{
    uint32_t prior = gate_.fetch_sub(1, std::memory_order_acq_rel);
    if ((prior & kGateCount) == 1 && (prior & kGateDeferred) != 0 && sending_.load(std::memory_order_acquire) == 0)
        claim_deferred_close();   // (a send that holds the single-owner flag finishes the close itself when it leaves)
}

// send_short's form of enter()/leave() (DD-110). The thread that finds the flag free takes it with ONE exchange (the
// interlocked instruction that is also the barrier between "I am inside" and the look at the closed flag: a close sets
// the closed flag with an interlocked operation and then waits for the flag to clear, so one of the two always sees the
// other). It leaves with a plain store. A second thread that arrives while the flag is taken (not the normal use) gets
// the counted form, so it can never clear the first thread's flag.
uint32_t WinMmDirectOutput::enter_send()
{
    if (sending_.exchange(1, std::memory_order_acq_rel) == 0) {
        if ((gate_.load(std::memory_order_acquire) & kGateClosed) == 0) return 1;
        leave_send(1);
        return 0;
    }
    return enter() ? 2 : 0;
}

void WinMmDirectOutput::leave_send(uint32_t how)
{
    if (how == 2) { leave(); return; }
    sending_.store(0, std::memory_order_release);   // a plain store: no interlocked operation on the way out
    const uint32_t flags = gate_.load(std::memory_order_acquire);
    if ((flags & kGateDeferred) != 0 && (flags & kGateCount) == 0) claim_deferred_close();
}

// The close has given up waiting (a call is still inside the driver) and left the finishing to that call's thread. A
// thread that leaves send_short with a plain store may have looked at the flags before the close set its flag (a load
// can pass the thread's own store for a few nanoseconds); so look again for a short moment: if no call is inside any
// more, finish the close here (claim_deferred_close lets exactly one thread do it).
void WinMmDirectOutput::settle_deferred_close()
{
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    const int64_t limit = static_cast<int64_t>(ticksPerSecond() / 10000.0);   // 100 microseconds
    for (;;) {
        if (no_call_inside()) { claim_deferred_close(); return; }
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= limit) return;
        YieldProcessor();
    }
}

void WinMmDirectOutput::claim_deferred_close()
{
    // exactly one thread clears the flag and so finishes the close
    if ((gate_.fetch_and(~kGateDeferred, std::memory_order_acq_rel) & kGateDeferred) != 0) finish_close();
}

bool WinMmDirectOutput::wait_for_calls_to_finish()
{
    const DWORD start = GetTickCount();
    for (int attempt = 0; !no_call_inside(); ++attempt) {
        if (GetTickCount() - start >= static_cast<DWORD>(closeWaitMilliseconds_)) return false;
        if (attempt < 64) SwitchToThread();
        else Sleep(1);
    }
    return true;
}

// Runs with the gate closed and no call in progress, so this is the only thread talking to the driver. The reset and
// MODM_CLOSE are time-limited; after one that did not return, the driver is not called again (not even MODM_CLOSE).
void WinMmDirectOutput::finish_close()
{
    if (!abandoned_.load(std::memory_order_acquire) && reset_device()) {
        if (resetStyle_ == ResetStyle::DriverReset) reclaim_all_long_messages();
        else reclaim_completed_long_messages();
        assembler_.reset();
        const ModMessageFn fn = mod_;
        const uint32_t local = local_;
        const uintptr_t instance = instance_;
        uint32_t result = 0;
        control_call(drv::kModClose, [fn, local, instance] { return guarded_mod_message(fn, local, drv::kModClose, instance, 0, 0); }, result);
    }
    if (abandoned_.load(std::memory_order_acquire)) release_abandoned();
    else release_driver();
    closePending_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------- DD-094 time-limited reset and close

const char* WinMmDirectOutput::call_name(uint32_t message)
{
    switch (message) {
    case drv::kModReset: return "MODM_RESET";
    case drv::kModClose: return "MODM_CLOSE";
    case drv::kModData: return "MODM_DATA (all-notes-off messages)";
    case drv::kModOpen: return "MODM_OPEN";
    default: return "a driver call";
    }
}

uint32_t WinMmDirectOutput::control_call_elapsed_milliseconds() const
{
    return controlCall_.load(std::memory_order_acquire) == 0 ? 0 : GetTickCount() - controlCallStartTick_.load(std::memory_order_acquire);
}

int64_t WinMmDirectOutput::milliseconds_since_last_data() const
{
    const uint32_t last = lastDataTick_.load(std::memory_order_relaxed);
    return last == 0 ? -1 : static_cast<int64_t>(GetTickCount() - last);
}

// Runs `body` (one reset or close of the driver) on a helper thread and waits at most controlWaitMilliseconds_. False:
// the call did not return; the output is then abandoned (see abandon()).
bool WinMmDirectOutput::control_call(uint32_t message, std::function<uint32_t()> body, uint32_t& result)
{
    std::shared_ptr<ControlJob> job = std::make_shared<ControlJob>();
    job->body = std::move(body);
    job->keepAlive = keepAlive_;
    const uint32_t start = GetTickCount();
    const uint32_t lastData = lastDataTick_.load(std::memory_order_relaxed);
    controlAfterDataMs_.store(lastData == 0 ? -1 : static_cast<int32_t>(start - lastData), std::memory_order_release);
    controlCallStartTick_.store(start, std::memory_order_release);
    controlCall_.store(message, std::memory_order_release);
    const bool returned = run_with_time_limit(job, controlWaitMilliseconds_);
    controlCall_.store(0, std::memory_order_release);
    if (!returned) {
        abandon(message);
        return false;
    }
    result = job->result;
    return true;
}

// The device's reset in the chosen style (the player: MODM_RESET). False when the driver did not return in time.
bool WinMmDirectOutput::reset_device()
{
    if (settleMilliseconds_ > 0) Sleep(static_cast<DWORD>(settleMilliseconds_));
    const ModMessageFn fn = mod_;
    const uint32_t local = local_;
    const uintptr_t instance = instance_;
    uint32_t result = 0;   // ignored, as winmm ignores midiOutReset's answer here
    if (resetStyle_ == ResetStyle::NotesOff)
        return control_call(drv::kModData, [fn, local, instance] { return send_notes_off(fn, local, instance); }, result);
    return control_call(drv::kModReset, [fn, local, instance] { return guarded_mod_message(fn, local, drv::kModReset, instance, 0, 0); }, result);
}

// A reset or close did not return: nothing more is sent, the driver is never called again in this process, and the
// helper thread is left inside it (it holds its own reference that keeps the driver loaded).
void WinMmDirectOutput::abandon(uint32_t message)
{
    gate_.fetch_or(kGateClosed, std::memory_order_acq_rel);
    open_.store(false, std::memory_order_release);
    abandonedCall_.store(message, std::memory_order_release);
    abandonedAfterDataMs_.store(controlAfterDataMs_.load(std::memory_order_acquire), std::memory_order_release);
    mark_driver_unresponsive(utf8_to_wide(driverFile_), std::string(call_name(message)) + " did not return within " +
        std::to_string(controlWaitMilliseconds_) + " ms");
    abandoned_.store(true, std::memory_order_release);
}

// Forgets the abandoned device without calling the driver: no MODM_UNPREPARE (SysEx buffers the driver may still hold
// are left allocated on purpose), no MODM_CLOSE.
void WinMmDirectOutput::release_abandoned()
{
    longBuffers_.clear();
    assembler_.reset();
    release_driver();
}

OutputStatus WinMmDirectOutput::stopped_answering_status() const
{
    return OutputStatus::failure(OutputErrorKind::InvalidOperation, "The MIDI driver " +
        (driverFile_.empty() ? std::string("of this device") : driverFile_) + " stopped answering (" +
        call_name(abandonedCall_.load(std::memory_order_acquire)) + " did not return within " +
        std::to_string(controlWaitMilliseconds_) + " ms). Nothing more is sent to it, and this program does not use it "
        "directly again until it is restarted; the next playback uses standard WinMM.");
}

void WinMmDirectOutput::release_driver()
{
    open_.store(false, std::memory_order_release);
    if (callbackToken_ != 0) unregister_callback(callbackToken_);
    callbackToken_ = 0;
    mod_ = nullptr;
    keepAlive_.reset();   // the driver object may unload / undo its start-up now
    instance_ = 0;
    outputIdentity_.clear();
    pathDescription_.clear();
}

OutputStatus WinMmDirectOutput::still_closing_status() const
{
    return OutputStatus::failure(OutputErrorKind::InvalidOperation,
        "The MIDI driver " + (driverFile_.empty() ? std::string("of this device") : driverFile_) + " has not returned from a call for " +
        std::to_string(closeWaitMilliseconds_) + " ms. Nothing more is sent to it; the device is reset and closed as soon "
        "as that call returns. Try again in a moment.");
}

// close(), open() and the destructor: reset and close the device, but never underneath a call another thread has inside
// the driver. The wait for such a call is bounded; after it the close is left to that call's thread.
OutputStatus WinMmDirectOutput::shut_down()
{
    if (closePending_.load(std::memory_order_acquire)) return still_closing_status();
    if (!open_.load(std::memory_order_acquire)) {
        // closed already, or abandoned by a reset that did not return (forgotten here, once that call's thread has left)
        if (mod_ != nullptr && wait_for_calls_to_finish()) {
            if (abandoned_.load(std::memory_order_acquire)) release_abandoned();
            else release_driver();
        }
        return OutputStatus::success();
    }
    gate_.fetch_or(kGateClosed, std::memory_order_acq_rel);   // from here on nothing more is sent
    open_.store(false, std::memory_order_release);
    if (wait_for_calls_to_finish()) {
        const bool wasAbandoned = abandoned_.load(std::memory_order_acquire);
        finish_close();
        if (!wasAbandoned && abandoned_.load(std::memory_order_acquire)) return stopped_answering_status();
        return OutputStatus::success();
    }
    closePending_.store(true, std::memory_order_release);
    uint32_t prior = gate_.fetch_or(kGateDeferred, std::memory_order_acq_rel);
    if ((prior & kGateCount) == 0) settle_deferred_close();   // the call returned just now: finish here (or it did)
    return closePending_.load(std::memory_order_acquire) ? still_closing_status() : OutputStatus::success();
}

double WinMmDirectOutput::ticksPerSecond()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

// A synthesizer that cannot keep up with the speed of a direct call answers MIDIERR_NOTREADY ("the port is still
// transmitting, wait and try again"; VirtualMIDISynth does when its queue is full). The system WinMM path is slower
// than any synthesizer, so it never meets this; here the caller waits, which paces playback to what the synthesizer can
// take instead of stopping the song with an error. The wait is bounded.
uint32_t WinMmDirectOutput::message_when_busy(uint32_t message, uintptr_t param1, uintptr_t param2)
{
    ++busyWaits_;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    const int64_t limit = static_cast<int64_t>(kBusyLimitMilliseconds * ticksPerSecond() / 1000.0);
    uint32_t result = kNotReady;
    for (int attempt = 0;; ++attempt) {
        if (attempt < 64) YieldProcessor();
        else if (attempt < 512) SwitchToThread();
        else Sleep(1);
        // DD-094: a close was asked for (Stop gave up waiting, the window closes): stop retrying, send nothing more
        if ((gate_.load(std::memory_order_acquire) & kGateClosed) != 0) {
            QueryPerformanceCounter(&now);
            result = kClosedWhileBusy;
            break;
        }
        result = mod_(local_, message, instance_, param1, param2);
        QueryPerformanceCounter(&now);
        if (result != kNotReady || now.QuadPart - start.QuadPart >= limit) break;
    }
    busyWaitTicks_ += now.QuadPart - start.QuadPart;
    return result;
}

OutputStatus WinMmDirectOutput::send_short(uint32_t packed)
{
    OutputStatus failure;   // stays the success value when try_send_short returns true
    try_send_short(packed, failure);
    return failure;
}

// The cheap success path (scan P11 item 1): no OutputStatus is built or destroyed per message; the status of a
// failure is built only when the send fails, with the same kind and text as before.
bool WinMmDirectOutput::try_send_short(uint32_t packed, OutputStatus& failure)
{
    // DD-110: inside a batch the gate was entered once by begin_send_batch; a close asked for since then is seen
    // here (a plain load: the batch's flag was published with an interlocked operation, so the close waits for it).
    // Otherwise one interlocked operation (see the top of the header).
    const bool inBatch = batchHow_ != 0;
    const uint32_t how = !inBatch ? enter_send() : (gate_.load(std::memory_order_acquire) & kGateClosed) == 0 ? 1u : 0u;
    if (how == 0) {
        failure = OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
        return false;
    }
    if (!longBuffers_.empty()) reclaim_completed_long_messages();
    uint32_t result = mod_(local_, drv::kModData, instance_, packed, 0);
    if (result == kNotReady) result = message_when_busy(drv::kModData, packed, 0);
    if (trackDataTime_) lastDataTick_.store(GetTickCount(), std::memory_order_relaxed);   // diagnostics only (stress tool)
    if (result == 0) {
        if (!inBatch) leave_send(how);   // last: a close that waited for this call may be finished here
        return true;
    }
    failure = error_status(result, "sending a short MIDI message through " + outputIdentity_);
    if (!inBatch) leave_send(how);
    return false;
}

// A burst of sends under one entry into the call gate. The thread that calls both is the sending thread. An entry that
// was refused (a close is under way) leaves no batch: the sends then take the ordinary form and are refused one by one.
void WinMmDirectOutput::begin_send_batch()
{
    if (batchHow_ == 0) batchHow_ = enter_send();
}

void WinMmDirectOutput::end_send_batch()
{
    const uint32_t how = batchHow_;
    batchHow_ = 0;
    if (how != 0) leave_send(how);
}

OutputStatus WinMmDirectOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    if (!enter())
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    OutputStatus status;
    if (fragment.length != 0) {
        reclaim_completed_long_messages();
        output::SystemExclusivePacket packet;
        if (assembler_.accept(fragment, packet)) status = send_long_packet(packet, fragment);
    }
    leave();
    return status;
}

uint32_t WinMmDirectOutput::prepare_buffer(LongBuffer* buffer)
{
    uint32_t result = guarded_mod_message(mod_, local_, drv::kModPrepare, instance_,
        reinterpret_cast<uintptr_t>(buffer->header), sizeof(MIDIHDR));
    if (result == kNotSupported) {
        // winmm's own rule: a driver without MODM_PREPARE leaves it to the caller's side of winmm, which marks the header.
        buffer->header->dwFlags |= MHDR_PREPARED;
        buffer->header->dwFlags &= ~static_cast<DWORD>(MHDR_DONE);
        result = 0;
    }
    return result;
}

void WinMmDirectOutput::unprepare_buffer(LongBuffer* buffer)
{
    uint32_t result = guarded_mod_message(mod_, local_, drv::kModUnprepare, instance_,
        reinterpret_cast<uintptr_t>(buffer->header), sizeof(MIDIHDR));
    if (result == kNotSupported) buffer->header->dwFlags &= ~static_cast<DWORD>(MHDR_PREPARED);
}

OutputStatus WinMmDirectOutput::send_long_packet(const output::SystemExclusivePacket& packet,
    const output::SystemExclusiveEvent& finalEvent)
{
    LongBuffer* buffer = allocate_long_buffer(packet.bytes.data(), packet.bytes.size());
    if (buffer == nullptr)
        return OutputStatus::failure(OutputErrorKind::OutOfMemory,
            "There is not enough memory to send a System Exclusive (SysEx) message to the MIDI device. "
            "Close other programs and try again.");
    MIDIHDR headerCopy = *buffer->header;
    uint32_t result = prepare_buffer(buffer);
    OutputStatus status = long_error(result, "MODM_PREPARE", packet, finalEvent, &headerCopy);
    if (!status.ok()) {
        free_long_buffer(buffer);
        return status;
    }
    result = guarded_mod_message(mod_, local_, drv::kModLongData, instance_, reinterpret_cast<uintptr_t>(buffer->header), sizeof(MIDIHDR));
    if (result == kNotReady) result = message_when_busy(drv::kModLongData, reinterpret_cast<uintptr_t>(buffer->header), sizeof(MIDIHDR));
    if (result != 0) {
        unprepare_buffer(buffer);
        status = long_error(result, "MODM_LONGDATA", packet, finalEvent, &headerCopy);
        free_long_buffer(buffer);
        return status;
    }
    longBuffers_.push_back(buffer);
    return OutputStatus::success();
}

OutputStatus WinMmDirectOutput::panic()
{
    if (!enter()) return OutputStatus::success();   // closed, or closing: the close itself sends MODM_RESET
    OutputStatus status;
    for (int i = 0; i < platform::kPanicMessageCount; ++i) {
        platform::PanicMessage message = platform::panic_message(i);
        uint32_t result = mod_(local_, drv::kModData, instance_, message.packed, 0);
        if (result == kNotReady) result = message_when_busy(drv::kModData, message.packed, 0);
        if (trackDataTime_) lastDataTick_.store(GetTickCount(), std::memory_order_relaxed);
        if (result != 0) {
            status = error_status(result, std::string("sending ") + message.name + " panic through " + outputIdentity_);
            break;
        }
    }
    leave();
    return status;
}

OutputStatus WinMmDirectOutput::reset()
{
    if (!enter()) return OutputStatus::success();   // closed, or closing (the close resets the device and the SysEx state)
    assembler_.reset();
    OutputStatus status;
    if (!reset_device()) status = stopped_answering_status();   // DD-094: time-limited; abandoned when it does not return
    else if (resetStyle_ == ResetStyle::DriverReset) reclaim_all_long_messages();
    else reclaim_completed_long_messages();
    leave();
    return status;
}

void WinMmDirectOutput::reclaim_completed_long_messages()
{
    if (longBuffers_.empty()) return;
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        if (long_buffer_done(*longBuffers_[i])) {
            unprepare_buffer(longBuffers_[i]);
            free_long_buffer(longBuffers_[i]);
            longBuffers_.erase(longBuffers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void WinMmDirectOutput::reclaim_all_long_messages()
{
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        unprepare_buffer(longBuffers_[i]);
        free_long_buffer(longBuffers_[i]);
    }
    longBuffers_.clear();
}

// DD-094: returns at once with a failure (the output counts as closed: nothing more is sent) when another thread's call
// is still inside the driver after the bounded wait; that thread then resets and closes the device.
OutputStatus WinMmDirectOutput::close() { return shut_down(); }

} // namespace win32
} // namespace midibn
