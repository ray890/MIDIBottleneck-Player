// midibn-midi-enum: ENUMERATION ONLY. Lists the WinMM output devices through the
// v2 backend (midiOutGetNumDevs and midiOutGetDevCapsW, nothing else) and
// reports whether an OmniMIDI.dll file exists where KDMAPI would look for it
// (file existence only; the file is never opened or loaded). It opens no
// device, sends no message and makes no sound.
#include "kdmapi_midi_output.h"
#include "winmm_midi_output.h"
#include "win32_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <string>

namespace {

const char* technology_name(unsigned technology)
{
    switch (technology) {
    case MOD_MIDIPORT: return "MIDI hardware port";
    case MOD_SYNTH: return "synthesizer";
    case MOD_SQSYNTH: return "square wave synthesizer";
    case MOD_FMSYNTH: return "FM synthesizer";
    case MOD_MAPPER: return "Microsoft MIDI mapper";
    case MOD_WAVETABLE: return "hardware wavetable synthesizer";
    case MOD_SWSYNTH: return "software synthesizer";
    default: return "unknown";
    }
}

std::string support_flags(unsigned support)
{
    std::string text;
    if (support & MIDICAPS_VOLUME) text += "VOLUME ";
    if (support & MIDICAPS_LRVOLUME) text += "LRVOLUME ";
    if (support & MIDICAPS_CACHE) text += "CACHE ";
    if (support & MIDICAPS_STREAM) text += "STREAM ";
    if (text.empty()) return "none";
    text.erase(text.size() - 1);
    return text;
}

std::string channel_list(unsigned mask)
{
    if ((mask & 0xFFFF) == 0xFFFF) return "all 16 channels";
    std::string text;
    for (int channel = 0; channel < 16; ++channel)
        if (mask & (1u << channel)) text += (text.empty() ? "" : ",") + std::to_string(channel + 1);
    return text.empty() ? "none" : "channels " + text;
}

} // namespace

int main()
{
    using namespace midibn;
    SetConsoleOutputCP(CP_UTF8);
    std::printf("midibn-midi-enum (enumeration only)\n");
    std::printf("Process: %s\n", win32::describe_os().c_str());

    std::vector<platform::OutputDeviceInfo> devices = win32::WinMmMidiOutput().enumerate();
    std::printf("WinMM output devices: %u\n", static_cast<unsigned>(devices.size()));
    for (size_t i = 0; i < devices.size(); ++i) {
        const platform::OutputDeviceInfo& d = devices[i];
        std::printf("  [%u] %s\n", d.device_id, d.name.c_str());
        std::printf("      technology: %u (%s)\n", static_cast<unsigned>(d.technology), technology_name(d.technology));
        std::printf("      voices: %u   notes: %u   channel mask: 0x%04X (%s)\n", static_cast<unsigned>(d.voices),
            static_cast<unsigned>(d.notes), static_cast<unsigned>(d.channel_mask), channel_list(d.channel_mask).c_str());
        std::printf("      support: 0x%08X (%s)\n", static_cast<unsigned>(d.support), support_flags(d.support).c_str());
        std::printf("      manufacturer id: %u   product id: %u   driver version: %u.%u\n",
            static_cast<unsigned>(d.manufacturer_id), static_cast<unsigned>(d.product_id),
            static_cast<unsigned>(d.driver_version >> 8), static_cast<unsigned>(d.driver_version & 0xFF));
    }
    std::printf("winmm.dll module: %s\n", win32::WinMmMidiOutput::loaded_module_path().c_str());

    std::vector<win32::KdmApiProviderCandidate> candidates = win32::kdmapi_provider_candidates();
    std::printf("OmniMIDI.dll (file existence only, not loaded):\n");
    for (size_t i = 0; i < candidates.size(); ++i)
        std::printf("  %s: %s - %s\n", candidates[i].description.c_str(), candidates[i].path.c_str(),
            candidates[i].exists ? "exists" : "not found");
    return 0;
}
