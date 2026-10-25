// MIDI output checks that need no device (Milestone 6). Nothing here calls
// midiOutOpen/ShortMsg/LongMsg/Reset or loads a KDMAPI provider: the WinMM
// backend runs on its injected native table, the KDMAPI backend on a fake
// provider, and the Null/recording backends are portable. The only files
// touched are small synthetic test files in a temporary directory (read, never
// loaded). Exit code 0 when all pass.
#include "midibn/midi_output.h"
#include "midibn/null_output.h"
#include "win32_error_text.h"
#include "kdmapi_midi_output.h"
#include "midi_long_buffer.h"
#include "midi_path_util.h"
#include "winmm_midi_output.h"
#include "win32_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

void check_text(const std::string& actual, const std::string& expected, const char* what)
{
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", what, expected.c_str(), actual.c_str());
    }
}

using namespace midibn;
using platform::OutputErrorKind;
using platform::OutputStatus;
using output::SystemExclusiveEvent;

std::vector<uint8_t> bytes_of(std::initializer_list<int> values)
{
    std::vector<uint8_t> result;
    for (int v : values) result.push_back(static_cast<uint8_t>(v));
    return result;
}

// An F0 or F7 event over `data` (kept alive by the caller).
SystemExclusiveEvent sysex(uint8_t status, const std::vector<uint8_t>& data, int index = 0, int64_t tick = 0, int64_t micros = 0)
{
    SystemExclusiveEvent event;
    event.status = status;
    event.data = data.data();
    event.length = data.size();
    event.event_index = index;
    event.tick = tick;
    event.micros = micros;
    return event;
}

// Feeds one event; returns the assembled packet bytes (empty when none).
bool feed(output::SystemExclusiveAssembler& assembler, uint8_t status, const std::vector<uint8_t>& data,
    output::SystemExclusivePacket& packet)
{
    return assembler.accept(sysex(status, data), packet);
}

// ---------------------------------------------------------------- packing and panic

void test_packing_and_panic()
{
    using platform::pack_short_message;
    const uint8_t note[] = { 0x90, 0x3C, 0x7F };
    check(pack_short_message(note, 3) == 0x007F3C90u, "pack: note on 90 3C 7F");
    check(pack_short_message(note, 2) == 0x00003C90u, "pack: two bytes");
    check(pack_short_message(note, 1) == 0x00000090u, "pack: one byte");
    const uint8_t longer[] = { 0xB5, 0x01, 0x02, 0x03 };
    check(pack_short_message(longer, 4) == 0x000201B5u, "pack: a fourth byte is ignored");
    check(pack_short_message(note, 0) == 0 && pack_short_message(nullptr, 3) == 0, "pack: empty and null give 0");

    // Panic order: for each channel CC120, CC123, CC64 = 0.
    bool order = true;
    for (int channel = 0; channel < 16; ++channel) {
        order = order && platform::panic_message(channel * 3).packed == (0xB0u | channel | (120u << 8));
        order = order && platform::panic_message(channel * 3 + 1).packed == (0xB0u | channel | (123u << 8));
        order = order && platform::panic_message(channel * 3 + 2).packed == (0xB0u | channel | (64u << 8));
    }
    check(order && platform::kPanicMessageCount == 48, "panic: 48 messages, CC120 CC123 CC64=0 for channels 0..15");
    check(platform::panic_message(0).packed == 0x0078B0u && platform::panic_message(1).packed == 0x007BB0u &&
        platform::panic_message(2).packed == 0x0040B0u && platform::panic_message(47).packed == 0x0040BFu,
        "panic: literal first and last messages");
    check_text(platform::panic_message(0).name, "CC120", "panic name 0");
    check_text(platform::panic_message(1).name, "CC123", "panic name 1");
    check_text(platform::panic_message(2).name, "sustain-off", "panic name 2");
}

// ---------------------------------------------------------------- SysEx assembler

