// ALSA MIDI output tests (Milestone 8, stage 4). The output talks to the sequencer only through the AlsaSeq seam, so
// most of this runs against a fake that records the exact snd_seq_event_t values, the call pattern and the order. When
// /dev/snd/seq exists, a client-to-client loopback test also sends through the real library to a test input port in this
// same process; otherwise that part is reported as skipped. No hardware port is opened, no sound is made.
#include "alsa_midi_output.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

using namespace midibn;
using namespace midibn::posix;

// ---------------------------------------------------------------- the fake sequencer

struct Call {
    enum Kind { Open, CreatePort, List, Connect, Direct, Buffered, Drain, Drop, Close } kind;
    snd_seq_event_t event;          // Direct / Buffered
    std::vector<uint8_t> sysex;     // the data of a variable-length event
    int a = 0, b = 0, c = 0;        // Connect: port, destination client, destination port
    std::string text;               // Open: client name; CreatePort: port name
};

struct FakeSeq : AlsaSeq {
    std::vector<Call> calls;
    std::vector<AlsaPortInfo> ports;
    bool opened = false;
    int ownClient = 128;
    int openResult = 0, createResult = 0, connectResult = 0, directResult = 0, bufferedResult = 0, drainResult = 0, listResult = 0;

    int open(const char* name) override
    {
        Call call; call.kind = Call::Open; call.text = name; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call);
        if (openResult < 0) return openResult;
        opened = true;
        return 0;
    }
    bool is_open() const override { return opened; }
    int client_id() const override { return ownClient; }
    int create_output_port(const char* name) override
    {
        Call call; call.kind = Call::CreatePort; call.text = name; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call);
        return createResult < 0 ? createResult : 0;
    }
    int list_ports(std::vector<AlsaPortInfo>& out) override
    {
        Call call; call.kind = Call::List; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call);
        if (listResult < 0) return listResult;
        out.insert(out.end(), ports.begin(), ports.end());
        return 0;
    }
    int connect_to(int port, int client, int dest) override
    {
        Call call; call.kind = Call::Connect; call.a = port; call.b = client; call.c = dest; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call);
        return connectResult;
    }
    void record(Call::Kind kind, snd_seq_event_t& event)
    {
        Call call; call.kind = kind; call.event = event;
        if ((event.flags & SND_SEQ_EVENT_LENGTH_MASK) == SND_SEQ_EVENT_LENGTH_VARIABLE) {
            const uint8_t* data = static_cast<const uint8_t*>(event.data.ext.ptr);
            call.sysex.assign(data, data + event.data.ext.len);
        }
        calls.push_back(call);
    }
    int output_direct(snd_seq_event_t& event) override
    {
        record(Call::Direct, event);
        return directResult < 0 ? directResult : 28;
    }
    int output_buffered(snd_seq_event_t& event) override { record(Call::Buffered, event); return bufferedResult < 0 ? bufferedResult : 28; }
    int drain() override { Call call; call.kind = Call::Drain; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call); return drainResult; }
    int drop_output() override { Call call; call.kind = Call::Drop; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call); return 0; }
    void close() override { Call call; call.kind = Call::Close; std::memset(&call.event, 0, sizeof(call.event)); calls.push_back(call); opened = false; }

    std::vector<Call> sent() const   // the event-carrying calls only
    {
        std::vector<Call> result;
        for (size_t i = 0; i < calls.size(); ++i) if (calls[i].kind == Call::Direct || calls[i].kind == Call::Buffered) result.push_back(calls[i]);
        return result;
    }
    int count(Call::Kind kind) const
    {
        int n = 0;
        for (size_t i = 0; i < calls.size(); ++i) if (calls[i].kind == kind) ++n;
        return n;
    }
};

AlsaPortInfo port(int client, int number, const char* clientName, const char* portName, unsigned caps)
{
    AlsaPortInfo info;
    info.client = client; info.port = number; info.client_name = clientName; info.port_name = portName; info.capability = caps;
    return info;
}

const unsigned kWritable = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;

std::vector<AlsaPortInfo> sample_ports()
{
    std::vector<AlsaPortInfo> ports;
    ports.push_back(port(0, 0, "System", "Timer", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE));
    ports.push_back(port(0, 1, "System", "Announce", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE));
    ports.push_back(port(14, 0, "Midi Through", "Midi Through Port-0", kWritable | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ));
    ports.push_back(port(20, 0, "FLUID Synth", "FLUID Synth", kWritable));
    ports.push_back(port(20, 1, "FLUID Synth", "Second", kWritable));
    ports.push_back(port(24, 0, "Keyboard", "Keyboard MIDI 1", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ));   // input only
    ports.push_back(port(30, 0, "Hidden", "Hidden", kWritable | SND_SEQ_PORT_CAP_NO_EXPORT));
    ports.push_back(port(31, 0, "Closed", "Closed", SND_SEQ_PORT_CAP_WRITE));                                       // not subscribable
    ports.push_back(port(128, 0, "MIDIBottleneck Player", "Output", kWritable));                                    // our own client
    return ports;
}

