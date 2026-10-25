// Linux MIDI output through the ALSA sequencer (Milestone 8, stage 4; ARCHITECTURE.md section 4). The same MidiOutput
// interface as WinMM and KDMAPI. The player creates one client "MIDIBottleneck Player" with one output port and
// connects it to the chosen destination port; every message is delivered DIRECTLY (no sequencer queue, no timestamps):
// the engine's own clock decides when a message is sent, as it does on Windows.
#pragma once

#include "alsa_seq.h"

#include "midibn/midi_output.h"

#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace posix {

// How send_short reaches the sequencer (the three call patterns the send-check tool measures).
enum class AlsaSendPattern {
    Direct,         // snd_seq_event_output_direct for every message: one write per message
    BufferedEach,   // snd_seq_event_output + snd_seq_drain_output for every message: one write per message
    Batched         // Direct outside a SendBatch; inside one: buffered, one drain at the end of the batch
};

class AlsaMidiOutput : public platform::MidiOutput {
public:
    static constexpr const char* kClientName = "MIDIBottleneck Player";
    static constexpr const char* kPortName = "Output";

    AlsaMidiOutput();                                      // the real sequencer
    explicit AlsaMidiOutput(std::unique_ptr<AlsaSeq> seq); // a substitute sequencer (tests)
    ~AlsaMidiOutput() override;
    AlsaMidiOutput(const AlsaMidiOutput&) = delete;
    AlsaMidiOutput& operator=(const AlsaMidiOutput&) = delete;

    // Writable, subscribable ports of other clients: not our own client, not client 0 (System: Timer and Announce),
    // not ports marked no-export. device_id = client * 256 + port.
    static std::vector<platform::OutputDeviceInfo> enumerate_devices();
    static uint32_t make_device_id(int client, int port) { return static_cast<uint32_t>(client) * 256u + static_cast<uint32_t>(port); }
    static int device_client(uint32_t device_id) { return static_cast<int>(device_id / 256u); }
    static int device_port(uint32_t device_id) { return static_cast<int>(device_id % 256u); }
    // The filter and the display name, separated from the library so a test can drive them.
    static std::vector<platform::OutputDeviceInfo> destinations_from(const std::vector<AlsaPortInfo>& ports, int own_client);
    // Plain sentence for a failed sequencer call: errno (positive) and what we were doing.
    static std::string unavailable_text(int error);
    // Why enumerate() last returned nothing because of an error (empty when the last listing worked).
    const std::string& last_enumeration_error() const { return lastEnumerationError_; }

    // Which call pattern send_short uses; the default is Batched. Set before open.
    void set_send_pattern(AlsaSendPattern pattern) { pattern_ = pattern; }
    AlsaSendPattern send_pattern() const { return pattern_; }

    const char* backend_name() const override { return "ALSA"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override;
    platform::OutputStatus open(uint32_t deviceId) override;
    platform::OutputStatus close() override;
    bool is_open() const override { return connected_; }
    platform::OutputStatus send_short(uint32_t packed) override;
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    platform::OutputStatus reset() override;
    platform::OutputStatus panic() override;
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { sourceFile_ = path_utf8; }
    std::string path_description() const override;
    void begin_send_batch() override;
    void end_send_batch() override;

    // Builds the sequencer event for a packed short message (status | data1 << 8 | data2 << 16). Returns false for a
    // byte that is not a status (running status) or a status with no sequencer event (F0, F4, F5, F7, F9, FD).
    static bool make_short_event(uint32_t packed, int source_port, snd_seq_event_t& event);

private:
    platform::OutputStatus send_event(snd_seq_event_t& event);
    platform::OutputStatus alsa_failure(const std::string& what, int error) const;

    mutable std::unique_ptr<AlsaSeq> seq_;
    mutable std::string lastEnumerationError_;
    AlsaSendPattern pattern_ = AlsaSendPattern::Batched;
    bool connected_ = false;
    int port_ = -1;
    int destClient_ = -1;
    int destPort_ = -1;
    std::string destName_;
    std::string sourceFile_;
    bool inBatch_ = false;
    int deferredError_ = 0;   // a drain at the end of a batch failed; reported by the next call
    output::SystemExclusiveAssembler assembler_;
};

} // namespace posix
} // namespace midibn
