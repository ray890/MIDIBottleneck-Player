#include "alsa_seq.h"

namespace midibn {
namespace posix {
namespace {

void ignore_library_message(const char*, int, const char*, int, const char*, ...) {}

class RealAlsaSeq : public AlsaSeq {
public:
    ~RealAlsaSeq() override { close(); }

    int open(const char* client_name) override
    {
        close();
        // libasound prints its own English diagnostics to stderr when /dev/snd/seq is missing; the player reports
        // the failure in its own words, so the library's message is silenced (process-wide, once).
        static bool silenced = (snd_lib_error_set_handler(ignore_library_message), true);
        (void)silenced;
        snd_seq_t* handle = nullptr;
        // "hw" is the sequencer device itself (/dev/snd/seq); no ALSA configuration file is needed.
        int result = snd_seq_open(&handle, "hw", SND_SEQ_OPEN_OUTPUT, 0);
        if (result < 0) return result;
        result = snd_seq_set_client_name(handle, client_name);
        if (result < 0) { snd_seq_close(handle); return result; }
        seq_ = handle;
        return 0;
    }

    bool is_open() const override { return seq_ != nullptr; }
    int client_id() const override { return seq_ != nullptr ? snd_seq_client_id(seq_) : -1; }

    int create_output_port(const char* port_name) override
    {
        if (seq_ == nullptr) return -EBADF;
        return snd_seq_create_simple_port(seq_, port_name, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    }

    int list_ports(std::vector<AlsaPortInfo>& ports) override
    {
        if (seq_ == nullptr) return -EBADF;
        snd_seq_client_info_t* client = nullptr;
        snd_seq_port_info_t* port = nullptr;
        snd_seq_client_info_alloca(&client);
        snd_seq_port_info_alloca(&port);
        snd_seq_client_info_set_client(client, -1);
        while (snd_seq_query_next_client(seq_, client) >= 0) {
            int id = snd_seq_client_info_get_client(client);
            snd_seq_port_info_set_client(port, id);
            snd_seq_port_info_set_port(port, -1);
            while (snd_seq_query_next_port(seq_, port) >= 0) {
                AlsaPortInfo info;
                info.client = id;
                info.port = snd_seq_port_info_get_port(port);
                info.client_name = snd_seq_client_info_get_name(client);
                info.port_name = snd_seq_port_info_get_name(port);
                info.capability = snd_seq_port_info_get_capability(port);
                info.type = snd_seq_port_info_get_type(port);
                ports.push_back(info);
            }
        }
        return 0;
    }

    int connect_to(int port, int dest_client, int dest_port) override
    {
        if (seq_ == nullptr) return -EBADF;
        return snd_seq_connect_to(seq_, port, dest_client, dest_port);
    }

    int output_direct(snd_seq_event_t& event) override
    {
        return seq_ != nullptr ? snd_seq_event_output_direct(seq_, &event) : -EBADF;
    }

    int output_buffered(snd_seq_event_t& event) override
    {
        return seq_ != nullptr ? snd_seq_event_output(seq_, &event) : -EBADF;
    }

    int drain() override { return seq_ != nullptr ? snd_seq_drain_output(seq_) : -EBADF; }
    int drop_output() override { return seq_ != nullptr ? snd_seq_drop_output(seq_) : -EBADF; }

    void close() override
    {
        if (seq_ != nullptr) snd_seq_close(seq_);
        seq_ = nullptr;
    }

private:
    snd_seq_t* seq_ = nullptr;
};

} // namespace

std::unique_ptr<AlsaSeq> make_real_alsa_seq() { return std::unique_ptr<AlsaSeq>(new RealAlsaSeq()); }

} // namespace posix
} // namespace midibn
