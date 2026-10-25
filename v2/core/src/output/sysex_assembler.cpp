#include "midibn/sysex_assembler.h"

#include <algorithm>
#include <cstdio>

namespace midibn {
namespace output {

namespace {

SystemExclusiveFragment make_fragment(const SystemExclusiveEvent& event)
{
    // v1 CreateFragment (SystemExclusiveAssembler.cs lines 85-94).
    SystemExclusiveFragment fragment;
    fragment.status = event.status;
    fragment.data_length = static_cast<int32_t>(event.length);
    fragment.event_index = event.event_index;
    fragment.tick = event.tick;
    fragment.micros = event.micros;
    return fragment;
}

std::string hex(unsigned long long value, int minDigits)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%0*llX", minDigits, value);
    return buffer;
}

std::string format_edge(const std::vector<uint8_t>* bytes, bool leading)
{
    // v1 FormatEdge: up to 16 bytes from the start or the end, "XX XX ...".
    if (!bytes || bytes->empty()) return "(empty)";
    size_t count = std::min<size_t>(16, bytes->size());
    size_t start = leading ? 0 : bytes->size() - count;
    std::string result;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) result += ' ';
        result += hex((*bytes)[start + i], 2);
    }
    return result;
}

std::string file_name_of(const std::string& path)
{
    size_t cut = path.find_last_of("\\/:");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

} // namespace

// v1 AcceptPacket (SystemExclusiveAssembler.cs lines 32-62).
bool SystemExclusiveAssembler::accept(const SystemExclusiveEvent& event, SystemExclusivePacket& packet)
{
    if (event.status == 0xF0) {
        pending_ = true;
        bytes_.assign(event.data, event.data + event.length);
        fragments_.clear();
        fragments_.push_back(make_fragment(event));
        return finish_if_complete(packet);
    }
    if (pending_) {
        bytes_.insert(bytes_.end(), event.data, event.data + event.length);
        fragments_.push_back(make_fragment(event));
        return finish_if_complete(packet);
    }
    if (is_complete(event.data, event.length)) {
        packet.bytes.assign(event.data, event.data + event.length);
        packet.fragments.clear();
        packet.fragments.push_back(make_fragment(event));
        return true;
    }
    return false;
}

// v1 Reset (lines 64-68).
void SystemExclusiveAssembler::reset()
{
    pending_ = false;
    bytes_.clear();
    fragments_.clear();
}

// v1 FinishIfComplete (lines 70-83). When the buffered bytes end in F7 but do
// not start with F0 the pending packet is dropped and nothing is returned.
bool SystemExclusiveAssembler::finish_if_complete(SystemExclusivePacket& packet)
{
    if (!pending_ || bytes_.size() < 2 || bytes_.back() != 0xF7) return false;
    std::vector<uint8_t> bytes;
    bytes.swap(bytes_);
    std::vector<SystemExclusiveFragment> fragments;
    fragments.swap(fragments_);
    pending_ = false;
    if (!is_complete(bytes.data(), bytes.size())) return false;
    packet.bytes.swap(bytes);
    packet.fragments.swap(fragments);
    return true;
}

// v1 IsComplete (lines 96-99).
bool SystemExclusiveAssembler::is_complete(const uint8_t* bytes, size_t length)
{
    return bytes != nullptr && length >= 2 && bytes[0] == 0xF0 && bytes[length - 1] == 0xF7;
}

// v1 SystemExclusiveDiagnostics.DescribeFailure (SystemExclusiveDiagnostics.cs lines 10-47).
std::string describe_sysex_failure(const std::string& native_operation, uint32_t error_code,
    const std::string& native_text, const std::string& source_file, const SystemExclusivePacket* packet,
    const SystemExclusiveEvent& final_event, int32_t header_size, uint32_t buffer_length, uint32_t bytes_recorded,
    uint32_t flags, uint64_t data_pointer, int pointer_size)
{
    static const char kNewLine[] = "\r\n";
    // UTF-8 for the micro sign, the right arrow and the horizontal ellipsis v1 prints.
    static const char kMicroSeconds[] = " \xC2\xB5" "s";
    static const char kArrow[] = " \xE2\x86\x92 ";
    static const char kEllipsis[] = "F0\xE2\x80\xA6" "F7";

    std::string text = "MIDI System Exclusive failure during " + native_operation + " (native code " +
        std::to_string(error_code) + ")";
    if (!native_text.empty()) text += ": " + native_text;
    text += kNewLine;
    text += "Source: " + (source_file.empty() ? std::string("unknown") : file_name_of(source_file));
    text += "; event #" + std::to_string(final_event.event_index) + "; tick " + std::to_string(final_event.tick) +
        "; source time " + std::to_string(final_event.micros) + kMicroSeconds;
    text += kNewLine;
    text += "Fragments: ";
    if (!packet || packet->fragments.empty()) text += "none recorded";
    else {
        for (size_t i = 0; i < packet->fragments.size(); ++i) {
            if (i > 0) text += kArrow;
            const SystemExclusiveFragment& fragment = packet->fragments[i];
            text += "F" + hex(fragment.status & 0x0F, 1) + "[#" + std::to_string(fragment.event_index) + ",tick=" +
                std::to_string(fragment.tick) + ",bytes=" + std::to_string(fragment.data_length) + "]";
        }
    }
    const std::vector<uint8_t>* bytes = packet ? &packet->bytes : nullptr;
    text += kNewLine;
    text += "Packet: " + std::to_string(bytes ? bytes->size() : 0) + " bytes; framed " + kEllipsis + ": " +
        (bytes && SystemExclusiveAssembler::is_complete(bytes->data(), bytes->size()) ? "yes" : "no") +
        "; leading " + format_edge(bytes, true) + "; trailing " + format_edge(bytes, false);
    text += kNewLine;
    text += "MIDIHDR: size=" + std::to_string(header_size) + ", dwBufferLength=" + std::to_string(buffer_length) +
        ", dwBytesRecorded=" + std::to_string(bytes_recorded) + ", dwFlags=0x" + hex(flags, 8) + ", data=0x" +
        hex(data_pointer, 1) + ", pointer alignment=" +
        (data_pointer == 0 ? std::string("-1") : std::to_string(data_pointer & static_cast<uint64_t>(pointer_size - 1)));
    return text;
}

} // namespace output
} // namespace midibn
