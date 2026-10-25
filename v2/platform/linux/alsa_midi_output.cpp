#include "alsa_midi_output.h"

#include <cerrno>
#include <cstring>

namespace midibn {
namespace posix {
namespace {

using platform::OutputErrorKind;
using platform::OutputStatus;

const size_t kSysexChunkBytes = 512;   // one sequencer event of a long message; a chunk is delivered before the next is written

std::string system_text(int error) { return std::strerror(error); }

} // namespace

AlsaMidiOutput::AlsaMidiOutput() : seq_(make_real_alsa_seq()) {}
AlsaMidiOutput::AlsaMidiOutput(std::unique_ptr<AlsaSeq> seq) : seq_(std::move(seq)) {}
AlsaMidiOutput::~AlsaMidiOutput() { close(); }

// ---------------------------------------------------------------- wording

std::string AlsaMidiOutput::unavailable_text(int error)
{
    switch (error) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
        return "The ALSA sequencer is not available on this system (no /dev/snd/seq).";
    case EACCES:
    case EPERM:
        return "This user is not allowed to use the ALSA sequencer (/dev/snd/seq). "
               "Add the user to the audio group, sign in again and try again.";
    case EBUSY:
        return "The ALSA sequencer is busy and cannot be opened right now. Close other MIDI programs and try again.";
    case ENOMEM:
        return "There is not enough memory to open the ALSA sequencer. Close other programs and try again.";
    default:
        return "The ALSA sequencer could not be opened (" + system_text(error) + ").";
    }
}

OutputStatus AlsaMidiOutput::alsa_failure(const std::string& what, int error) const
{
    std::string text;
    if (error == ENOENT || error == ENXIO || error == ENODEV || error == EPIPE)
        text = what + " The MIDI port is no longer available. It may have been unplugged or its program closed. "
               "Choose the output again.";
    else
        text = what + " (" + system_text(error) + ")";
    return OutputStatus::failure(OutputErrorKind::IoError, text, -error);
}

// ---------------------------------------------------------------- devices

std::vector<platform::OutputDeviceInfo> AlsaMidiOutput::destinations_from(const std::vector<AlsaPortInfo>& ports, int own_client)
{
    std::vector<platform::OutputDeviceInfo> devices;
    const unsigned need = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    for (size_t i = 0; i < ports.size(); ++i) {
        const AlsaPortInfo& port = ports[i];
        if (port.client == own_client || port.client == SND_SEQ_CLIENT_SYSTEM) continue;   // client 0: Timer, Announce
        if ((port.capability & need) != need) continue;
        if (port.capability & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
        platform::OutputDeviceInfo info;
        info.device_id = make_device_id(port.client, port.port);
        std::string label = port.client_name;
        if (!port.port_name.empty() && port.port_name != port.client_name) label += ": " + port.port_name;
        info.name = label + " (" + std::to_string(port.client) + ":" + std::to_string(port.port) + ")";
        devices.push_back(info);
    }
    return devices;
}

std::vector<platform::OutputDeviceInfo> AlsaMidiOutput::enumerate_devices()
{
    AlsaMidiOutput output;
    return output.enumerate();
}

std::vector<platform::OutputDeviceInfo> AlsaMidiOutput::enumerate() const
{
    lastEnumerationError_.clear();
    std::vector<platform::OutputDeviceInfo> none;
    bool temporary = !seq_->is_open();
    if (temporary) {
        int result = seq_->open(kClientName);
        if (result < 0) {
            lastEnumerationError_ = unavailable_text(-result);
            return none;
        }
    }
    std::vector<AlsaPortInfo> ports;
    int result = seq_->list_ports(ports);
    int own = seq_->client_id();
    if (temporary) seq_->close();
    if (result < 0) {
        lastEnumerationError_ = "The list of MIDI ports could not be read from the ALSA sequencer (" + system_text(-result) + ").";
        return none;
    }
    return destinations_from(ports, own);
}

// ---------------------------------------------------------------- open / close

OutputStatus AlsaMidiOutput::open(uint32_t deviceId)
{
    close();
    int destClient = device_client(deviceId);
    int destPort = device_port(deviceId);
    std::string name = std::to_string(destClient) + ":" + std::to_string(destPort);
    int result = seq_->open(kClientName);
    if (result < 0)
        return OutputStatus::failure(OutputErrorKind::IoError, unavailable_text(-result), result);
    // Look the destination up by name, for the status line and to say plainly when it has gone.
    std::vector<AlsaPortInfo> ports;
    if (seq_->list_ports(ports) >= 0) {
        std::vector<platform::OutputDeviceInfo> destinations = destinations_from(ports, seq_->client_id());
        bool found = false;
        for (size_t i = 0; i < destinations.size(); ++i)
            if (destinations[i].device_id == deviceId) { name = destinations[i].name; found = true; break; }
        if (!found) {
            seq_->close();
            return OutputStatus::failure(OutputErrorKind::IoError,
                "The MIDI port " + name + " is not available in the ALSA sequencer. It may have been unplugged or its "
                "program closed. Choose the output again.", -ENOENT);
        }
    }
    int port = seq_->create_output_port(kPortName);
    if (port < 0) {
        seq_->close();
        return alsa_failure("The player could not create its MIDI output port in the ALSA sequencer.", -port);
    }
    result = seq_->connect_to(port, destClient, destPort);
    if (result < 0) {
        seq_->close();
        return alsa_failure("The player could not connect to the MIDI port " + name + ".", -result);
    }
    port_ = port;
    destClient_ = destClient;
    destPort_ = destPort;
    destName_ = name;
    connected_ = true;
    inBatch_ = false;
    deferredError_ = 0;
    assembler_.reset();
    return OutputStatus::success();
}

OutputStatus AlsaMidiOutput::close()
{
    assembler_.reset();
    if (!connected_ && !seq_->is_open()) return OutputStatus::success();
    if (connected_ && inBatch_) seq_->drain();
    inBatch_ = false;
    connected_ = false;
    seq_->close();   // closing the client removes the subscription
    port_ = -1;
    return OutputStatus::success();
}

std::string AlsaMidiOutput::path_description() const
{
    if (!connected_) return std::string();
    return "ALSA sequencer, sending directly to " + destName_ + ".";
}

// ---------------------------------------------------------------- short messages

bool AlsaMidiOutput::make_short_event(uint32_t packed, int source_port, snd_seq_event_t& event)
{
    const unsigned status = packed & 0xFFu;
    const unsigned d1 = (packed >> 8) & 0x7Fu;
    const unsigned d2 = (packed >> 16) & 0x7Fu;
    if (status < 0x80u) return false;
    std::memset(&event, 0, sizeof(event));
    snd_seq_ev_set_source(&event, source_port);
    snd_seq_ev_set_subs(&event);
    snd_seq_ev_set_direct(&event);
    if (status < 0xF0u) {
        const unsigned channel = status & 0x0Fu;
        switch (status & 0xF0u) {
        case 0x80: snd_seq_ev_set_noteoff(&event, channel, d1, d2); break;
        case 0x90: snd_seq_ev_set_noteon(&event, channel, d1, d2); break;   // velocity 0 stays a note-on of velocity 0
        case 0xA0: snd_seq_ev_set_keypress(&event, channel, d1, d2); break;
        case 0xB0: snd_seq_ev_set_controller(&event, channel, d1, d2); break;
        case 0xC0: snd_seq_ev_set_pgmchange(&event, channel, d1); break;
        case 0xD0: snd_seq_ev_set_chanpress(&event, channel, d1); break;
        default: // 0xE0
            snd_seq_ev_set_pitchbend(&event, channel, static_cast<int>(d1 | (d2 << 7)) - 8192);
            break;
        }
        return true;
    }
    switch (status) {
    case 0xF1: event.type = SND_SEQ_EVENT_QFRAME; event.data.control.value = static_cast<int>(d1); return true;
    case 0xF2: event.type = SND_SEQ_EVENT_SONGPOS; event.data.control.value = static_cast<int>(d1 | (d2 << 7)); return true;
    case 0xF3: event.type = SND_SEQ_EVENT_SONGSEL; event.data.control.value = static_cast<int>(d1); return true;
    case 0xF6: event.type = SND_SEQ_EVENT_TUNE_REQUEST; return true;
    case 0xF8: event.type = SND_SEQ_EVENT_CLOCK; return true;
    case 0xFA: event.type = SND_SEQ_EVENT_START; return true;
    case 0xFB: event.type = SND_SEQ_EVENT_CONTINUE; return true;
    case 0xFC: event.type = SND_SEQ_EVENT_STOP; return true;
    case 0xFE: event.type = SND_SEQ_EVENT_SENSING; return true;
    case 0xFF: event.type = SND_SEQ_EVENT_RESET; return true;
    default: return false;
    }
}

OutputStatus AlsaMidiOutput::send_event(snd_seq_event_t& event)
{
    int result;
    if (inBatch_) {
        result = seq_->output_buffered(event);
    } else if (pattern_ == AlsaSendPattern::BufferedEach) {
        result = seq_->output_buffered(event);
        if (result >= 0) result = seq_->drain();
    } else {
        result = seq_->output_direct(event);
    }
    if (result < 0) return alsa_failure("A MIDI message could not be sent through the ALSA sequencer.", -result);
    return OutputStatus::success();
}

OutputStatus AlsaMidiOutput::send_short(uint32_t packed)
{
    if (!connected_) return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    if (deferredError_ != 0) {
        int error = deferredError_;
        deferredError_ = 0;
        return alsa_failure("MIDI messages could not be sent through the ALSA sequencer.", error);
    }
    snd_seq_event_t event;
    if (!make_short_event(packed, port_, event))
        return OutputStatus::failure(OutputErrorKind::InvalidOperation,
            "This MIDI message cannot be sent through the ALSA sequencer.");
    return send_event(event);
}

void AlsaMidiOutput::begin_send_batch()
{
    if (connected_ && pattern_ == AlsaSendPattern::Batched) inBatch_ = true;
}

void AlsaMidiOutput::end_send_batch()
{
    if (!inBatch_) return;
    inBatch_ = false;
    int result = seq_->drain();
    if (result < 0) deferredError_ = -result;
}

// ---------------------------------------------------------------- long messages

OutputStatus AlsaMidiOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    if (!connected_) return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    if (fragment.length == 0) return OutputStatus::success();
    // As for WinMM: an F0 event starts a packet, an F7 event continues it, and only a complete F0...F7 message is sent.
    output::SystemExclusivePacket packet;
    if (!assembler_.accept(fragment, packet)) return OutputStatus::success();
    // A long message goes out in chunks; each is delivered before the next is written, so the order is exact.
    if (inBatch_) {   // keep the order of earlier buffered short messages
        int result = seq_->drain();
        if (result < 0) return alsa_failure("A MIDI message could not be sent through the ALSA sequencer.", -result);
    }
    size_t offset = 0;
    while (offset < packet.bytes.size()) {
        size_t count = packet.bytes.size() - offset;
        if (count > kSysexChunkBytes) count = kSysexChunkBytes;
        snd_seq_event_t event;
        std::memset(&event, 0, sizeof(event));
        snd_seq_ev_set_source(&event, port_);
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        snd_seq_ev_set_sysex(&event, static_cast<unsigned>(count), const_cast<uint8_t*>(packet.bytes.data() + offset));
        int result = seq_->output_direct(event);
        if (result < 0)
            return alsa_failure("A System Exclusive message could not be sent through the ALSA sequencer.", -result);
        offset += count;
    }
    return OutputStatus::success();
}

// ---------------------------------------------------------------- reset and panic

// As v1 Reset: discards any unfinished System Exclusive message and what is buffered but not yet sent. Like WinMM's
// reset it sends no controller messages of its own; reset_and_silence() follows it with panic().
OutputStatus AlsaMidiOutput::reset()
{
    assembler_.reset();
    if (!connected_) return OutputStatus::success();
    inBatch_ = false;
    deferredError_ = 0;
    seq_->drop_output();
    return OutputStatus::success();
}

// As v1 Panic: per channel CC120, CC123, CC64 = 0 (the shared panic_message order), written in one drain.
OutputStatus AlsaMidiOutput::panic()
{
    if (!connected_) return OutputStatus::success();
    inBatch_ = false;
    deferredError_ = 0;
    for (int i = 0; i < platform::kPanicMessageCount; ++i) {
        snd_seq_event_t event;
        make_short_event(platform::panic_message(i).packed, port_, event);
        int result = seq_->output_buffered(event);
        if (result < 0) {
            seq_->drop_output();
            return alsa_failure(std::string("The ") + platform::panic_message(i).name +
                " panic message could not be sent through the ALSA sequencer.", -result);
        }
    }
    int result = seq_->drain();
    if (result < 0) return alsa_failure("The panic messages could not be sent through the ALSA sequencer.", -result);
    return OutputStatus::success();
}

} // namespace posix
} // namespace midibn
