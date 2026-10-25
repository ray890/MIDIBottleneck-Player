// midibn-alsa-ports: lists the ALSA sequencer ports the player could send to. It opens the sequencer only to read the
// list; it creates no port, subscribes to nothing and sends nothing.
//   midibn-alsa-ports
// Prints one line per destination: "<client>:<port>  <client name>: <port name>". Exit code 0 (also with an empty list),
// 1 when the sequencer cannot be read (the reason is printed).
#include "alsa_midi_output.h"

#include <cstdio>

int main()
{
    using namespace midibn;
    posix::AlsaMidiOutput output;
    std::vector<platform::OutputDeviceInfo> devices = output.enumerate();
    if (!output.last_enumeration_error().empty()) {
        std::printf("%s\n", output.last_enumeration_error().c_str());
        return 1;
    }
    if (devices.empty()) std::printf("No MIDI output ports were found in the ALSA sequencer.\n");
    for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("%d:%d  %s\n", posix::AlsaMidiOutput::device_client(devices[i].device_id),
            posix::AlsaMidiOutput::device_port(devices[i].device_id), devices[i].name.c_str());
    }
    return 0;
}