// Opens an output on a fake that already knows `sample_ports`, connected to 20:0.
struct Rig {
    FakeSeq* seq;
    AlsaMidiOutput output;
    explicit Rig(FakeSeq* fake) : seq(fake), output(std::unique_ptr<AlsaSeq>(fake)) { fake->ports = sample_ports(); }
};

void expect_event(const Call& call, snd_seq_event_type_t type, int channel, const std::string& what)
{
    check(call.event.type == type, what + ": type");
    check(call.event.source.port == 0 && call.event.dest.client == SND_SEQ_ADDRESS_SUBSCRIBERS &&
        call.event.queue == SND_SEQ_QUEUE_DIRECT, what + ": direct to subscribers, no queue");
    if (channel >= 0) check(call.event.data.note.channel == channel, what + ": channel");
}

// ---------------------------------------------------------------- the tests

void test_enumeration()
{
    Rig rig(new FakeSeq());
    std::vector<platform::OutputDeviceInfo> devices = rig.output.enumerate();
    check(rig.output.last_enumeration_error().empty(), "enumeration works");
    check(devices.size() == 3, "three destinations are listed (not system, own client, input-only, hidden or closed ports)");
    if (devices.size() == 3) {
        check(devices[0].device_id == 14u * 256 + 0 && devices[0].name == "Midi Through: Midi Through Port-0 (14:0)", "Midi Through is listed with client and port names");
        check(devices[1].device_id == 20u * 256 + 0 && devices[1].name == "FLUID Synth (20:0)", "a port named like its client is not repeated");
        check(devices[2].device_id == 20u * 256 + 1 && devices[2].name == "FLUID Synth: Second (20:1)", "the second port of a client is listed");
    }
    check(AlsaMidiOutput::device_client(20u * 256 + 1) == 20 && AlsaMidiOutput::device_port(20u * 256 + 1) == 1, "device id splits into client and port");
    check(rig.seq->count(Call::Open) == 1 && rig.seq->count(Call::Close) == 1, "enumerating before open opens and closes a temporary client");

    FakeSeq* none = new FakeSeq(); none->openResult = -ENOENT;
    AlsaMidiOutput missing{ std::unique_ptr<AlsaSeq>(none) };
    check(missing.enumerate().empty() && missing.last_enumeration_error() == "The ALSA sequencer is not available on this system (no /dev/snd/seq).", "no /dev/snd/seq: empty list with the plain sentence");
    FakeSeq* broken = new FakeSeq(); broken->listResult = -EIO;
    AlsaMidiOutput unreadable{ std::unique_ptr<AlsaSeq>(broken) };
    check(unreadable.enumerate().empty() && unreadable.last_enumeration_error().find("could not be read") != std::string::npos, "a failing listing is reported");
}

void test_texts()
{
    check(AlsaMidiOutput::unavailable_text(ENOENT) == "The ALSA sequencer is not available on this system (no /dev/snd/seq).", "text: ENOENT");
    check(AlsaMidiOutput::unavailable_text(ENODEV) == AlsaMidiOutput::unavailable_text(ENOENT), "text: ENODEV same as ENOENT");
    check(AlsaMidiOutput::unavailable_text(EACCES).find("not allowed to use the ALSA sequencer") != std::string::npos &&
        AlsaMidiOutput::unavailable_text(EACCES).find("audio group") != std::string::npos, "text: EACCES names the audio group");
    check(AlsaMidiOutput::unavailable_text(EBUSY).find("busy") != std::string::npos, "text: EBUSY");
    check(AlsaMidiOutput::unavailable_text(ENOMEM).find("not enough memory") != std::string::npos, "text: ENOMEM");
    check(AlsaMidiOutput::unavailable_text(EIO).find("could not be opened (") != std::string::npos, "text: other errors carry the system text");
}

