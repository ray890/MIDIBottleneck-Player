// Android MIDI output, shared part of both routes (Milestone 8, U12; ARCHITECTURE.md section 4).
//
// BatchingOutput is the midibn::platform::MidiOutput the engine talks to on Android. It turns every message into raw
// MIDI bytes, collects the bytes of one engine burst (begin_send_batch .. end_send_batch) in a native buffer and hands
// the whole buffer to a RouteTarget with ONE write. A RouteTarget is the last hop:
//   * AMidiRouteTarget (amidi_route_target.cpp, "modern" flavor): AMidiInputPort_send from C++.
//   * JavaRouteTarget (java_route_target.cpp, "android6" flavor): one JNI call to MidiRoute.sendBytes, which does one
//     MidiInputPort.send of the whole buffer.
//   * FakeRouteTarget (fake_route_target.h): records the writes; used by the tests.
// No locking inside (the MidiOutput contract): one thread at a time, except set_lost(), which any thread may call.
#pragma once

#include "midibn/midi_output.h"
#include "midibn/sysex_assembler.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace android {

// Largest single write handed to a target. A MIDI port takes about 1015 bytes per packet; whole messages are never
// split, a long SysEx message is cut into pieces of this size.
const size_t kMaxRouteWrite = 1000;

// Packed short message (status | data1 << 8 | data2 << 16) to raw bytes. Returns the byte count (1 to 3), or 0 when the
// value is not a sendable message (not a status byte, or F0, F4, F5, F7, F9, FD which are not short messages).
size_t encode_short_message(uint32_t packed, uint8_t out[3]);

// Write trace (P13-USB): a small lock-free ring of the latest writes to the device (bytes, time inside the write, result),
// for the overload investigation. Off by default; while off a write costs one relaxed atomic load. The Java side reads it
// (RouteNative.traceSnapshot) and dumps it to logcat with tag "MIDIBN-route". Times are CLOCK_MONOTONIC nanoseconds, the
// same clock as Java's System.nanoTime.
struct WriteTraceRecord {
    int route;          // 0 = Java route, 1 = native (AMidi) route
    int64_t start_ns;
    int64_t duration_ns;
    int bytes;
    int result;         // bytes written, or -1 on failure
};
void write_trace_enable(bool on);   // also clears the ring when turned on
bool write_trace_enabled();
void write_trace_record(const WriteTraceRecord& record);
std::vector<WriteTraceRecord> write_trace_snapshot();   // oldest first

class RouteTarget {
public:
    virtual ~RouteTarget() {}
    virtual const char* name() const = 0;
    // The devices this route can open, as far as native code knows them (may be empty).
    virtual std::vector<platform::OutputDeviceInfo> devices() const { return std::vector<platform::OutputDeviceInfo>(); }
    // Called by MidiOutput::open. A target whose device was opened on the Java side returns true when it is attached.
    virtual bool open(uint32_t device_id, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    // One write of whole messages (or a piece of a SysEx message), at most kMaxRouteWrite bytes. False on failure with
    // the plain-sentence reason in `error`.
    virtual bool write(const uint8_t* data, size_t length, std::string& error) = 0;
    // MidiOutput::reset: discard what the device side has queued, if the platform can (may do nothing).
    virtual void discard_pending() {}
    virtual std::string describe() const { return std::string(); }
};

class BatchingOutput : public platform::MidiOutput {
public:
    explicit BatchingOutput(std::unique_ptr<RouteTarget> target);

    const char* backend_name() const override { return target_->name(); }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return target_->devices(); }
    platform::OutputStatus open(uint32_t device_id) override;
    platform::OutputStatus close() override;
    bool is_open() const override { return open_; }
    platform::OutputStatus send_short(uint32_t packed) override;
    bool try_send_short(uint32_t packed, platform::OutputStatus& failure) override;
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    platform::OutputStatus reset() override;
    platform::OutputStatus panic() override;
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { sourceFile_ = path_utf8; }
    std::string path_description() const override;
    void begin_send_batch() override { inBatch_ = true; }
    void end_send_batch() override;

    // Any thread: the device is gone (unplugged, closed by the system). Every later send reports `message`.
    void set_lost(const std::string& message);
    // Bytes in the buffer not yet written, and the number of target writes so far (tests, benchmark).
    size_t pending_bytes() const { return buffer_.size(); }
    uint64_t write_count() const { return writes_; }
    RouteTarget* target() { return target_.get(); }

private:
    bool timed_write(const uint8_t* data, size_t length, std::string& error);
    bool flush(platform::OutputStatus& failure);
    bool write_chunked(const uint8_t* data, size_t length, platform::OutputStatus& failure);
    bool check_ready(platform::OutputStatus& failure);
    void fail(const std::string& text, platform::OutputStatus& failure);

    std::unique_ptr<RouteTarget> target_;
    std::vector<uint8_t> buffer_;
    bool open_ = false;
    bool inBatch_ = false;
    bool failed_ = false;           // a write failed: later sends report the same failure until close/open
    platform::OutputStatus failure_;
    std::atomic<bool> lost_;
    std::string lostMessage_;
    uint64_t writes_ = 0;
    std::string sourceFile_;
    output::SystemExclusiveAssembler assembler_;
};

} // namespace android
} // namespace midibn