void test_assembler()
{
    output::SystemExclusiveAssembler assembler;
    output::SystemExclusivePacket packet;

    // A complete F0...F7 in one F0 event.
    std::vector<uint8_t> whole = bytes_of({ 0xF0, 0x7E, 0x00, 0xF7 });
    check(feed(assembler, 0xF0, whole, packet) && packet.bytes == whole && packet.fragments.size() == 1 &&
        !assembler.pending(), "sysex: single complete F0 event");

    // F0 start, F7 event continues (payload ends with F7).
    packet = output::SystemExclusivePacket();
    check(!feed(assembler, 0xF0, bytes_of({ 0xF0, 0x01 }), packet) && assembler.pending(), "sysex: unfinished F0 stays pending");
    check(feed(assembler, 0xF7, bytes_of({ 0x02, 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0x01, 0x02, 0xF7 }) &&
        packet.fragments.size() == 2 && packet.fragments[0].status == 0xF0 && packet.fragments[1].status == 0xF7 &&
        !assembler.pending(), "sysex: F0 + F7 continuation assembles two fragments");

    // F7 event holding only the terminator.
    packet = output::SystemExclusivePacket();
    feed(assembler, 0xF0, bytes_of({ 0xF0, 0x01 }), packet);
    check(feed(assembler, 0xF7, bytes_of({ 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0x01, 0xF7 }), "sysex: bare F7 terminates");

    // A lone F0 byte (empty payload) needs a terminator to reach two bytes.
    packet = output::SystemExclusivePacket();
    check(!feed(assembler, 0xF0, bytes_of({ 0xF0 }), packet), "sysex: one-byte F0 is incomplete");
    check(feed(assembler, 0xF7, bytes_of({ 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0xF7 }), "sysex: F0 then F7 gives the two-byte packet");

    // Stray continuations without a pending F0 are dropped; a framed escape packet is sent directly.
    packet = output::SystemExclusivePacket();
    check(!feed(assembler, 0xF7, bytes_of({ 0xF7 }), packet), "sysex: stray bare F7 dropped");
    check(!feed(assembler, 0xF7, bytes_of({ 0x01, 0xF7 }), packet), "sysex: stray payload ending in F7 dropped (not framed by F0)");
    check(!feed(assembler, 0xF7, bytes_of({ 0x01, 0x02 }), packet), "sysex: stray payload dropped");
    check(feed(assembler, 0xF7, bytes_of({ 0xF0, 0x05, 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0x05, 0xF7 }) &&
        packet.fragments.size() == 1 && packet.fragments[0].status == 0xF7, "sysex: F7 event carrying F0...F7 is sent directly");

    // A new F0 discards an unfinished packet.
    packet = output::SystemExclusivePacket();
    feed(assembler, 0xF0, bytes_of({ 0xF0, 0x01 }), packet);
    check(feed(assembler, 0xF0, bytes_of({ 0xF0, 0x02, 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0x02, 0xF7 }) &&
        packet.fragments.size() == 1, "sysex: a second F0 discards the unfinished first");

    // Reset drops pending bytes.
    packet = output::SystemExclusivePacket();
    feed(assembler, 0xF0, bytes_of({ 0xF0, 0x01 }), packet);
    assembler.reset();
    check(!assembler.pending() && !feed(assembler, 0xF7, bytes_of({ 0x02, 0xF7 }), packet), "sysex: reset drops the pending packet");

    // v1 only tests the last byte: an F7 inside a chunk does not end the packet.
    packet = output::SystemExclusivePacket();
    check(!feed(assembler, 0xF0, bytes_of({ 0xF0, 0x01, 0xF7, 0x02 }), packet) && assembler.pending(), "sysex: F7 mid-chunk does not finish");
    check(feed(assembler, 0xF7, bytes_of({ 0xF7 }), packet) && packet.bytes == bytes_of({ 0xF0, 0x01, 0xF7, 0x02, 0xF7 }),
        "sysex: later F7 finishes the whole accumulated bytes");

    // Finished packets leave no pending state: the next bare F7 is stray again.
    packet = output::SystemExclusivePacket();
    check(!feed(assembler, 0xF7, bytes_of({ 0xF7 }), packet), "sysex: pending cleared after a finished packet");

    // is_complete rules.
    const uint8_t framed[] = { 0xF0, 0xF7 };
    const uint8_t open[] = { 0xF0, 0x01 };
    const uint8_t one[] = { 0xF0 };
    const uint8_t unframed[] = { 0x01, 0xF7 };
    using output::SystemExclusiveAssembler;
    check(SystemExclusiveAssembler::is_complete(framed, 2) && !SystemExclusiveAssembler::is_complete(open, 2) &&
        !SystemExclusiveAssembler::is_complete(one, 1) && !SystemExclusiveAssembler::is_complete(unframed, 2) &&
        !SystemExclusiveAssembler::is_complete(nullptr, 0), "sysex: IsComplete rules");
}

void test_sysex_diagnostics()
{
    output::SystemExclusivePacket packet;
    packet.bytes = bytes_of({ 0xF0, 0x01, 0x02, 0xF7 });
    output::SystemExclusiveFragment a, b;
    a.status = 0xF0; a.data_length = 2; a.event_index = 5; a.tick = 100; a.micros = 5000;
    b.status = 0xF7; b.data_length = 2; b.event_index = 6; b.tick = 200; b.micros = 9000;
    packet.fragments.push_back(a);
    packet.fragments.push_back(b);
    SystemExclusiveEvent final = sysex(0xF7, packet.bytes, 6, 200, 9000);
    std::string text = output::describe_sysex_failure("midiOutLongMsg", 68, "bad", "C:\\songs\\a.mid", &packet, final, 112, 4, 0, 0,
        0x1008, 8);
    check_text(text,
        "MIDI System Exclusive failure during midiOutLongMsg (native code 68): bad\r\n"
        "Source: a.mid; event #6; tick 200; source time 9000 \xC2\xB5s\r\n"
        "Fragments: F0[#5,tick=100,bytes=2] \xE2\x86\x92 F7[#6,tick=200,bytes=2]\r\n"
        "Packet: 4 bytes; framed F0\xE2\x80\xA6" "F7: yes; leading F0 01 02 F7; trailing F0 01 02 F7\r\n"
        "MIDIHDR: size=112, dwBufferLength=4, dwBytesRecorded=0, dwFlags=0x00000000, data=0x1008, pointer alignment=0",
        "sysex diagnostics text");
    std::string bare = output::describe_sysex_failure("PrepareLongData", 1, "", "", nullptr, final, 64, 0, 0, 0, 0, 4);
    check(bare.find("Source: unknown;") != std::string::npos && bare.find("none recorded") != std::string::npos &&
        bare.find("Packet: 0 bytes; framed F0\xE2\x80\xA6" "F7: no; leading (empty); trailing (empty)") != std::string::npos &&
        bare.find("pointer alignment=-1") != std::string::npos && bare.find("(native code 1)\r\n") != std::string::npos,
        "sysex diagnostics: no packet, no file, no native text, null pointer");
}

// ---------------------------------------------------------------- Null / recording / safety

class ScriptedOutput : public platform::NullMidiOutput {
public:
    OutputStatus resetResult, panicResult;
    std::string order;
    OutputStatus reset() override { order += "R"; return resetResult; }
    OutputStatus panic() override { order += "P"; return panicResult; }
};

void test_null_and_recording()
{
    platform::NullMidiOutput none;
    check(none.open(0).ok() && none.send_short(0x7F3C90).ok() && none.panic().ok() && none.reset().ok() &&
        none.sysex_supported() && none.enumerate().empty() && none.close().ok(), "Null: every call succeeds and does nothing");
    check_text(platform::NullMidiOutput::display_name(), "None", "Null display name");
    none.set_source_file("x.mid");
    none.close();
    check(none.source_file().empty(), "Null: close clears the source file (v1 Close)");

    platform::RecordingMidiOutput rec;
    OutputStatus closedShort = rec.send_short(1);
    check(!closedShort.ok() && closedShort.kind == OutputErrorKind::InvalidOperation &&
        closedShort.message == "No MIDI output device is open.", "Recording: not open -> v1 text");
    check(rec.panic().ok() && rec.messages.empty(), "Recording: panic while closed sends nothing (v1 returns)");
    rec.open(0);
    rec.send_short(0x7F3C90);
    check(rec.messages.size() == 1 && rec.messages[0].packed == 0x7F3C90u, "Recording: records a short message");
    {   // the default try_send_short forwards to send_short (an output without its own lean form)
        OutputStatus failure;
        check(rec.try_send_short(0x7F3C91, failure) && rec.messages.size() == 2 && rec.messages[1].packed == 0x7F3C91u,
            "Recording: the default try_send_short sends through send_short");
        rec.close();
        check(!rec.try_send_short(1, failure) && failure.kind == OutputErrorKind::InvalidOperation &&
            failure.message == "No MIDI output device is open.", "Recording: the default try_send_short passes the failure on");
        rec.open(0);
        rec.messages.pop_back();
    }
    rec.messages.clear();
    rec.panic();
    bool order = rec.messages.size() == 48;
    for (size_t i = 0; order && i < rec.messages.size(); ++i)
        order = rec.messages[i].kind == platform::RecordedMessage::Short &&
            rec.messages[i].packed == platform::panic_message(static_cast<int>(i)).packed;
    check(order, "Recording: panic records the 48 messages in v1 order");

    std::vector<uint8_t> a = bytes_of({ 0xF0, 0x41 }), b = bytes_of({ 0x10, 0xF7 });
    rec.messages.clear();
    rec.send_long(sysex(0xF0, a));
    check(rec.messages.empty(), "Recording: unfinished SysEx not sent");
    rec.send_long(sysex(0xF7, b));
    check(rec.messages.size() == 1 && rec.messages[0].kind == platform::RecordedMessage::Long &&
        rec.messages[0].bytes == bytes_of({ 0xF0, 0x41, 0x10, 0xF7 }), "Recording: complete SysEx sent as one long message");
    rec.send_long(sysex(0xF0, a));
    rec.reset();
    rec.send_long(sysex(0xF7, b));
    check(rec.messages.size() == 2 && rec.messages[1].kind == platform::RecordedMessage::Reset, "Recording: reset drops unfinished SysEx");

    // MidiOutputSafety.ResetAndSilence.
    ScriptedOutput safe;
    check(platform::reset_and_silence(&safe).ok() && safe.order == "RP", "ResetAndSilence: reset then panic");
    check(platform::reset_and_silence(nullptr).ok(), "ResetAndSilence: null output is a no-op");
    ScriptedOutput resetFails;
    resetFails.resetResult = OutputStatus::failure(OutputErrorKind::Win32, "reset boom", 7);
    OutputStatus s = platform::reset_and_silence(&resetFails);
    check(resetFails.order == "RP" && s.kind == OutputErrorKind::InvalidOperation &&
        s.message == "MIDI reset failed; a post-reset panic was still attempted." && s.inner == "reset boom",
        "ResetAndSilence: reset failure still runs panic and is reported");
    ScriptedOutput panicFails;
    panicFails.panicResult = OutputStatus::failure(OutputErrorKind::Win32, "panic boom", 9);
    s = platform::reset_and_silence(&panicFails);
    check(s.kind == OutputErrorKind::Win32 && s.message == "panic boom" && s.native_code == 9,
        "ResetAndSilence: panic failure alone is returned unchanged");
    ScriptedOutput bothFail;
    bothFail.resetResult = OutputStatus::failure(OutputErrorKind::Win32, "r", 1);
    bothFail.panicResult = OutputStatus::failure(OutputErrorKind::Win32, "p", 2);
    s = platform::reset_and_silence(&bothFail);
    check(s.kind == OutputErrorKind::InvalidOperation && s.message == "MIDI reset and post-reset panic both failed.",
        "ResetAndSilence: both failing");
}

// ---------------------------------------------------------------- WinMM backend on an injected table

struct WinMmFake {
    std::vector<uint32_t> shorts;
    int prepares = 0, longs = 0, unprepares = 0, resets = 0;
    uint32_t shortResult = 0;          // returned for every short message
    int failShortAt = -1;              // index of the short message that fails (-1 none)
    uint32_t prepareResult = 0, longResult = 0;
    bool completeImmediately = false;  // driver sets MHDR_DONE as soon as the long message is accepted
    std::vector<MIDIHDR*> headers;
    std::vector<uint8_t> lastLong;

    win32::WinMmMidiOutput::Api api()
    {
        win32::WinMmMidiOutput::Api table;
        table.short_msg = [this](void*, uint32_t message) -> uint32_t {
            int index = static_cast<int>(shorts.size());
            shorts.push_back(message);
            return index == failShortAt ? shortResult : 0;
        };
        table.prepare_header = [this](void*, void*, uint32_t) -> uint32_t { ++prepares; return prepareResult; };
        table.long_msg = [this](void*, void* header, uint32_t) -> uint32_t {
            ++longs;
            MIDIHDR* h = static_cast<MIDIHDR*>(header);
            lastLong.assign(reinterpret_cast<uint8_t*>(h->lpData), reinterpret_cast<uint8_t*>(h->lpData) + h->dwBufferLength);
            if (longResult == 0) {
                headers.push_back(h);
                if (completeImmediately) h->dwFlags |= MHDR_DONE;
            }
            return longResult;
        };
        table.unprepare_header = [this](void*, void*, uint32_t) -> uint32_t { ++unprepares; return 0; };
        table.reset = [this](void*) -> uint32_t { ++resets; return 0; };
        table.error_text = [](uint32_t code) -> std::wstring { return L"native text " + std::to_wstring(code); };
        return table;
    }
};

void test_winmm_backend()
{
    // A WinMM output that was never opened sends nothing and calls no native function.
    {
        win32::WinMmMidiOutput closed;
        OutputStatus s = closed.send_short(1);
        check(!closed.is_open() && !s.ok() && s.kind == OutputErrorKind::InvalidOperation &&
            s.message == "No MIDI output device is open.", "WinMM: not open -> v1 text");
        check(closed.send_long(sysex(0xF0, bytes_of({ 0xF0, 0xF7 }))).message == "No MIDI output device is open.",
            "WinMM: long message while not open -> v1 text");
        check(closed.panic().ok() && closed.reset().ok() && closed.close().ok(), "WinMM: panic/reset/close while not open are no-ops");
        check(closed.sysex_supported() && closed.sysex_disabled_reason().empty() && std::string(closed.backend_name()) == "WinMM",
            "WinMM: capability flags before open");
    }

    check(win32::WinMmMidiOutput::is_prepared_long_message_unsafe_description(L"WinMM to KDMAPI or syndrv") &&
        win32::WinMmMidiOutput::is_prepared_long_message_unsafe_description(L"winmm to kdmapi OR SYNDRV") &&
        !win32::WinMmMidiOutput::is_prepared_long_message_unsafe_description(L"Windows Multimedia API") &&
        !win32::WinMmMidiOutput::is_prepared_long_message_unsafe_description(L""),
        "WinMM: unsafe provider description (case-insensitive)");
    check(win32::WinMmMidiOutput::is_cumulative_byte_count_contract({ 0, 3, 6, 9 }) &&
        win32::WinMmMidiOutput::is_cumulative_byte_count_contract({ 0, 3, 6, 9, 12 }) &&
        !win32::WinMmMidiOutput::is_cumulative_byte_count_contract({ 0, 3, 6 }) &&
        !win32::WinMmMidiOutput::is_cumulative_byte_count_contract({ 0, 0, 0, 0 }) &&
        !win32::WinMmMidiOutput::is_cumulative_byte_count_contract({ 1, 3, 6, 9 }), "WinMM: cumulative byte-count contract");

    WinMmFake fake;
    {
        win32::WinMmMidiOutput out(fake.api());
        check(out.is_open() && out.output_identity().find("Deterministic WinMM boundary (device 0, handle 0x1, module ") == 0,
            "WinMM boundary: open on the fake handle, v1 identity text");

        check(out.send_short(0x007F3C90).ok() && fake.shorts.size() == 1 && fake.shorts[0] == 0x007F3C90u,
            "WinMM: short message passed through packed");

        // Panic order and shape.
        fake.shorts.clear();
        check(out.panic().ok() && fake.shorts.size() == 48, "WinMM: panic sends 48 messages");
        bool order = fake.shorts.size() == 48;
        for (size_t i = 0; order && i < 48; ++i) order = fake.shorts[i] == platform::panic_message(static_cast<int>(i)).packed;
        check(order, "WinMM: panic order CC120, CC123, CC64=0 per channel");

        // Errors: v1 wording and stop at the first failing panic message.
        fake.shorts.clear();
        fake.failShortAt = 1;
        fake.shortResult = 68;
        OutputStatus s = out.panic();
        check(fake.shorts.size() == 2 && !s.ok() && s.kind == OutputErrorKind::Win32 && s.native_code == 68, "WinMM: panic stops at the first failure");
        check_text(s.message, "MIDI error while sending CC123 panic through " + out.output_identity() +
            " (native code 68): native text 68", "WinMM: panic failure text");
        fake.shorts.clear();
        fake.failShortAt = 0;
        fake.shortResult = 5;
        s = out.send_short(0x90);
        check_text(s.message, "MIDI error while sending a short MIDI message through " + out.output_identity() +
            " (native code 5): native text 5", "WinMM: short message failure text");
        fake.failShortAt = -1;

        // SysEx: assembled, prepared, sent as one long message; reclaimed once the driver marks it done.
        fake.shorts.clear();
        check(out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x7E }))).ok() && fake.longs == 0, "WinMM: unfinished SysEx not sent");
        check(out.send_long(sysex(0xF7, bytes_of({ 0x09, 0xF7 }))).ok() && fake.prepares == 1 && fake.longs == 1 &&
            fake.lastLong == bytes_of({ 0xF0, 0x7E, 0x09, 0xF7 }) && out.outstanding_long_messages() == 1,
            "WinMM: complete SysEx prepared and sent once");
        check(fake.headers.size() == 1 && fake.unprepares == 0, "WinMM: in-flight buffer not yet reclaimed");
        fake.headers[0]->dwFlags |= MHDR_DONE;
        out.send_short(0x90);
        check(fake.unprepares == 1 && out.outstanding_long_messages() == 0, "WinMM: completed buffer unprepared before the next send");

        // Reset reclaims everything and drops unfinished SysEx; the native reset is called.
        out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
        check(out.outstanding_long_messages() == 1, "WinMM: second long message outstanding");
        int unpreparedBefore = fake.unprepares;
        out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01 })));
        check(out.reset().ok() && fake.resets == 1 && fake.unprepares == unpreparedBefore + 1 &&
            out.outstanding_long_messages() == 0, "WinMM: reset calls the native reset and unprepares everything");
        out.send_long(sysex(0xF7, bytes_of({ 0x02, 0xF7 })));
        check(fake.longs == 2, "WinMM: reset dropped the unfinished SysEx");

        // Failure paths of the long message: v1 diagnostics text, prepared header unprepared only after a send failure.
        WinMmFake bad;
        bad.prepareResult = 3;
        win32::WinMmMidiOutput prepFail(bad.api());
        prepFail.set_source_file("D:\\music\\song.mid");
        s = prepFail.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 }), 12, 480, 250000));
        check(!s.ok() && s.kind == OutputErrorKind::Win32 && s.native_code == 3 && bad.longs == 0 && bad.unprepares == 0 &&
            prepFail.outstanding_long_messages() == 0, "WinMM: prepare failure sends nothing and leaks nothing");
        check(s.message.find("MIDI System Exclusive failure during midiOutPrepareHeader (native code 3): native text 3\r\n"
            "Source: song.mid; event #12; tick 480; source time 250000 \xC2\xB5s\r\n"
            "Fragments: F0[#12,tick=480,bytes=3]\r\n") == 0, "WinMM: prepare failure diagnostics text");
        WinMmFake bad2;
        bad2.longResult = 68;
        win32::WinMmMidiOutput sendFail(bad2.api());
        s = sendFail.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
        check(!s.ok() && s.native_code == 68 && bad2.unprepares == 1 && sendFail.outstanding_long_messages() == 0 &&
            s.message.find("failure during midiOutLongMsg (native code 68)") != std::string::npos &&
            s.message.find("Source: unknown;") != std::string::npos, "WinMM: midiOutLongMsg failure unprepares and reports");
    }
    check(fake.resets >= 1, "WinMM: boundary instance destruction is quiet");
}