void test_open_close()
{
    {
        FakeSeq* fake = new FakeSeq();
        Rig rig(fake);
        platform::OutputStatus status = rig.output.open(AlsaMidiOutput::make_device_id(20, 0));
        check(status.ok() && rig.output.is_open(), "open succeeds");
        check(fake->calls[0].kind == Call::Open && fake->calls[0].text == "MIDIBottleneck Player", "the client is named MIDIBottleneck Player");
        check(fake->count(Call::CreatePort) == 1, "exactly one output port is created");
        int connects = 0;
        for (size_t i = 0; i < fake->calls.size(); ++i)
            if (fake->calls[i].kind == Call::Connect) { ++connects; check(fake->calls[i].b == 20 && fake->calls[i].c == 0, "connected to 20:0"); }
        check(connects == 1, "exactly one connection");
        check(rig.output.path_description() == "ALSA sequencer, sending directly to FLUID Synth (20:0).", "status text names the destination");
        check(std::string(rig.output.backend_name()) == "ALSA" && rig.output.sysex_supported() && rig.output.sysex_disabled_reason().empty(), "capability flags");
        check(rig.output.close().ok() && !rig.output.is_open() && !fake->opened, "close closes the client (and with it the connection)");
        check(rig.output.close().ok(), "closing twice is fine");
    }
    {
        FakeSeq* fake = new FakeSeq(); fake->openResult = -ENOENT;
        AlsaMidiOutput output{ std::unique_ptr<AlsaSeq>(fake) };
        platform::OutputStatus status = output.open(AlsaMidiOutput::make_device_id(20, 0));
        check(!status.ok() && status.message == "The ALSA sequencer is not available on this system (no /dev/snd/seq).", "open without a sequencer: plain sentence");
        check(!output.is_open(), "... and the output stays closed");
    }
    {
        FakeSeq* fake = new FakeSeq(); fake->openResult = -EACCES;
        AlsaMidiOutput output{ std::unique_ptr<AlsaSeq>(fake) };
        check(output.open(1).message.find("not allowed") != std::string::npos, "open without permission: audio group hint");
    }
    {
        FakeSeq* fake = new FakeSeq();
        Rig rig(fake);
        platform::OutputStatus status = rig.output.open(AlsaMidiOutput::make_device_id(99, 0));
        check(!status.ok() && status.message == "The MIDI port 99:0 is not available in the ALSA sequencer. It may have been unplugged or its program closed. Choose the output again.", "open of a vanished port");
        check(fake->count(Call::CreatePort) == 0 && !fake->opened, "... creates no port and closes the client");
    }
    {
        FakeSeq* fake = new FakeSeq(); fake->createResult = -ENOMEM;
        Rig rig(fake);
        platform::OutputStatus status = rig.output.open(AlsaMidiOutput::make_device_id(20, 0));
        check(!status.ok() && status.message.find("could not create its MIDI output port") != std::string::npos && !fake->opened, "port creation failure is reported and cleaned up");
    }
    {
        FakeSeq* fake = new FakeSeq(); fake->connectResult = -EPERM;
        Rig rig(fake);
        platform::OutputStatus status = rig.output.open(AlsaMidiOutput::make_device_id(20, 0));
        check(!status.ok() && status.message.find("could not connect to the MIDI port FLUID Synth (20:0)") != std::string::npos && !fake->opened && !rig.output.is_open(), "connect failure is reported and cleaned up");
    }
    {
        Rig rig(new FakeSeq());
        check(rig.output.send_short(0x7F3C90).message == "No MIDI output device is open.", "send before open: same text as WinMM");
        output::SystemExclusiveEvent sysex; uint8_t data[2] = { 0xF0, 0xF7 }; sysex.status = 0xF0; sysex.data = data; sysex.length = 2;
        check(rig.output.send_long(sysex).message == "No MIDI output device is open.", "send_long before open: same text");
        check(rig.output.reset().ok() && rig.output.panic().ok(), "reset and panic on a closed output do nothing");
    }
}

uint32_t pack(unsigned status, unsigned d1, unsigned d2) { return status | (d1 << 8) | (d2 << 16); }

