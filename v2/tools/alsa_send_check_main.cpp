// midibn-alsa-send-check: measures how fast the ALSA output can send to one port, using ONLY silent messages.
//   midibn-alsa-send-check CLIENT:PORT [--seconds S] [--pattern direct|buffered|batched|all]
// CLIENT:PORT is a destination from midibn-alsa-ports. The messages are Control Change 20..31 on all 16 channels (undefined
// controllers, so they make no sound), the same set the Windows direct-driver check uses, and at the end the three panic
// controllers (CC120, CC123, CC64 = 0). It connects to the port you name and to nothing else.
// Reports, for each call pattern: messages sent, seconds, messages per second and nanoseconds per message, and the
// slowest 256-message burst. Do not point it at a port that feeds a synthesizer you did not mean to touch.
#include "alsa_midi_output.h"
#include "linux_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace midibn;

struct Result {
    long long sent = 0;
    long long errors = 0;
    double seconds = 0;
    double slowest_burst_us = 0;
    std::string first_error;
};

Result measure(posix::AlsaMidiOutput& output, double seconds)
{
    Result result;
    const int64_t limit = static_cast<int64_t>(seconds * 1e9);
    uint32_t n = 0;
    const int64_t start = posix::performance_counter();
    for (;;) {
        const int64_t burstStart = posix::performance_counter();
        {
            platform::SendBatch batch(&output);   // 256 sends per batch, as the engine sends a burst of due events
            for (int i = 0; i < 256; ++i, ++n) {
                uint32_t packed = 0xB0u | (n & 15u) | (static_cast<uint32_t>(20 + (n >> 4) % 12) << 8) |
                    (static_cast<uint32_t>((n * 7) & 127) << 16);
                platform::OutputStatus status = output.send_short(packed);
                if (!status.ok()) { if (result.errors == 0) result.first_error = status.message; ++result.errors; }
            }
        }
        const int64_t now = posix::performance_counter();
        result.sent += 256;
        double burst = static_cast<double>(now - burstStart) / 1000.0;
        if (burst > result.slowest_burst_us) result.slowest_burst_us = burst;
        if (now - start >= limit) { result.seconds = static_cast<double>(now - start) / 1e9; break; }
        if (result.errors > 1000) { result.seconds = static_cast<double>(now - start) / 1e9; break; }
    }
    return result;
}

const char* pattern_name(posix::AlsaSendPattern pattern)
{
    switch (pattern) {
    case posix::AlsaSendPattern::Direct: return "direct (event_output_direct per message)";
    case posix::AlsaSendPattern::BufferedEach: return "buffered (event_output + drain per message)";
    default: return "batched (event_output per message, one drain per 256)";
    }
}

} // namespace

int main(int argc, char** argv)
{
    int client = -1, port = -1;
    double seconds = 2.0;
    std::string which = "all";
    if (argc >= 2) std::sscanf(argv[1], "%d:%d", &client, &port);
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::atof(argv[i + 1]);
        else if (std::strcmp(argv[i], "--pattern") == 0) which = argv[i + 1];
    }
    if (client < 0 || port < 0 || seconds <= 0 || (which != "all" && which != "direct" && which != "buffered" && which != "batched")) {
        std::fprintf(stderr, "usage: midibn-alsa-send-check CLIENT:PORT [--seconds S] [--pattern direct|buffered|batched|all]\n"
                             "(list the ports with midibn-alsa-ports; only silent controller messages are sent)\n");
        return 2;
    }
    std::printf("ALSA send check: silent messages only (CC 20-31, panic controllers) to %d:%d, %.1f s per pattern\n", client, port, seconds);
    const posix::AlsaSendPattern patterns[3] = { posix::AlsaSendPattern::Direct, posix::AlsaSendPattern::BufferedEach, posix::AlsaSendPattern::Batched };
    const char* keys[3] = { "direct", "buffered", "batched" };
    int failures = 0;
    for (int p = 0; p < 3; ++p) {
        if (which != "all" && which != keys[p]) continue;
        posix::AlsaMidiOutput output;
        output.set_send_pattern(patterns[p]);
        platform::OutputStatus status = output.open(posix::AlsaMidiOutput::make_device_id(client, port));
        if (!status.ok()) { std::printf("%s\n", status.message.c_str()); return 1; }
        Result result = measure(output, seconds);
        platform::OutputStatus panic = output.panic();
        output.close();
        std::printf("%-58s %10lld sent  %7.0f msg/s  %8.0f ns/msg  slowest burst %.0f us  errors %lld%s%s\n", pattern_name(patterns[p]),
            result.sent, result.seconds > 0 ? result.sent / result.seconds : 0.0,
            result.sent > 0 ? result.seconds * 1e9 / result.sent : 0.0, result.slowest_burst_us, result.errors,
            result.first_error.empty() ? "" : "  first: ", result.first_error.c_str());
        if (result.errors > 0 || !panic.ok()) ++failures;
    }
    std::printf(failures == 0 ? "ALSA-SEND-CHECK: PASS\n" : "ALSA-SEND-CHECK: FAIL\n");
    return failures == 0 ? 0 : 1;
}