// ---------------------------------------------------------------- KDMAPI backend on a fake provider

class FakeKdmApi : public win32::KdmApiNative {
public:
    bool available = true, initialize = true, terminate = true, longs = true;
    uint32_t prepareResult = 0, sendResult = 0;
    std::string versionText = "1.2.3.4", pathText = "C:\\fake\\OmniMIDI.dll";
    std::string status = "prepared long-message exports available";
    std::vector<uint32_t> shorts;
    int initCalls = 0, terminateCalls = 0, resets = 0, prepares = 0, sends = 0, unprepares = 0;
    std::vector<MIDIHDR*> headers;
    std::vector<uint8_t> lastLong;

    bool is_available() override { return available; }
    bool initialize_stream() override { ++initCalls; return initialize; }
    bool terminate_stream() override { ++terminateCalls; return terminate; }
    void reset_stream() override { ++resets; }
    void send_short(uint32_t message) override { shorts.push_back(message); }
    uint32_t prepare_long(void*, uint32_t) override { ++prepares; return prepareResult; }
    uint32_t send_long(void* header, uint32_t) override
    {
        ++sends;
        MIDIHDR* h = static_cast<MIDIHDR*>(header);
        lastLong.assign(reinterpret_cast<uint8_t*>(h->lpData), reinterpret_cast<uint8_t*>(h->lpData) + h->dwBufferLength);
        if (sendResult == 0) headers.push_back(h);
        return sendResult;
    }
    uint32_t unprepare_long(void*, uint32_t) override { ++unprepares; return 0; }
    std::string version() const override { return versionText; }
    std::string provider_path() const override { return pathText; }
    bool supports_long_messages() const override { return longs; }
    std::string long_message_status() const override { return status; }
};