void test_short_messages()
{
    FakeSeq* fake = new FakeSeq();
    Rig rig(fake);
    rig.output.set_send_pattern(AlsaSendPattern::Direct);
    check(rig.output.open(AlsaMidiOutput::make_device_id(20, 0)).ok(), "open for message tests");
    fake->calls.clear();
    struct Case { uint32_t packed; };
    std::vector<uint32_t> messages = {
        pack(0x93, 60, 100), pack(0x83, 60, 64), pack(0x90, 61, 0), pack(0xA5, 62, 33), pack(0xBF, 7, 99), pack(0xC2, 41, 0),
        pack(0xD9, 77, 0), pack(0xE0, 0, 64), pack(0xE1, 0, 0), pack(0xE2, 127, 127), pack(0xE3, 1, 65),
        pack(0xF1, 0x35, 0), pack(0xF2, 0x01, 0x02), pack(0xF3, 9, 0), pack(0xF6, 0, 0), pack(0xF8, 0, 0), pack(0xFA, 0, 0),
        pack(0xFB, 0, 0), pack(0xFC, 0, 0), pack(0xFE, 0, 0), pack(0xFF, 0, 0) };
    for (size_t i = 0; i < messages.size(); ++i) check(rig.output.send_short(messages[i]).ok(), "send " + std::to_string(i));
    std::vector<Call> sent = fake->sent();
    check(sent.size() == messages.size(), "one direct event per message, in the same order");
    check(fake->count(Call::Buffered) == 0 && fake->count(Call::Drain) == 0, "the direct pattern never buffers or drains");
    if (sent.size() == messages.size()) {
        size_t i = 0;
        expect_event(sent[i], SND_SEQ_EVENT_NOTEON, 3, "note on");
        check(sent[i].event.data.note.note == 60 && sent[i].event.data.note.velocity == 100, "note on: note and velocity"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_NOTEOFF, 3, "note off");
        check(sent[i].event.data.note.note == 60 && sent[i].event.data.note.velocity == 64 && sent[i].event.data.note.off_velocity == 0, "note off: note and release velocity (in the velocity field)"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_NOTEON, 0, "note on velocity 0");
        check(sent[i].event.data.note.note == 61 && sent[i].event.data.note.velocity == 0, "note on with velocity 0 stays a note on with velocity 0 (as the bytes say)"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_KEYPRESS, 5, "poly pressure");
        check(sent[i].event.data.note.note == 62 && sent[i].event.data.note.velocity == 33, "poly pressure: note and value"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_CONTROLLER, 15, "control change");
        check(sent[i].event.data.control.param == 7 && sent[i].event.data.control.value == 99, "control change: controller and value"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_PGMCHANGE, 2, "program change");
        check(sent[i].event.data.control.value == 41, "program change: program"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_CHANPRESS, 9, "channel pressure");
        check(sent[i].event.data.control.value == 77, "channel pressure: value"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_PITCHBEND, 0, "pitch bend centre");
        check(sent[i].event.data.control.value == 0, "pitch bend 0x2000 is 0"); ++i;
        check(sent[i].event.data.control.value == -8192, "pitch bend minimum is -8192"); ++i;
        check(sent[i].event.data.control.value == 8191, "pitch bend maximum is 8191"); ++i;
        check(sent[i].event.data.control.value == 1 + (65 << 7) - 8192, "pitch bend combines the low byte first"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_QFRAME, -1, "quarter frame");
        check(sent[i].event.data.control.value == 0x35, "quarter frame value"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_SONGPOS, -1, "song position");
        check(sent[i].event.data.control.value == (1 | (2 << 7)), "song position combines the two bytes"); ++i;
        expect_event(sent[i], SND_SEQ_EVENT_SONGSEL, -1, "song select");
        check(sent[i].event.data.control.value == 9, "song select value"); ++i;
        expect_event(sent[i++], SND_SEQ_EVENT_TUNE_REQUEST, -1, "tune request");
        expect_event(sent[i++], SND_SEQ_EVENT_CLOCK, -1, "clock");
        expect_event(sent[i++], SND_SEQ_EVENT_START, -1, "start");
        expect_event(sent[i++], SND_SEQ_EVENT_CONTINUE, -1, "continue");
        expect_event(sent[i++], SND_SEQ_EVENT_STOP, -1, "stop");
        expect_event(sent[i++], SND_SEQ_EVENT_SENSING, -1, "active sensing");
        expect_event(sent[i++], SND_SEQ_EVENT_RESET, -1, "system reset");
    }
    // Data bytes are masked to 7 bits.
    fake->calls.clear();
    rig.output.send_short(pack(0x90, 0xBC, 0xFF));
    std::vector<Call> masked = fake->sent();
    check(masked.size() == 1 && masked[0].event.data.note.note == 0x3C && masked[0].event.data.note.velocity == 0x7F, "data bytes are masked to 7 bits");
    // Messages with no sequencer event.
    fake->calls.clear();
    check(!rig.output.send_short(pack(0x45, 1, 2)).ok(), "a data byte as the status is refused");
    check(rig.output.send_short(pack(0xF4, 0, 0)).message == "This MIDI message cannot be sent through the ALSA sequencer.", "an undefined system byte is refused with a plain sentence");
    check(!rig.output.send_short(pack(0xF0, 0, 0)).ok() && !rig.output.send_short(pack(0xF7, 0, 0)).ok(), "F0 and F7 do not go through send_short");
    check(fake->sent().empty(), "refused messages reach nothing");
}

