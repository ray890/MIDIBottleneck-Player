#include "route_output.h"

#include <chrono>
#include <cstring>

namespace midibn {
namespace android {

using platform::OutputErrorKind;
using platform::OutputStatus;

size_t encode_short_message(uint32_t packed, uint8_t out[3])
{
    const uint8_t status = static_cast<uint8_t>(packed & 0xFFu);
    const uint8_t d1 = static_cast<uint8_t>((packed >> 8) & 0x7Fu);
    const uint8_t d2 = static_cast<uint8_t>((packed >> 16) & 0x7Fu);
    if (status < 0x80u) return 0;
    out[0] = status;
    if (status < 0xF0u) {
        const unsigned type = status & 0xF0u;
        out[1] = d1;
        if (type == 0xC0u || type == 0xD0u) return 2;
        out[2] = d2;
        return 3;
    }
    switch (status) {
    case 0xF1: case 0xF3: out[1] = d1; return 2;
    case 0xF2: out[1] = d1; out[2] = d2; return 3;
    case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 1;
    default: return 0;   // F0, F4, F5, F7, F9, FD
    }
}

namespace {

const size_t kTraceCapacity = 2048;   // records, power of two

struct TraceSlot {
    std::atomic<int64_t> start{0};
    std::atomic<int64_t> duration{0};
    std::atomic<int> route{0};
    std::atomic<int> bytes{0};
    std::atomic<int> result{0};
};

std::atomic<bool> g_trace_on{false};
std::atomic<uint64_t> g_trace_next{0};
TraceSlot g_trace[kTraceCapacity];

int64_t trace_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void write_trace_enable(bool on)
{
    if (on) g_trace_next.store(0, std::memory_order_relaxed);
    g_trace_on.store(on, std::memory_order_release);
}

bool write_trace_enabled() { return g_trace_on.load(std::memory_order_relaxed); }

void write_trace_record(const WriteTraceRecord& record)
{
    TraceSlot& slot = g_trace[g_trace_next.fetch_add(1, std::memory_order_relaxed) & (kTraceCapacity - 1)];
    slot.route.store(record.route, std::memory_order_relaxed);
    slot.start.store(record.start_ns, std::memory_order_relaxed);
    slot.duration.store(record.duration_ns, std::memory_order_relaxed);
    slot.bytes.store(record.bytes, std::memory_order_relaxed);
    slot.result.store(record.result, std::memory_order_relaxed);
}

std::vector<WriteTraceRecord> write_trace_snapshot()
{
    std::vector<WriteTraceRecord> out;
    const uint64_t next = g_trace_next.load(std::memory_order_relaxed);
    const uint64_t first = next > kTraceCapacity ? next - kTraceCapacity : 0;
    for (uint64_t i = first; i < next; ++i) {
        const TraceSlot& slot = g_trace[i & (kTraceCapacity - 1)];
        WriteTraceRecord r;
        r.route = slot.route.load(std::memory_order_relaxed);
        r.start_ns = slot.start.load(std::memory_order_relaxed);
        r.duration_ns = slot.duration.load(std::memory_order_relaxed);
        r.bytes = slot.bytes.load(std::memory_order_relaxed);
        r.result = slot.result.load(std::memory_order_relaxed);
        out.push_back(r);
    }
    return out;
}

bool BatchingOutput::timed_write(const uint8_t* data, size_t length, std::string& error)
{
    if (!g_trace_on.load(std::memory_order_relaxed)) return target_->write(data, length, error);
    const int64_t t0 = trace_now_ns();
    const bool ok = target_->write(data, length, error);
    WriteTraceRecord r;
    r.route = std::strcmp(target_->name(), "Native MIDI (AMidi)") == 0 ? 1 : 0;
    r.start_ns = t0;
    r.duration_ns = trace_now_ns() - t0;
    r.bytes = static_cast<int>(length);
    r.result = ok ? static_cast<int>(length) : -1;
    write_trace_record(r);
    return ok;
}

BatchingOutput::BatchingOutput(std::unique_ptr<RouteTarget> target) : target_(std::move(target)), lost_(false)
{
    buffer_.reserve(kMaxRouteWrite + 16);
}

void BatchingOutput::fail(const std::string& text, OutputStatus& failure)
{
    failed_ = true;
    failure_ = OutputStatus::failure(OutputErrorKind::IoError, text);
    failure = failure_;
}

bool BatchingOutput::check_ready(OutputStatus& failure)
{
    if (!open_) {
        failure = OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
        return false;
    }
    if (lost_.load(std::memory_order_acquire) && !failed_) {
        failed_ = true;
        failure_ = OutputStatus::failure(OutputErrorKind::IoError, lostMessage_);
    }
    if (failed_) { failure = failure_; return false; }
    return true;
}

void BatchingOutput::set_lost(const std::string& message)
{
    if (lost_.load(std::memory_order_relaxed)) return;
    lostMessage_ = message;
    lost_.store(true, std::memory_order_release);
}

OutputStatus BatchingOutput::open(uint32_t device_id)
{
    close();
    std::string error;
    if (!target_->open(device_id, error))
        return OutputStatus::failure(OutputErrorKind::IoError, error.empty() ? "The MIDI output could not be opened." : error);
    open_ = true;
    failed_ = false;
    inBatch_ = false;
    lost_.store(false);
    lostMessage_.clear();
    buffer_.clear();
    assembler_.reset();
    return OutputStatus::success();
}

OutputStatus BatchingOutput::close()
{
    assembler_.reset();
    if (open_ && !failed_ && !buffer_.empty()) {
        OutputStatus ignored;
        flush(ignored);
    }
    buffer_.clear();
    inBatch_ = false;
    open_ = false;
    failed_ = false;
    target_->close();
    return OutputStatus::success();
}

bool BatchingOutput::flush(OutputStatus& failure)
{
    if (buffer_.empty()) return true;
    std::string error;
    ++writes_;
    bool ok = timed_write(buffer_.data(), buffer_.size(), error);
    buffer_.clear();
    if (!ok) {
        fail(error.empty() ? "MIDI messages could not be sent to the device." : error, failure);
        return false;
    }
    return true;
}

bool BatchingOutput::write_chunked(const uint8_t* data, size_t length, OutputStatus& failure)
{
    while (length > 0) {
        size_t count = length > kMaxRouteWrite ? kMaxRouteWrite : length;
        std::string error;
        ++writes_;
        if (!timed_write(data, count, error)) {
            fail(error.empty() ? "MIDI messages could not be sent to the device." : error, failure);
            return false;
        }
        data += count;
        length -= count;
    }
    return true;
}

bool BatchingOutput::try_send_short(uint32_t packed, OutputStatus& failure)
{
    if (!open_ || failed_ || lost_.load(std::memory_order_relaxed)) {
        if (!check_ready(failure)) return false;
    }
    uint8_t bytes[3];
    size_t n = encode_short_message(packed, bytes);
    if (n == 0) {
        failure = OutputStatus::failure(OutputErrorKind::InvalidOperation, "This MIDI message cannot be sent to the device.");
        return false;
    }
    if (buffer_.size() + n > kMaxRouteWrite && !flush(failure)) return false;
    buffer_.insert(buffer_.end(), bytes, bytes + n);
    if (!inBatch_) return flush(failure);
    return true;
}

OutputStatus BatchingOutput::send_short(uint32_t packed)
{
    OutputStatus failure;
    if (try_send_short(packed, failure)) return OutputStatus::success();
    return failure;
}

void BatchingOutput::end_send_batch()
{
    inBatch_ = false;
    if (buffer_.empty()) return;
    OutputStatus failure;
    if (!open_ || failed_) { buffer_.clear(); return; }
    flush(failure);   // a failure is kept (failed_) and reported by the next send
}

OutputStatus BatchingOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    OutputStatus failure;
    if (!check_ready(failure)) return failure;
    if (fragment.length == 0) return OutputStatus::success();
    output::SystemExclusivePacket packet;
    if (!assembler_.accept(fragment, packet)) return OutputStatus::success();
    if (!flush(failure)) return failure;   // keep the order of earlier short messages
    if (!write_chunked(packet.bytes.data(), packet.bytes.size(), failure)) return failure;
    return OutputStatus::success();
}

OutputStatus BatchingOutput::reset()
{
    assembler_.reset();
    buffer_.clear();
    inBatch_ = false;
    if (open_ && !failed_ && !lost_.load(std::memory_order_relaxed)) target_->discard_pending();
    return OutputStatus::success();
}

OutputStatus BatchingOutput::panic()
{
    if (!open_) return OutputStatus::success();
    OutputStatus failure;
    if (!check_ready(failure)) return failure;
    buffer_.clear();
    inBatch_ = false;
    for (int i = 0; i < platform::kPanicMessageCount; ++i) {
        uint8_t bytes[3];
        size_t n = encode_short_message(platform::panic_message(i).packed, bytes);
        buffer_.insert(buffer_.end(), bytes, bytes + n);
    }
    if (!flush(failure))
        return OutputStatus::failure(OutputErrorKind::IoError, "The panic messages could not be sent: " + failure.message);
    return OutputStatus::success();
}

std::string BatchingOutput::path_description() const
{
    if (!open_) return std::string();
    std::string text = target_->describe();
    return text.empty() ? std::string("Android MIDI output.") : text;
}

} // namespace android
} // namespace midibn