void test_kdmapi_backend()
{
    {
        win32::KdmApiMidiOutput never;   // never opened: no provider is resolved or loaded
        OutputStatus s = never.send_short(1);
        check(!never.is_open() && s.kind == OutputErrorKind::InvalidOperation && s.message == "The KDMAPI output is not open.",
            "KDMAPI: not open -> v1 text");
        check(never.panic().ok() && never.reset().ok() && never.close().ok() && never.provider_path().empty() &&
            never.sysex_supported() && never.enumerate().empty() && std::string(never.backend_name()) == "KDMAPI",
            "KDMAPI: closed no-ops and optimistic SysEx flag");
    }

    FakeKdmApi fake;
    win32::KdmApiMidiOutput out(&fake);
    check(out.open(0).ok() && out.is_open() && fake.initCalls == 1, "KDMAPI: open initialises the stream");
    out.send_short(0x007F3C90);
    check(fake.shorts.size() == 1 && fake.shorts[0] == 0x007F3C90u, "KDMAPI: short message passed through");
    {   // try_send_short (scan P11 item 1): true and no status on success, send_short's status on failure
        OutputStatus failure = OutputStatus::failure(OutputErrorKind::Win32, "left alone", 5);
        check(out.try_send_short(0x007F3C91, failure) && fake.shorts.size() == 2 && fake.shorts[1] == 0x007F3C91u &&
            failure.message == "left alone", "KDMAPI: try_send_short sends and leaves the status alone");
        win32::KdmApiMidiOutput never;
        check(!never.try_send_short(1, failure) && failure.kind == OutputErrorKind::InvalidOperation &&
            failure.message == "The KDMAPI output is not open." && failure.message == never.send_short(1).message,
            "KDMAPI: try_send_short on a closed output stores send_short's status");
    }
    fake.shorts.clear();
    check(out.panic().ok(), "KDMAPI: panic ok");
    bool order = fake.shorts.size() == 48;
    for (size_t i = 0; order && i < 48; ++i) order = fake.shorts[i] == platform::panic_message(static_cast<int>(i)).packed;
    check(order, "KDMAPI: panic order CC120, CC123, CC64=0 per channel");

    check(out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x43 }))).ok() && fake.sends == 0, "KDMAPI: unfinished SysEx not sent");
    check(out.send_long(sysex(0xF7, bytes_of({ 0x12, 0xF7 }))).ok() && fake.prepares == 1 && fake.sends == 1 &&
        fake.lastLong == bytes_of({ 0xF0, 0x43, 0x12, 0xF7 }) && out.outstanding_long_messages() == 1,
        "KDMAPI: complete SysEx prepared and sent once");
    fake.headers[0]->dwFlags |= MHDR_DONE;
    out.send_short(0x90);
    check(fake.unprepares == 1 && out.outstanding_long_messages() == 0, "KDMAPI: completed buffer reclaimed before the next send");

    out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
    check(out.reset().ok() && fake.resets == 1 && fake.unprepares == 2 && out.outstanding_long_messages() == 0,
        "KDMAPI: reset resets the stream and unprepares outstanding buffers");

    // Failure paths.
    fake.prepareResult = 3;
    OutputStatus s = out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
    check(!s.ok() && s.native_code == 3 && s.message.find("MIDI System Exclusive failure during PrepareLongData (native code 3)\r\n") == 0 &&
        fake.unprepares == 2 && out.outstanding_long_messages() == 0, "KDMAPI: prepare failure text, nothing leaked");
    fake.prepareResult = 0;
    fake.sendResult = 7;
    s = out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
    check(!s.ok() && s.native_code == 7 && s.message.find("failure during SendDirectLongData (native code 7)") != std::string::npos &&
        fake.unprepares == 3 && out.outstanding_long_messages() == 0, "KDMAPI: send failure unprepares and reports");
    fake.sendResult = 0;

    fake.longs = false;
    fake.status = "prepared long-message contract incomplete; missing UnprepareLongData";
    s = out.send_long(sysex(0xF0, bytes_of({ 0xF0, 0x01, 0xF7 })));
    const std::string expected = "The selected KDMAPI provider can send short MIDI messages but does not export "
        "PrepareLongData, SendDirectLongData, and UnprepareLongData. System Exclusive playback requires that prepared "
        "long-message contract (prepared long-message contract incomplete; missing UnprepareLongData). "
        "Provider: C:\\fake\\OmniMIDI.dll";
    check(s.kind == OutputErrorKind::NotSupported && s.message == expected && !out.sysex_supported() &&
        out.sysex_disabled_reason() == expected, "KDMAPI: long-message exports missing -> v1 NotSupported text and capability flag");
    fake.longs = true;

    // Close: reset + terminate; a borrowed provider survives; a rejected termination is reported but the output is closed.
    check(out.close().ok() && !out.is_open() && fake.terminateCalls == 1 && fake.resets == 2, "KDMAPI: close resets and terminates");
    fake.terminate = false;
    out.open(0);
    s = out.close();
    check(!s.ok() && s.kind == OutputErrorKind::InvalidOperation && !out.is_open() &&
        s.message == "The KDMAPI provider rejected stream termination: C:\\fake\\OmniMIDI.dll", "KDMAPI: rejected termination text");
    fake.terminate = true;

    // Open failures.
    FakeKdmApi unavailable;
    unavailable.available = false;
    win32::KdmApiMidiOutput o1(&unavailable);
    s = o1.open(0);
    check(!s.ok() && !o1.is_open() && s.message == "The KDMAPI provider reported that direct output is unavailable: C:\\fake\\OmniMIDI.dll",
        "KDMAPI: unavailable -> v1 text");
    FakeKdmApi rejects;
    rejects.initialize = false;
    win32::KdmApiMidiOutput o2(&rejects);
    s = o2.open(0);
    check(!s.ok() && s.message == "The KDMAPI provider rejected stream initialization (KDMAPI 1.2.3.4): C:\\fake\\OmniMIDI.dll",
        "KDMAPI: init rejected -> v1 text");

    // Destruction closes quietly (reset + terminate on the open one).
    FakeKdmApi tail;
    {
        win32::KdmApiMidiOutput o3(&tail);
        o3.open(0);
    }
    check(tail.resets == 1 && tail.terminateCalls == 1, "KDMAPI: destructor resets and terminates an open stream");
}