void test_patterns()
{
    // Direct: outside and inside a batch alike, one direct call per message.
    {
        FakeSeq* fake = new FakeSeq(); Rig rig(fake);
        rig.output.set_send_pattern(AlsaSendPattern::Direct);
        rig.output.open(AlsaMidiOutput::make_device_id(20, 0)); fake->calls.clear();
        { platform::SendBatch batch(&rig.output); rig.output.send_short(pack(0x90, 1, 1)); rig.output.send_short(pack(0x90, 2, 1)); }
        check(fake->count(Call::Direct) == 2 && fake->count(Call::Buffered) == 0 && fake->count(Call::Drain) == 0, "Direct: two direct calls even inside a batch");
    }
    // BufferedEach: buffer + drain per message.
    {
        FakeSeq* fake = new FakeSeq(); Rig rig(fake);
        rig.output.set_send_pattern(AlsaSendPattern::BufferedEach);
        rig.output.open(AlsaMidiOutput::make_device_id(20, 0)); fake->calls.clear();
        rig.output.send_short(pack(0x90, 1, 1)); rig.output.send_short(pack(0x90, 2, 1));
        check(fake->calls.size() == 4 && fake->calls[0].kind == Call::Buffered && fake->calls[1].kind == Call::Drain &&
            fake->calls[2].kind == Call::Buffered && fake->calls[3].kind == Call::Drain, "BufferedEach: buffer, drain, buffer, drain");
    }
    // Batched (the default): direct outside a batch, buffered inside with one drain at the end, order kept.
    {
        FakeSeq* fake = new FakeSeq(); Rig rig(fake);
        check(rig.output.send_pattern() == AlsaSendPattern::Batched, "Batched is the default");
        rig.output.open(AlsaMidiOutput::make_device_id(20, 0)); fake->calls.clear();
        rig.output.send_short(pack(0x90, 1, 1));
        {
            platform::SendBatch batch(&rig.output);
            for (int n = 10; n < 15; ++n) rig.output.send_short(pack(0x90, n, 1));
        }
        rig.output.send_short(pack(0x80, 1, 1));
        check(fake->calls.size() == 1 + 5 + 1 + 1 && fake->calls[0].kind == Call::Direct && fake->calls[6].kind == Call::Drain && fake->calls[7].kind == Call::Direct,
            "Batched: direct, five buffered, one drain at the end of the batch, direct");
        bool ordered = true;
        for (int n = 0; n < 5; ++n) ordered = ordered && fake->calls[1 + n].kind == Call::Buffered && fake->calls[1 + n].event.data.note.note == 10 + n;
        check(ordered, "Batched: the buffered messages are in the order they were sent");
        // A long message in a batch drains the earlier messages first.
        fake->calls.clear();
        uint8_t body[] = { 0xF0, 0x7D, 0x01, 0xF7 };
        output::SystemExclusiveEvent sysex; sysex.status = 0xF0; sysex.data = body; sysex.length = 4;
        { platform::SendBatch batch(&rig.output); rig.output.send_short(pack(0x90, 3, 1)); rig.output.send_long(sysex); rig.output.send_short(pack(0x80, 3, 1)); }
        check(fake->calls.size() == 5 && fake->calls[0].kind == Call::Buffered && fake->calls[1].kind == Call::Drain && fake->calls[2].kind == Call::Direct &&
            fake->calls[3].kind == Call::Buffered && fake->calls[4].kind == Call::Drain, "Batched: a SysEx inside a batch is sent after the earlier messages, before the later ones");
    }
    // Errors.
    {
        FakeSeq* fake = new FakeSeq(); Rig rig(fake);
        rig.output.open(AlsaMidiOutput::make_device_id(20, 0));
        fake->directResult = -EPIPE;
        platform::OutputStatus status = rig.output.send_short(pack(0x90, 1, 1));
        check(!status.ok() && status.native_code == -EPIPE && status.message == "A MIDI message could not be sent through the ALSA sequencer. The MIDI port is no longer available. It may have been unplugged or its program closed. Choose the output again.", "a vanished destination is explained plainly");
        fake->directResult = -EIO;
        status = rig.output.send_short(pack(0x90, 1, 1));
        check(!status.ok() && status.message.find("could not be sent through the ALSA sequencer. (") != std::string::npos, "other send errors carry the system text");
        // A failing drain at the end of a batch is reported by the next call.
        fake->directResult = 0; fake->drainResult = -EIO;
        { platform::SendBatch batch(&rig.output); rig.output.send_short(pack(0x90, 1, 1)); }
        fake->drainResult = 0;
        check(!rig.output.send_short(pack(0x90, 1, 1)).ok(), "a failed drain at the end of a batch is reported by the next send");
        check(rig.output.send_short(pack(0x90, 1, 1)).ok(), "... once");
    }
}

std::vector<uint8_t> joined(const std::vector<Call>& calls)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < calls.size(); ++i) bytes.insert(bytes.end(), calls[i].sysex.begin(), calls[i].sysex.end());
    return bytes;
}

