// MIDIBottleneck Player v2 for Linux (GTK 3). Switches (all optional; for tests and pictures):
//   FILE | --open FILE                  load FILE at start
//   --play [--rate N]                   after loading, start playback on output None (never opens a MIDI port)
//   --output none                       the output for --play and --smoke (only "none" is accepted: tests never open a port)
//   --scale P  --width W                Application scale 50..200, initial window width
//   --position X,Y                      window position
//   --auto-close-ms N                   close after N ms
//   --capture FILE.png [--capture-after-ms N] [--capture-window main|dialog|help|about|analysis|channels|routing]
//                                       write a PNG of a window after N ms, then close (run only under Xvfb for pictures)
//   --show help|about|analysis|channels|routing [--help-topic N]   open one more window at start
//   --force-large-warning               show the large-MIDI memory warning for any file
//   --force-one-core                    act as a one-physical-core PC (shows the few-processor menu item)
//   --settings FILE                     keep the remembered settings in FILE instead of the user's own settings file
//   --no-settings                       start from the defaults and save nothing (--smoke, --capture and --auto-close-ms launches do the same
//                                       on their own unless --settings names a file)
//   --settings-change                   test: change a few options at start so the saved file can be checked
//   --adaptive-display off | --display-tier N   adaptive display rate switches
//   --strict                            exit code 3 when any GLib/GTK warning or critical was logged
//   --smoke                           scripted check on output None (needs --open); prints SMOKE lines and the exit code
#include "main_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace midibn::gtkui;

int main(int argc, char** argv)
{
    Options options;
    std::string output = "none";
    bool strict = false;
    // GTK takes its own switches (--display, --gtk-module ...) out of argv first.
    if (!gtk_init_check(&argc, &argv)) {
        std::fprintf(stderr, "midibn-gtk: no display is available (set DISPLAY).\n");
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--open" && has_value) options.open_file = argv[++i];
        else if (a == "--play") options.play = true;
        else if (a == "--rate" && has_value) options.select_rate = std::atoi(argv[++i]);
        else if (a == "--output" && has_value) output = argv[++i];
        else if (a == "--scale" && has_value) { options.scale_percent = std::atoi(argv[++i]); options.scale_given = true; }
        else if (a == "--settings" && has_value) options.settings_path = argv[++i];
        else if (a == "--no-settings") options.no_settings = true;
        else if (a == "--settings-change") options.settings_change = true;
        else if (a == "--width" && has_value) options.client_width = std::atoi(argv[++i]);
        else if (a == "--position" && has_value) {
            if (std::sscanf(argv[++i], "%d,%d", &options.position_x, &options.position_y) == 2) options.has_position = true;
        }
        else if (a == "--auto-close-ms" && has_value) options.auto_close_ms = std::atoi(argv[++i]);
        else if (a == "--capture" && has_value) options.capture_file = argv[++i];
        else if (a == "--capture-after-ms" && has_value) options.capture_after_ms = std::atoi(argv[++i]);
        else if (a == "--capture-window" && has_value) options.capture_window = argv[++i];
        else if (a == "--show" && has_value) options.show = argv[++i];
        else if (a == "--help-topic" && has_value) options.help_topic = std::atoi(argv[++i]);
        else if (a == "--force-large-warning") options.force_large_warning = true;
        else if (a == "--force-one-core") options.force_one_core = true;
        else if (a == "--adaptive-display" && has_value) options.adaptive_display = std::string(argv[++i]) != "off";
        else if (a == "--display-tier" && has_value) options.display_fixed_tier = std::atoi(argv[++i]);
        else if (a == "--smoke") options.smoke = true;
        else if (a == "--strict") strict = true;
        else if (options.open_file.empty() && a.compare(0, 2, "--") != 0) options.open_file = a;
        else {
            std::fprintf(stderr, "midibn-gtk: unknown or incomplete switch: %s\n", a.c_str());
            return 2;
        }
    }
    if (output != "none") {
        std::fprintf(stderr, "midibn-gtk: --output accepts only \"none\" (the scripted switches never open a MIDI port).\n");
        return 2;
    }
    install_log_counter();
    g_set_prgname("midibn-gtk");
    g_set_application_name(kProductName);
    MainWindow window;
    if (!window.create(options)) return 1;
    gtk_main();
    if (strict && logged_problems() > 0) {
        std::fprintf(stderr, "midibn-gtk: %d GLib/GTK warnings or criticals were logged (--strict)\n", logged_problems());
        return 3;
    }
    return window.exit_code();
}