// ---------------------------------------------------------------- provider search and PE check (files only)

std::wstring make_temp_dir()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"midibn-midi-tests-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = data.empty() ? TRUE : WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

// A minimal "PE": MZ header, e_lfanew at 0x3C, "PE\0\0", Machine.
std::vector<uint8_t> fake_pe(uint16_t machine, int peOffset = 0x80, size_t total = 0x100)
{
    std::vector<uint8_t> data(total, 0);
    data[0] = 'M'; data[1] = 'Z';
    std::memcpy(&data[0x3C], &peOffset, 4);
    if (peOffset >= 0 && static_cast<size_t>(peOffset) + 6 <= total) {
        data[static_cast<size_t>(peOffset)] = 'P'; data[static_cast<size_t>(peOffset) + 1] = 'E';
        std::memcpy(&data[static_cast<size_t>(peOffset) + 4], &machine, 2);
    }
    return data;
}

void test_provider_files()
{
    using win32::DynamicKdmApiNative;
    std::wstring root = make_temp_dir();
    std::wstring app = root + L"\\app", sys = root + L"\\sys", none = root + L"\\none";
    CreateDirectoryW(app.c_str(), nullptr);
    CreateDirectoryW(sys.c_str(), nullptr);
    CreateDirectoryW(none.c_str(), nullptr);
    std::vector<uint8_t> dummy = bytes_of({ 1, 2, 3 });
    write_file(sys + L"\\OmniMIDI.dll", dummy);

    std::wstring resolved;
    OutputStatus s = DynamicKdmApiNative::resolve_provider_path(L"", app, sys, resolved);
    check(s.ok() && resolved == sys + L"\\OmniMIDI.dll", "provider: falls back to the system directory");
    write_file(app + L"\\OmniMIDI.dll", dummy);
    s = DynamicKdmApiNative::resolve_provider_path(L"", app + L"\\", sys, resolved);
    check(s.ok() && resolved == app + L"\\OmniMIDI.dll", "provider: application directory wins over the system directory");
    s = DynamicKdmApiNative::resolve_provider_path(L"  ", app, sys, resolved);
    check(s.ok() && resolved == app + L"\\OmniMIDI.dll", "provider: a blank explicit path is ignored");
    s = DynamicKdmApiNative::resolve_provider_path(L"", none, none, resolved);
    check_text(s.message, "No KDMAPI provider was found. Place a matching OmniMIDI.dll beside the application, install OmniMIDI, or turn off KDMAPI.",
        "provider: not found text");
    check(s.kind == OutputErrorKind::FileNotFound, "provider: not found kind");
    s = DynamicKdmApiNative::resolve_provider_path(root + L"\\missing.dll", app, sys, resolved);
    check(s.kind == OutputErrorKind::FileNotFound && s.message == "The selected KDMAPI provider was not found.",
        "provider: explicit path missing");
    s = DynamicKdmApiNative::resolve_provider_path(sys + L"\\OmniMIDI.dll", app, sys, resolved);
    check(s.ok() && resolved == sys + L"\\OmniMIDI.dll", "provider: an explicit path is used as is");

    // Registered-driver search (Windows XP: System32\OmniMIDI\OmniMIDI.dll). Fake folders and dummy files; nothing is loaded.
    {
        std::wstring sysWow = root + L"\\syswow", sys32 = root + L"\\sys32", reg = root + L"\\regapp";
        CreateDirectoryW(sysWow.c_str(), nullptr);
        CreateDirectoryW((sysWow + L"\\OmniMIDI").c_str(), nullptr);
        CreateDirectoryW(reg.c_str(), nullptr);
        win32::SystemFolders folders;
        folders.directory = sysWow;      // what a 32-bit process on 64-bit Windows sees
        folders.alternate = sys32;       // the other spelling of the same folder
        std::vector<win32::DriverEntry> entries;
        entries.push_back(win32::classify_driver_entry(L"midi", L"wdmaud.drv", folders));
        entries.push_back(win32::classify_driver_entry(L"midimapper", L"midimap.dll", folders));
        entries.push_back(win32::classify_driver_entry(L"midi1", L"OmniMIDI\\OmniMapper.dll", folders));
        entries.push_back(win32::classify_driver_entry(L"midi2", sys32 + L"\\OmniMIDI\\OmniMIDI.dll", folders));   // registry text names System32
        entries.push_back(win32::classify_driver_entry(L"midi3", L"other.dll", folders));
        entries.push_back(win32::classify_driver_entry(L"midi4", L"C:\\elsewhere\\x.dll", folders));
        std::vector<std::wstring> found = win32::kdmapi_registered_driver_folders(entries, sysWow);
        check(found.size() == 1 && found[0] == sysWow + L"\\OmniMIDI", "registered folders: OmniMapper and System32-spelled paths give one SysWOW64 folder, others are left out");
        std::vector<std::wstring> some;
        some.push_back(sysWow + L"\\OmniMIDI");
        write_file(sysWow + L"\\OmniMIDI\\OmniMIDI.dll", dummy);
        write_file(sysWow + L"\\OmniMIDI.dll", dummy);
        std::wstring appNone = root + L"\\appnone";
        CreateDirectoryW(appNone.c_str(), nullptr);
        s = DynamicKdmApiNative::resolve_provider_path(L"", appNone, some, sysWow, resolved);
        check(s.ok() && resolved == sysWow + L"\\OmniMIDI\\OmniMIDI.dll", "provider: the registered driver folder wins over the system directory");
        write_file(reg + L"\\OmniMIDI.dll", dummy);
        s = DynamicKdmApiNative::resolve_provider_path(L"", reg, some, sysWow, resolved);
        check(s.ok() && resolved == reg + L"\\OmniMIDI.dll", "provider: the application directory still wins over the registered folder");
        DeleteFileW((sysWow + L"\\OmniMIDI\\OmniMIDI.dll").c_str());
        s = DynamicKdmApiNative::resolve_provider_path(L"", appNone, some, sysWow, resolved);
        check(s.ok() && resolved == sysWow + L"\\OmniMIDI.dll", "provider: the system directory is the last fallback");
        s = DynamicKdmApiNative::resolve_provider_path(L"", appNone, std::vector<std::wstring>(), none, resolved);
        check(s.kind == OutputErrorKind::FileNotFound, "provider: nothing anywhere is still not found");
        std::vector<win32::DriverEntry> same;
        same.push_back(win32::classify_driver_entry(L"midi", L"OmniMapper.dll", folders));
        check(win32::kdmapi_registered_driver_folders(same, sysWow).empty(), "registered folders: a driver in the system directory itself adds nothing (searched last)");
    }

    // PE machine reading and the architecture check (files are read, never loaded).
    const uint16_t thisMachine = sizeof(void*) == 8 ? 0x8664 : 0x014C;
    const uint16_t otherMachine = sizeof(void*) == 8 ? 0x014C : 0x8664;
    std::wstring pe = root + L"\\pe.dll";
    write_file(pe, fake_pe(0x8664));
    uint16_t machine = 0;
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).ok() && machine == 0x8664, "PE: reads x64 machine");
    write_file(pe, fake_pe(0x014C));
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).ok() && machine == 0x014C, "PE: reads x86 machine");
    write_file(pe, fake_pe(thisMachine));
    check(DynamicKdmApiNative::validate_provider_architecture(pe).ok(), "PE: matching architecture accepted");
    write_file(pe, fake_pe(otherMachine));
    s = DynamicKdmApiNative::validate_provider_architecture(pe);
    std::string expectedText = std::string("The KDMAPI provider architecture does not match this ") + (sizeof(void*) == 8 ? "x64" : "x86") +
        " application: " + win32::wide_to_utf8(pe) + " (PE machine 0x" + (otherMachine == 0x8664 ? "8664" : "014C") + ").";
    check(s.kind == OutputErrorKind::BadImageFormat && s.message == expectedText, "PE: wrong architecture text");

    write_file(pe, std::vector<uint8_t>(63, 'M'));
    s = DynamicKdmApiNative::read_pe_machine(pe, machine);
    check(s.kind == OutputErrorKind::BadImageFormat && s.message == "The KDMAPI provider is not a valid Windows PE image: " + win32::wide_to_utf8(pe),
        "PE: shorter than 64 bytes");
    std::vector<uint8_t> noMagic = fake_pe(0x8664);
    noMagic[0] = 'X';
    write_file(pe, noMagic);
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).message.find("is not a valid Windows PE image") != std::string::npos, "PE: bad MZ magic");
    write_file(pe, fake_pe(0x8664, 0x1000, 0x100));
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).message.find("has an invalid PE header") != std::string::npos, "PE: offset beyond the file");
    write_file(pe, fake_pe(0x8664, -1));
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).message.find("has an invalid PE header") != std::string::npos, "PE: negative offset");
    std::vector<uint8_t> noSignature = fake_pe(0x8664);
    noSignature[0x80] = 'Q';
    write_file(pe, noSignature);
    check(DynamicKdmApiNative::read_pe_machine(pe, machine).message.find("has an invalid PE signature") != std::string::npos, "PE: bad signature");
    // The architecture check opens the file read-only with read sharing (as v1 did); when that fails
    // the text names the file and says what to do (DD-003; v1 showed .NET's own FileStream text).
    s = DynamicKdmApiNative::read_pe_machine(root + L"\\absent.dll", machine);
    check(s.kind == OutputErrorKind::FileNotFound && s.native_code == ERROR_FILE_NOT_FOUND, "PE: missing file kind");
    check_text(s.message, "The KDMAPI provider file was not found: " + win32::wide_to_utf8(root) + "\\absent.dll. Check that OmniMIDI is installed and that the file has not been moved or deleted.", "PE: missing file text");
    s = DynamicKdmApiNative::read_pe_machine(root + L"\\nodir\\x.dll", machine);
    check(s.kind == OutputErrorKind::DirectoryNotFound, "PE: missing directory kind");
    check_text(s.message, "The folder of the KDMAPI provider file does not exist: " + win32::wide_to_utf8(root) + "\\nodir\\x.dll. Check the folder name or choose a different provider file.", "PE: missing directory text");
    s = DynamicKdmApiNative::read_pe_machine(root, machine);   // a directory, not a file
    check(s.kind == OutputErrorKind::UnauthorizedAccess, "PE: directory kind");
    check_text(s.message, "Windows denied access to the KDMAPI provider file: " + win32::wide_to_utf8(root) + ". Check the file's permissions or choose a copy in a folder you can read.", "PE: directory text");
    // The path is named in full even when a relative one is given (v1 passes GetFullPath's result).
    s = DynamicKdmApiNative::read_pe_machine(L"midibn-relative-absent.dll", machine);
    check_text(s.message, "The KDMAPI provider file was not found: " + win32::wide_to_utf8(win32::full_path(L"midibn-relative-absent.dll")) + ". Check that OmniMIDI is installed and that the file has not been moved or deleted.",
        "PE: relative path is shown in full");
    {
        std::wstring held = root + L"\\held.dll";
        write_file(held, fake_pe(thisMachine));
        HANDLE holder = CreateFileW(held.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(holder != INVALID_HANDLE_VALUE, "PE: could hold the file share-none");
        s = DynamicKdmApiNative::read_pe_machine(held, machine);
        check(s.kind == OutputErrorKind::IoError && s.native_code == ERROR_SHARING_VIOLATION, "PE: sharing violation kind");
        check_text(s.message, "The KDMAPI provider file is being used by another program and cannot be read right now: " + win32::wide_to_utf8(held) + ". Close the other program and try again.",
            "PE: sharing violation text");
        s = DynamicKdmApiNative::validate_provider_architecture(held);   // the same text reaches the architecture check
        check(s.kind == OutputErrorKind::IoError, "PE: sharing violation reaches validate_provider_architecture");
        if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);
        DeleteFileW(held.c_str());
    }

    DeleteFileW(pe.c_str());
    DeleteFileW((app + L"\\OmniMIDI.dll").c_str());
    DeleteFileW((sys + L"\\OmniMIDI.dll").c_str());
    RemoveDirectoryW(app.c_str());
    RemoveDirectoryW(sys.c_str());
    RemoveDirectoryW(none.c_str());
    RemoveDirectoryW(root.c_str());
}