void test_sysex()
{
    FakeSeq* fake = new FakeSeq(); Rig rig(fake);
    rig.output.open(AlsaMidiOutput::make_device_id(20, 0)); fake->calls.clear();
    // A complete short message in one event.
    uint8_t whole[] = { 0xF0, 0x7D, 0x10, 0x20, 0xF7 };
    output::SystemExclusiveEvent event; event.status = 0xF0; event.data = whole; event.length = sizeof(whole);
    check(rig.output.send_long(event).ok(), "complete SysEx accepted");
    std::vector<Call> sent = fake->sent();
    check(sent.size() == 1 && sent[0].kind == Call::Direct && sent[0].event.type == SND_SEQ_EVENT_SYSEX &&
        (sent[0].event.flags & SND_SEQ_EVENT_LENGTH_MASK) == SND_SEQ_EVENT_LENGTH_VARIABLE &&
        sent[0].sysex == std::vector<uint8_t>(whole, whole + sizeof(whole)), "one variable-length sysex event with exactly F0 7D 10 20 F7");
    check(sent[0].event.dest.client == SND_SEQ_ADDRESS_SUBSCRIBERS && sent[0].event.queue == SND_SEQ_QUEUE_DIRECT, "sysex is direct to subscribers");
    // An F0 event without its end is held back; the F7 event completes it.
    fake->calls.clear();
    uint8_t first[] = { 0xF0, 0x7D, 0x01, 0x02 }; uint8_t rest[] = { 0x03, 0xF7 };
    event.data = first; event.length = sizeof(first);
    check(rig.output.send_long(event).ok() && fake->sent().empty(), "an unfinished SysEx sends nothing yet");
    output::SystemExclusiveEvent tail; tail.status = 0xF7; tail.data = rest; tail.length = sizeof(rest);
    check(rig.output.send_long(tail).ok(), "the continuation is accepted");
    sent = fake->sent();
    check(sent.size() == 1 && sent[0].sysex == std::vector<uint8_t>({ 0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7 }), "the assembled message is sent whole, framed F0..F7");
    // An empty fragment is ignored.
    fake->calls.clear();
    output::SystemExclusiveEvent empty; empty.status = 0xF0;
    check(rig.output.send_long(empty).ok() && fake->sent().empty(), "an empty fragment is ignored");
    // A long message is chunked, in order, and the bytes are intact.
    std::vector<uint8_t> big(1300);
    big[0] = 0xF0; for (size_t i = 1; i + 1 < big.size(); ++i) big[i] = static_cast<uint8_t>(i & 0x7F); big.back() = 0xF7;
    event.data = big.data(); event.length = big.size();
    fake->calls.clear();
    check(rig.output.send_long(event).ok(), "a 1300-byte SysEx is accepted");
    sent = fake->sent();
    check(sent.size() == 3 && sent[0].sysex.size() == 512 && sent[1].sysex.size() == 512 && sent[2].sysex.size() == 276, "it goes out in chunks of 512, 512 and 276 bytes");
    check(joined(sent) == big, "the chunks joined are exactly the message");
    bool allDirect = true;
    for (size_t i = 0; i < sent.size(); ++i) allDirect = allDirect && sent[i].kind == Call::Direct;
    check(allDirect, "every chunk is delivered directly before the next is written");
    // Failure.
    fake->directResult = -EIO; fake->calls.clear();
    event.data = whole; event.length = sizeof(whole);
    platform::OutputStatus status = rig.output.send_long(event);
    check(!status.ok() && status.message.find("A System Exclusive message could not be sent") == 0, "SysEx failure has its own sentence");
}

void test_reset_and_panic()
{
    FakeSeq* fake = new FakeSeq(); Rig rig(fake);
    rig.output.open(AlsaMidiOutput::make_device_id(20, 0)); fake->calls.clear();
    // reset drops buffered output and an unfinished SysEx, and sends nothing.
    uint8_t first[] = { 0xF0, 0x7D, 0x01 }; uint8_t rest[] = { 0x02, 0xF7 };
    output::SystemExclusiveEvent event; event.status = 0xF0; event.data = first; event.length = sizeof(first);
    rig.output.send_long(event);
    check(rig.output.reset().ok(), "reset succeeds");
    check(fake->count(Call::Drop) == 1 && fake->sent().empty(), "reset drops unsent output and sends no messages");
    output::SystemExclusiveEvent tail; tail.status = 0xF7; tail.data = rest; tail.length = sizeof(rest);
    rig.output.send_long(tail);
    check(fake->sent().empty(), "the SysEx unfinished before the reset is gone (its continuation sends nothing)");
    // panic: CC120, CC123, CC64=0 on each channel, in the shared order, one drain at the end.
    fake->calls.clear();
    check(rig.output.panic().ok(), "panic succeeds");
    std::vector<Call> sent = fake->sent();
    check(sent.size() == 48 && fake->calls.back().kind == Call::Drain && fake->count(Call::Drain) == 1, "panic buffers 48 messages and drains once");
    bool same = sent.size() == 48;
    for (int i = 0; same && i < 48; ++i) {
        platform::PanicMessage expected = platform::panic_message(i);
        same = sent[i].kind == Call::Buffered && sent[i].event.type == SND_SEQ_EVENT_CONTROLLER &&
            sent[i].event.data.control.channel == (expected.packed & 15u) && sent[i].event.data.control.param == ((expected.packed >> 8) & 0x7Fu) &&
            sent[i].event.data.control.value == static_cast<int>((expected.packed >> 16) & 0x7Fu);
    }
    check(same, "the 48 panic messages match the shared panic order exactly (per channel 120, 123, 64=0)");
    check(sent.size() == 48 && sent[0].event.data.control.param == 120 && sent[1].event.data.control.param == 123 && sent[2].event.data.control.param == 64 &&
        sent[2].event.data.control.value == 0 && sent[3].event.data.control.channel == 1, "spot check: channel 0 first, 120 / 123 / 64");
    // reset_and_silence: reset (drop) before panic.
    fake->calls.clear();
    check(platform::reset_and_silence(&rig.output).ok(), "reset_and_silence succeeds");
    check(fake->calls.front().kind == Call::Drop && fake->sent().size() == 48, "reset_and_silence resets first, then panics");
    // Panic failure.
    fake->calls.clear(); fake->bufferedResult = -EPIPE;
    platform::OutputStatus status = rig.output.panic();
    check(!status.ok() && status.message.find("CC120 panic message could not be sent") != std::string::npos && fake->count(Call::Drop) == 1, "a failing panic names the message and drops what was buffered");
}

