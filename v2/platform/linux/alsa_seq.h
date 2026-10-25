// The seam between the ALSA MIDI output and the ALSA sequencer library (Milestone 8, stage 4). The output class calls
// only the few functions below; the real implementation forwards them to libasound, and a unit test substitutes a fake
// that records the exact snd_seq_event_t values the output produced. Nothing here opens a device until open() is called.
#pragma once

#include <alsa/asoundlib.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace posix {

// One port of one sequencer client, with the fields the output filters on.
struct AlsaPortInfo {
    int client = 0;
    int port = 0;
    std::string client_name;
    std::string port_name;
    unsigned capability = 0;   // SND_SEQ_PORT_CAP_*
    unsigned type = 0;         // SND_SEQ_PORT_TYPE_*
};

// All functions return 0 or a positive value on success and a negative errno value on failure (ALSA's own convention).
class AlsaSeq {
public:
    virtual ~AlsaSeq() {}
    // Opens the sequencer for output and names our client.
    virtual int open(const char* client_name) = 0;
    virtual bool is_open() const = 0;
    virtual int client_id() const = 0;
    // Creates a readable, subscribable application port; returns its port number (>= 0).
    virtual int create_output_port(const char* port_name) = 0;
    // Every port of every client, in the order the sequencer lists them (client 0 included).
    virtual int list_ports(std::vector<AlsaPortInfo>& ports) = 0;
    // Subscribes our output port to the destination.
    virtual int connect_to(int port, int dest_client, int dest_port) = 0;
    // snd_seq_event_output_direct: delivered at once, no output buffer, no queue.
    virtual int output_direct(snd_seq_event_t& event) = 0;
    // snd_seq_event_output: appended to the output buffer (drained by drain() or when the buffer is full).
    virtual int output_buffered(snd_seq_event_t& event) = 0;
    virtual int drain() = 0;          // snd_seq_drain_output
    virtual int drop_output() = 0;    // snd_seq_drop_output: discards what is buffered and not yet sent
    virtual void close() = 0;
};

// The real implementation over libasound ("hw", blocking output).
std::unique_ptr<AlsaSeq> make_real_alsa_seq();

} // namespace posix
} // namespace midibn