// The wording of the failures the backends report when Windows refuses a file or memory request
// (register entry DD-003: v1 showed raw .NET exception texts). No file is touched here.
void test_error_texts()
{
    using win32::provider_file_open_error;
    const std::string p = "C:\\probe\\file.dll";
    struct Row { DWORD code; OutputErrorKind kind; const char* text; };
    const Row rows[] = {
        { 2, OutputErrorKind::FileNotFound, "The KDMAPI provider file was not found: C:\\probe\\file.dll. Check that OmniMIDI is installed and that the file has not been moved or deleted." },
        { 3, OutputErrorKind::DirectoryNotFound, "The folder of the KDMAPI provider file does not exist: C:\\probe\\file.dll. Check the folder name or choose a different provider file." },
        { 5, OutputErrorKind::UnauthorizedAccess, "Windows denied access to the KDMAPI provider file: C:\\probe\\file.dll. Check the file's permissions or choose a copy in a folder you can read." },
        { 15, OutputErrorKind::DriveNotFound, "The drive that holds the KDMAPI provider file is not available: C:\\probe\\file.dll. Check that the drive is connected and ready." },
        { 32, OutputErrorKind::IoError, "The KDMAPI provider file is being used by another program and cannot be read right now: C:\\probe\\file.dll. Close the other program and try again." },
        { 206, OutputErrorKind::PathTooLong, "The path of the KDMAPI provider file is too long for Windows: C:\\probe\\file.dll. Move the file to a folder with a shorter path." },
        { 995, OutputErrorKind::OperationCanceled, "Opening the KDMAPI provider file was canceled by Windows: C:\\probe\\file.dll. Try again." }
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        OutputStatus s = provider_file_open_error(rows[i].code, p);
        check(s.kind == rows[i].kind && s.native_code == static_cast<int32_t>(rows[i].code), "error text: kind and native code");
        check_text(s.message, rows[i].text, ("error text: open failure for Win32 error " + std::to_string(rows[i].code)).c_str());
    }
    check_text(provider_file_open_error(2, "D:\\a b\\x.dll").message,
        "The KDMAPI provider file was not found: D:\\a b\\x.dll. Check that OmniMIDI is installed and that the file has not been moved or deleted.",
        "error text: the path is inserted verbatim");

    // Anything else: Windows' own description without its trailing line break, the code in the text.
    check_text(win32::system_error_description(999999), "Windows error 999999", "error text: no Windows description");
    const std::string known[] = { win32::system_error_description(53), win32::system_error_description(87),
        win32::system_error_description(123) };
    for (size_t i = 0; i < 3; ++i)
        check(!known[i].empty() && known[i].back() != '\n' && known[i].back() != '\r' && known[i].back() != ' ',
            "error text: the Windows description has no trailing line break");
    OutputStatus other = provider_file_open_error(87, p);
    check(other.kind == OutputErrorKind::IoError && other.native_code == 87, "error text: other codes are I/O failures");
    check_text(other.message, "The KDMAPI provider file could not be opened: " + p + ". Windows reports: " + known[1] + " (error 87).",
        "error text: other code names the file and the Windows description");
    check_text(provider_file_open_error(999999, p).message,
        "The KDMAPI provider file could not be opened: " + p + ". Windows reports: Windows error 999999 (error 999999).",
        "error text: unknown code");
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ENGLISH) {   // the recorded texts are English
        check_text(known[0], "The network path was not found.", "error text: Windows description 53 (English)");
        check_text(known[1], "The parameter is incorrect.", "error text: Windows description 87 (English)");
        check_text(known[2], "The filename, directory name, or volume label syntax is incorrect.", "error text: Windows description 123 (English)");
    }

    check_text(win32::provider_file_truncated_message(p),
        "The KDMAPI provider file ended before its header could be read: C:\\probe\\file.dll. The file may be damaged or incomplete; reinstall OmniMIDI or choose a different provider file.",
        "error text: truncated provider file");
    check_text(win32::out_of_memory_for_sysex_message(),
        "There is not enough memory to send a System Exclusive (SysEx) message to the MIDI device. Close other programs and try again.",
        "error text: out of memory for a SysEx buffer");
    // DD-010: readiness probes that never gave two accepted results in a row fail the open even when the last one
    // was accepted (v1 reported that last result, 0, which is success).
    check(win32::WinMmMidiOutput::probe_failure_code(0, 1050) == 1050, "probe: last accepted but never two in a row: the earlier error is reported");
    check(win32::WinMmMidiOutput::probe_failure_code(7, 1050) == 7, "probe: the last error is reported when the last probe failed");
    check(win32::WinMmMidiOutput::probe_failure_code(7, 0) == 7, "probe: single error");
    // The allocator reports failure (null) rather than throwing, for a size that cannot exist.
    check(win32::allocate_long_buffer(nullptr, static_cast<size_t>(-1) / 2) == nullptr, "alloc: impossible SysEx buffer is refused");
}

} // namespace

int main()
{
    test_packing_and_panic();
    test_assembler();
    test_sysex_diagnostics();
    test_null_and_recording();
    test_winmm_backend();
    test_kdmapi_backend();
    test_provider_files();
    test_error_texts();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