// ---- Receive side of the loopback test. What the listener reads is recorded as plain values so the judging logic can be
// tested without a sequencer (see test_loopback_judging). On a real machine other programs (session managers, MIDI
// bridges) may also write to a freshly created port, and the system announce port may deliver notices; those are listed
// in the log but do not count against the player. Only events that come from the player's own client are judged.
struct Received {
    int type = 0;
    int sourceClient = -1, sourcePort = -1, destClient = -1, destPort = -1;
    int channel = -1;      // -1 when the event has none
    int value = 0;         // note / controller number / bend value
    unsigned length = 0;   // SysEx length
};

std::string describe(const Received& r)
{
    char text[200];
    std::snprintf(text, sizeof(text), "type %d from %d:%d to %d:%d channel %d value %d length %u",
        r.type, r.sourceClient, r.sourcePort, r.destClient, r.destPort, r.channel, r.value, r.length);
    return text;
}

// True when the events from `playerClient` are exactly note on, controller, pitch bend, SysEx, in that order.
// `extras` counts events from anyone else (reported, never a failure).
bool judge_loopback(const std::vector<Received>& all, int playerClient, int& extras)
{
    std::vector<Received> mine;
    extras = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].sourceClient == playerClient) mine.push_back(all[i]); else ++extras;
    }
    return mine.size() == 4 && mine[0].type == SND_SEQ_EVENT_NOTEON && mine[0].channel == 3 && mine[0].value == 60 &&
        mine[1].type == SND_SEQ_EVENT_CONTROLLER && mine[1].channel == 3 && mine[1].value == 7 &&
        mine[2].type == SND_SEQ_EVENT_PITCHBEND && mine[2].channel == 3 && mine[2].value == 0 &&
        mine[3].type == SND_SEQ_EVENT_SYSEX && mine[3].length == 4;
}

Received make_received(int type, int srcClient, int srcPort, int channel, int value, unsigned length = 0)
{
    Received r; r.type = type; r.sourceClient = srcClient; r.sourcePort = srcPort; r.destClient = 130; r.destPort = 0;
    r.channel = channel; r.value = value; r.length = length;
    return r;
}

// Feeds the judging logic the event sequences a real machine can produce, so a sequencer is not needed to check it.
void test_loopback_judging()
{
    const int player = 129;
    std::vector<Received> clean;
    clean.push_back(make_received(SND_SEQ_EVENT_NOTEON, player, 0, 3, 60));
    clean.push_back(make_received(SND_SEQ_EVENT_CONTROLLER, player, 0, 3, 7));
    clean.push_back(make_received(SND_SEQ_EVENT_PITCHBEND, player, 0, 3, 0));
    clean.push_back(make_received(SND_SEQ_EVENT_SYSEX, player, 0, -1, 0, 4));
    int extras = -1;
    check(judge_loopback(clean, player, extras) && extras == 0, "judging: the four events from the player pass");

    // A "port subscribed" notice from the system announce port (0:1) in front, and noise from another client between.
    std::vector<Received> noisy = clean;
    noisy.insert(noisy.begin(), make_received(SND_SEQ_EVENT_PORT_SUBSCRIBED, 0, 1, -1, 0));
    noisy.insert(noisy.begin() + 3, make_received(SND_SEQ_EVENT_CLOCK, 131, 0, -1, 0));
    check(judge_loopback(noisy, player, extras) && extras == 2, "judging: notices and other clients' events are listed but do not fail");

    std::vector<Received> missing(clean.begin(), clean.begin() + 3);
    check(!judge_loopback(missing, player, extras), "judging: a missing SysEx fails");
    std::vector<Received> swapped = clean; std::swap(swapped[0], swapped[1]);
    check(!judge_loopback(swapped, player, extras), "judging: a wrong order fails");
    std::vector<Received> wrongBend = clean; wrongBend[2].value = 64;
    check(!judge_loopback(wrongBend, player, extras), "judging: a wrong value fails");
    std::vector<Received> duplicate = clean; duplicate.push_back(clean[0]);
    check(!judge_loopback(duplicate, player, extras), "judging: an extra event from the player fails");
    std::vector<Received> onlyForeign; onlyForeign.push_back(make_received(SND_SEQ_EVENT_PORT_SUBSCRIBED, 0, 1, -1, 0));
    check(!judge_loopback(onlyForeign, player, extras) && extras == 1, "judging: nothing from the player fails");
}

Received from_library(const snd_seq_event_t& e)
{
    Received r;
    r.type = e.type; r.sourceClient = e.source.client; r.sourcePort = e.source.port;
    r.destClient = e.dest.client; r.destPort = e.dest.port;
    switch (e.type) {
    case SND_SEQ_EVENT_NOTEON: case SND_SEQ_EVENT_NOTEOFF: case SND_SEQ_EVENT_KEYPRESS:
        r.channel = e.data.note.channel; r.value = e.data.note.note; break;
    case SND_SEQ_EVENT_CONTROLLER: case SND_SEQ_EVENT_PITCHBEND: case SND_SEQ_EVENT_PGMCHANGE: case SND_SEQ_EVENT_CHANPRESS:
        r.channel = e.data.control.channel;
        r.value = (e.type == SND_SEQ_EVENT_CONTROLLER) ? static_cast<int>(e.data.control.param) : e.data.control.value; break;
    case SND_SEQ_EVENT_SYSEX: r.length = e.data.ext.len; break;
    default: break;
    }
    return r;
}

// Client-to-client loopback through the real library: our output port to a test input port in this process.
void test_loopback()
{
    struct stat info;
    if (::stat("/dev/snd/seq", &info) != 0) {
        std::printf("SKIPPED loopback test: /dev/snd/seq does not exist on this machine (no ALSA sequencer)\n");
        return;
    }
    snd_seq_t* listener = nullptr;
    if (snd_seq_open(&listener, "hw", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        std::printf("SKIPPED loopback test: /dev/snd/seq exists but cannot be opened by this user\n");
        return;
    }
    snd_seq_set_client_name(listener, "MIDIBottleneck loopback test");
    int listenPort = snd_seq_create_simple_port(listener, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    int listenClient = snd_seq_client_id(listener);
    check(listenPort >= 0, "loopback: test input port created");
    AlsaMidiOutput output;
    std::vector<platform::OutputDeviceInfo> devices = output.enumerate();
    bool listed = false;
    for (size_t i = 0; i < devices.size(); ++i) listed = listed || devices[i].device_id == AlsaMidiOutput::make_device_id(listenClient, listenPort);
    check(listed, "loopback: the test input port is listed as a destination");
    platform::OutputStatus status = output.open(AlsaMidiOutput::make_device_id(listenClient, listenPort));
    check(status.ok(), "loopback: open succeeds " + status.message);
    if (status.ok()) {
        // Who is subscribed to the listener's port? That is the player's client.
        int playerClient = -1;
        snd_seq_query_subscribe_t* query = nullptr;
        snd_seq_query_subscribe_alloca(&query);
        snd_seq_addr_t listenAddr; listenAddr.client = static_cast<unsigned char>(listenClient); listenAddr.port = static_cast<unsigned char>(listenPort);
        snd_seq_query_subscribe_set_root(query, &listenAddr);
        snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
        snd_seq_query_subscribe_set_index(query, 0);
        if (snd_seq_query_port_subscribers(listener, query) >= 0) playerClient = snd_seq_query_subscribe_get_addr(query)->client;
        std::printf("loopback: listener is %d:%d, player client (from the subscription) is %d\n", listenClient, listenPort, playerClient);

        platform::OutputStatus a = output.send_short(pack(0x93, 60, 100));
        platform::OutputStatus b = output.send_short(pack(0xB3, 7, 90));
        platform::OutputStatus c = output.send_short(pack(0xE3, 0, 64));
        uint8_t body[] = { 0xF0, 0x7D, 0x05, 0xF7 };
        output::SystemExclusiveEvent sysex; sysex.status = 0xF0; sysex.data = body; sysex.length = sizeof(body);
        platform::OutputStatus d = output.send_long(sysex);
        check(a.ok() && b.ok() && c.ok() && d.ok(), "loopback: sends succeed");
        // Read for up to one second; once the four are in, keep listening a further 150 ms so stray extra events show up.
        std::vector<Received> got;
        int mineSoFar = 0, idleAfterFour = 0;
        for (int tries = 0; tries < 100 && idleAfterFour < 15; ++tries) {
            snd_seq_event_t* event = nullptr;
            if (snd_seq_event_input(listener, &event) >= 0 && event != nullptr) {
                got.push_back(from_library(*event));
                if (got.back().sourceClient == playerClient) ++mineSoFar;
                idleAfterFour = 0;
            } else {
                ::usleep(10000);
                if (mineSoFar >= 4) ++idleAfterFour;
            }
        }
        std::printf("loopback: %zu event(s) arrived:\n", got.size());
        for (size_t i = 0; i < got.size(); ++i) std::printf("  [%zu] %s\n", i, describe(got[i]).c_str());
        int extras = 0;
        bool ok = playerClient >= 0 && judge_loopback(got, playerClient, extras);
        if (extras > 0) std::printf("loopback: %d event(s) came from other clients or the system; they are not counted against the player\n", extras);
        check(ok, "loopback: the four events arrive in order with the right types");
        output.close();
    }
    snd_seq_close(listener);
}

} // namespace

int main()
{
    test_enumeration();
    test_texts();
    test_open_close();
    test_short_messages();
    test_patterns();
    test_sysex();
    test_reset_and_panic();
    test_loopback_judging();
    test_loopback();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
