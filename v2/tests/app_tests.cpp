// Tests of the main window's behaviour layer (app::PlayerController): the Rate model
// state machine, queue/overflow settings, play preconditions, load orchestration and
// transport on the None output. No device is opened and no provider is loaded: the
// WinMM and KDMAPI slots are filled with recording outputs.
//
// Usage: midibn-app-tests [fixtureDirectory]   (default: v2\harness\fixtures)
#include "midibn/app/player_controller.h"
#include "midibn/app/ui_mappings.h"
#include "midibn/null_output.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace midibn;
using namespace midibn::app;

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

// A recording output that stands in for the direct WinMM output (DD-085): it can refuse to open, like an unsupported device.
class FakeDirectOutput : public platform::RecordingMidiOutput {
public:
    bool refuse = false;
    int opens = 0;
    int closes = 0;
    // DD-094: a send that stays inside the "driver" while block_sends is set (a synthesizer whose queue stays full)
    std::atomic<bool> block_sends{ false };
    std::atomic<int> blocked{ 0 };
    std::atomic<bool> stopped_answering{ false };   // DD-094: a reset or close of the "driver" did not return in time
    bool driver_stopped_answering() const override { return stopped_answering.load(); }
    platform::OutputStatus open(uint32_t id) override
    {
        ++opens;
        if (refuse) return platform::OutputStatus::failure(platform::OutputErrorKind::NotSupported, "no installed MIDI driver reports this device");
        return platform::RecordingMidiOutput::open(id);
    }
    platform::OutputStatus close() override
    {
        ++closes;
        return platform::RecordingMidiOutput::close();
    }
    platform::OutputStatus send_short(uint32_t packed) override
    {
        if (block_sends.load()) {
            blocked.store(1);
            while (block_sends.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return platform::RecordingMidiOutput::send_short(packed);
    }
    std::string path_description() const override { return "Direct WinMM driver access: Fake.dll (tested driver)"; }
};

struct Fixture {
    win32::Win32FileSystem files;
    std::shared_ptr<FakeDirectOutput> direct = std::make_shared<FakeDirectOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> winmm = std::make_shared<platform::RecordingMidiOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> kdmapi = std::make_shared<platform::RecordingMidiOutput>();
    std::unique_ptr<PlayerController> controller;
    std::atomic<int> wakes{ 0 };

    explicit Fixture(std::function<DirectProbeResult(uint32_t)> probe = nullptr, bool withMapper = false, const RememberedSettings* initial = nullptr)
    {
        PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.probe_direct = probe;
        env.list_devices = [withMapper] {
            std::vector<platform::OutputDeviceInfo> devices(2);
            devices[0].device_id = 0; devices[0].name = "Fake device A";
            devices[1].device_id = 1; devices[1].name = "Fake device B";
            if (withMapper) { devices.resize(3); devices[2].device_id = 0xFFFFFFFFu; devices[2].name = "Fake MIDI Mapper"; }
            return devices;
        };
        env.create_winmm = [this] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env.create_kdmapi = [this] { return std::shared_ptr<platform::MidiOutput>(kdmapi); };
        env.create_winmm_direct = [this] { return std::shared_ptr<platform::MidiOutput>(direct); };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.wake_ui = [this] { ++wakes; };
        if (initial) { env.has_initial_settings = true; env.initial_settings = *initial; }
        controller.reset(new PlayerController(env));
    }
    const PlayerView& v() { return controller->view(); }
    bool wait_loaded(int ms)
    {
        for (int i = 0; i < ms / 5; ++i) {
            controller->pump();
            if (controller->has_song() || !controller->loading()) return controller->has_song();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

void test_initial_state()
{
    Fixture f;
    const PlayerView& v = f.v();
    check(v.output_items.size() == 3 && v.output_items[0] == "None" && v.output_items[1] == "Fake device A", "output list: None then the devices");
    check(v.output_selected == 1, "default output is the first real device");
    check(v.rate_choice == RateModelChoice::None, "Rate model starts at None");
    check(!v.processing_value.visible && v.service_value_label.empty(), "no rate value shown while Rate model is None");
    check(!v.play_enabled, "Play disabled before a file is loaded");
    check(v.queue_value.value == 2000 && v.queue_value.minimum == 1 && v.queue_value.maximum == 1000000, "queue limit 2000 in 1..1,000,000");
    check(!v.queue_check_checked && !v.queue_value.enabled && !v.overflow_enabled, "queue value and overflow disabled while the limit is off");
    // DD-025: complete-note choices first, then the single-event choices; the default policy is still Drop newest events.
    check(v.overflow_items.size() == 6 && v.overflow_items[0] == "Drop incoming complete notes" &&
        v.overflow_items[1] == "Drop oldest complete note" && v.overflow_items[2] == "Drop newest events" &&
        v.overflow_items[3] == "Drop oldest events" && v.overflow_items[4] == "Clear buffer and jump to realtime" &&
        v.overflow_items[5] == "Drop background notes", "six overflow choices in the DD-025 order");
    check(v.overflow_selected == 2 && f.controller->engine().overflow_policy() == engine::OverflowPolicy::DropNewest,
        "default overflow choice is Drop newest events");
    check(v.menu.chase_checked && !v.menu.preserve_checked && !v.menu.output_paced_checked, "menu defaults: chase on, others off");
    check(v.state_text == "Stopped", "state Stopped");
    check(v.statistics.values[0] == "00:00.000 / 00:00.000", "timeline statistic at zero");
}

void test_rate_models()
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.select_rate_choice(RateModelChoice::ProcessingTime);
    {
        const PlayerView& v = f.v();
        check(v.processing_value.visible && v.service_value_label == "Processing time per event:" && v.service_unit == "\xC2\xB5s", "Processing time labels");
        check(v.processing_value.value == 100 && v.slider_value == microseconds_to_slider(100), "Processing time value 100 and matching slider");
        check(v.processing_value.enabled && v.slider_enabled, "Processing time controls enabled");
        check(c.engine().simulate_slowdown(), "Processing time simulates slowdown");
    }
    c.select_rate_choice(RateModelChoice::MidiBitrate);
    {
        const PlayerView& v = f.v();
        check(v.service_value_label == "MIDI bitrate:" && v.service_unit == "bit/s" && v.din_visible, "Bitrate labels and DIN button");
        check(v.processing_value.value == c.engine().midi_bitrate(), "bitrate value follows the engine");
        c.din_preset();
        check(f.v().processing_value.value == 31250, "5-pin DIN sets 31,250");
        c.set_processing_value(500000);
        check(c.engine().midi_bitrate() == 500000 && f.v().slider_value == bitrate_to_slider(500000), "typing a bitrate updates engine and slider");
    }
    c.select_rate_choice(RateModelChoice::EventsPerSecond);
    {
        const PlayerView& v = f.v();
        check(v.service_value_label == "Events/sec:" && v.processing_value.value == 10000 && !v.din_visible, "Events/sec labels and remembered 10,000");
        c.set_slider(5000);
        check(f.v().processing_value.value == slider_to_event_rate(5000), "slider moves the events/sec value");
    }
    c.select_rate_choice(RateModelChoice::PerNoteIntervalGate);
    {
        const PlayerView& v = f.v();
        check(v.queue_check_text == "Interval polyphony limit:" && v.events_label == "note messages/interval", "interval gate relabels the queue limit");
        check(v.queue_value.maximum == 128 && !v.overflow_enabled, "interval limit 1..128, overflow not applicable");
        check(v.service_value_label == "Processing time per event:" && v.processing_value.value == 100, "gate interval remembered 100 us");
        check(!c.engine().simulate_slowdown(), "gates do not simulate slowdown");
    }
    c.select_rate_choice(RateModelChoice::DelayedNoteOff);
    check(f.v().service_value_label == "Minimum NoteOn duration:", "Delayed NoteOff label");
    c.select_rate_choice(RateModelChoice::NoteAttackLimit);
    check(f.v().service_value_label == "Note starts/sec:" && f.v().processing_value.value == 100, "Note start rate gate label and remembered 100");
    c.select_rate_choice(RateModelChoice::OutputRateLimit);
    check(f.v().service_value_label == "Maximum output rate:" && f.v().processing_value.value == 10000, "Output rate gate label and remembered 10,000");
    c.select_rate_choice(RateModelChoice::None);
    {
        const PlayerView& v = f.v();
        check(!v.processing_value.visible && !c.engine().simulate_slowdown(), "back to None hides the value and stops slowdown");
        check(v.queue_check_text == "Queue length limit:", "queue limit label restored");
    }
}

void test_queue_settings()
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.set_queue_limit_checked(true);
    check(f.v().queue_value.enabled && f.v().overflow_enabled, "enabling the queue limit enables value and overflow");
    c.set_queue_value(500);
    check(c.engine().queue_length_limit() == 500, "queue limit reaches the engine");
    c.toggle_queue_age_limit();
    {
        const PlayerView& v = f.v();
        check(v.queue_check_text == "Queue waiting-time limit:" && v.events_label == "\xC2\xB5s", "age limit relabels the queue limit");
        check(v.overflow_items.size() == 3 && v.overflow_items[0] == "Clear buffer and jump to realtime" &&
            v.overflow_items[1] == "Drop oldest complete note (soft limit)" && v.overflow_items[2] == "Drop oldest events",
            "age mode overflow choices (Clear buffer first)");
        check(v.overflow_selected == 0 && c.engine().overflow_policy() == engine::OverflowPolicy::ClearBufferAndCatchUp,
            "age mode defaults to Clear buffer and jump to realtime");
    }
    // The choice is remembered by policy, not by position: pick Drop oldest events, leave and re-enter age mode.
    c.set_overflow_selection(2);
    check(c.engine().overflow_policy() == engine::OverflowPolicy::DropOldest, "age mode index 2 is Drop oldest events");
    c.toggle_queue_age_limit();
    check(c.engine().overflow_policy() == engine::OverflowPolicy::DropNewest, "event mode keeps its own remembered policy");
    c.toggle_queue_age_limit();
    {
        const PlayerView& v = f.v();
        check(v.overflow_selected == 2 && c.engine().overflow_policy() == engine::OverflowPolicy::DropOldest,
            "age mode restores Drop oldest events by policy");
        check(v.queue_value.value == 1000000 && v.queue_value.maximum == 1800000000, "age limit 1,000,000 us up to 30 minutes");
    }
    c.toggle_queue_age_limit();
    check(f.v().overflow_items.size() == 6 && f.v().queue_value.value == 500, "leaving age mode restores the event count 500");
}

void test_forward_queue_precondition(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    check(f.wait_loaded(10000), "fixture loads (forward-queue test)");
    c.toggle_apply_queue_limit_without_slowdown();
    c.set_queue_limit_checked(true);    // with Rate model None this switches to the last ordinary model
    check(c.engine().apply_queue_limit_without_slowdown() && f.v().rate_choice == RateModelChoice::ProcessingTime,
        "forward-only queue selects an ordinary Rate model");
    check(f.v().rate_forward_heading, "Rate model popup shows the forward-only heading");
    check(f.v().play_enabled, "Drop newest events is allowed without slowdown");
    c.set_overflow_selection(3);        // Drop oldest events cannot retract sent MIDI
    check(c.engine().overflow_policy() == engine::OverflowPolicy::DropOldest, "index 3 is Drop oldest events");
    check(!f.v().play_enabled && f.v().play_tooltip.find("requires simulated slowdown") != std::string::npos,
        "Drop oldest events without slowdown disables Play with an explanation");
    c.set_overflow_selection(0);        // Drop incoming complete notes is allowed without slowdown
    check(c.engine().overflow_policy() == engine::OverflowPolicy::DropIncomingCompleteNotes && f.v().play_enabled,
        "Drop incoming complete notes is allowed without slowdown");
    c.set_overflow_selection(2);
    check(c.engine().overflow_policy() == engine::OverflowPolicy::DropNewest && f.v().play_enabled, "Drop newest events enables Play again");
}

void test_load_and_transport(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    std::string path = fixtureDir + "\\tempo-changes.mid";
    c.play_clicked();
    std::vector<UiMessage> messages = c.take_messages();
    check(messages.size() == 1 && messages[0].title == "No MIDI file" && messages[0].text == "Open a MIDI file first.", "Play without a file says so");
    c.open_file(path);
    check(f.wait_loaded(10000), "fixture loads");
    check(f.v().play_enabled && !f.v().file_info.empty() && f.v().duration_us > 0, "after load: Play enabled, information and duration shown");
    check(f.v().file_text == "tempo-changes.mid", "file name shown");
    c.select_output(0);
    c.play_clicked();
    std::string state = f.v().state_text;
    check(state == "Playing" || state == "Completed", "Play starts on None (" + state + ")");
    if (state == "Playing") {
        c.play_clicked();
        check(f.v().state_text == "Paused" && f.v().play_text == "Resume", "Play pauses; button reads Resume");
        c.play_clicked();
        check(f.v().state_text == "Playing" || f.v().state_text == "Completed", "Resume resumes");
    }
    c.seek_to(f.v().duration_us / 2);
    c.stop_clicked();
    check(f.v().state_text == "Stopped" && c.selected_position() == 0, "Stop stops and rewinds");
    // a second load replaces the first
    c.open_file(fixtureDir + "\\f0-all-message-types.mid");
    check(f.wait_loaded(10000), "second fixture loads");
    check(f.v().file_text == "f0-all-message-types.mid", "second file name shown");
    // a missing file reports an error and leaves nothing loaded
    c.open_file(fixtureDir + "\\no-such-file.mid");
    f.wait_loaded(3000);
    messages = c.take_messages();
    check(!c.has_song() && messages.size() >= 1 && messages[0].title == "Unable to load MIDI file", "missing file: error message, nothing loaded");
}

void test_kdmapi_uses_recording_output(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    check(f.wait_loaded(10000), "fixture loads (KDMAPI test)");
    c.set_kdmapi(true);
    check(f.v().kdmapi_checked && !f.v().output_enabled, "KDMAPI checked disables the output list");
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    c.stop_clicked();
    check(!f.kdmapi->messages.empty(), "playback sent to the (recording) KDMAPI slot");
    check(f.winmm->messages.empty(), "nothing sent to the WinMM slot");
}

// DD-085 / P12-CHK: the "Direct WinMM" checkbox: off by default, session-only, never a silent fallback.
void test_direct_winmm(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    check(!c.direct_winmm() && f.v().output_path_text.empty() && !f.v().direct_checked && f.v().direct_enabled &&
            f.v().direct_tooltip.find("Direct mode is off") != std::string::npos && f.v().direct_tooltip.find("Experimental") == std::string::npos &&
            f.v().direct_tooltip.find("ModMessage") == std::string::npos, "direct WinMM starts off, enabled, with its tooltip");
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    check(f.wait_loaded(10000), "fixture loads (direct WinMM test)");
    // off: playback uses the standard WinMM slot
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.stop_clicked();
    check(!f.winmm->messages.empty() && f.direct->messages.empty() && f.direct->opens == 0, "option off: standard WinMM plays, the direct output is not touched");
    f.winmm->messages.clear();

    c.toggle_direct_winmm();
    check(c.direct_winmm() && f.v().direct_checked && f.v().direct_enabled, "toggle on");
    check(f.v().direct_tooltip.find("checks the selected device when you press Play") != std::string::npos, "on, not yet played: the tooltip says the device is checked at Play");
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(f.v().direct_tooltip.find("Direct mode is in use") != std::string::npos && f.v().direct_tooltip.find("Fake.dll") != std::string::npos &&
        f.v().direct_tooltip.find("has been tested") != std::string::npos && f.v().direct_enabled, "in use: the tooltip names the driver and says it is tested");
    c.stop_clicked();
    check(!f.direct->messages.empty() && f.winmm->messages.empty(), "option on and device supported: the direct output plays, standard WinMM does not");
    f.direct->messages.clear();

    // DD-111: Stop leaves the direct output open; the next Play and Stop do not close it either
    const int closesAfterStop = f.direct->closes;
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.stop_clicked();
    check(f.direct->closes == closesAfterStop, "direct output: Stop and the next Play do not close it (kept open)");
    f.direct->messages.clear();

    // a device the driver path cannot serve: standard WinMM plays and the status line says why
    f.direct->refuse = true;
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string text = f.v().direct_tooltip;
    check(text.find("not in use for Fake device A") != std::string::npos && text.find("No installed MIDI driver reports this device") != std::string::npos &&
        text.find("play through Windows as usual") != std::string::npos, "unsupported device: the tooltip gives the device, the reason and the path in use (" + text + ")");
    check(f.v().direct_checked && !f.v().direct_enabled, "unsupported device: the box is greyed out and keeps its tick");
    c.stop_clicked();
    check(!f.winmm->messages.empty() && f.direct->messages.empty(), "unsupported device: playback went through standard WinMM");
    check(c.take_messages().empty(), "unsupported device: no blocking message box");
    f.winmm->messages.clear();

    // another device resets the outcome; None and KDMAPI do not use the option
    const int closesBeforeDeviceChange = f.direct->closes;
    c.select_output(2);
    check(f.direct->closes == closesBeforeDeviceChange + 1, "direct output: a device change while stopped closes the kept-open output (DD-111)");
    check(f.v().direct_enabled && f.v().direct_tooltip.find("checks the selected device when you press Play") != std::string::npos, "another device: enabled again, the tooltip asks again at Play");
    c.select_output(0);
    check(!f.v().direct_enabled && f.v().direct_checked && f.v().direct_tooltip.find("output is None") != std::string::npos, "output None: greyed out, tick kept, the tooltip says why");
    c.select_output(1);
    c.set_kdmapi(true);
    check(!f.v().direct_enabled && f.v().direct_checked && f.v().direct_tooltip.find("KDMAPI is ticked") != std::string::npos, "KDMAPI: greyed out, tick kept, the tooltip says why");
    c.set_kdmapi(false);

    // off again: no status, standard path
    c.toggle_direct_winmm();
    check(!c.direct_winmm() && !f.v().direct_checked && f.v().direct_enabled && f.v().direct_tooltip.find("Direct mode is off") != std::string::npos, "toggle off: the tooltip says it is off");
    f.direct->refuse = false;
    f.direct->opens = 0;
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.stop_clicked();
    check(f.direct->opens == 0 && !f.winmm->messages.empty(), "toggle off: back to standard WinMM");
}

// DD-094: Stop gives up after its bounded wait while a send is stuck inside the driver; Play then waits for that call
// instead of closing and reopening the output underneath it, and works again once the call has returned.
void test_play_waits_for_stuck_output(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    check(f.wait_loaded(10000), "fixture loads (stuck output test)");
    c.engine().set_worker_stop_timeout_milliseconds(100);
    c.toggle_direct_winmm();
    f.direct->block_sends = true;
    c.play_clicked();
    for (int i = 0; i < 400 && f.direct->blocked.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(f.direct->blocked.load() != 0, "stuck output: a send is held inside the fake driver");
    c.stop_clicked();
    std::vector<UiMessage> messages = c.take_messages();
    check(messages.size() == 1 && messages[0].title == "Unable to stop MIDI output safely", "stuck output: Stop returns after its bounded wait and says so");
    const int opens = f.direct->opens, closes = f.direct->closes;
    c.play_clicked();
    messages = c.take_messages();
    check(messages.size() == 1 && messages[0].title == "MIDI output is still stopping" && f.v().state_text == "Stopped",
        "stuck output: Play warns that the output is still stopping and does not start");
    check(f.direct->opens == opens && f.direct->closes == closes, "stuck output: the output is neither closed nor reopened underneath the stuck call");
    f.direct->block_sends = false;   // the driver call returns
    for (int i = 0; i < 400 && c.engine().has_live_worker(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(!c.engine().has_live_worker(), "stuck output: the old worker ends once its call has returned");
    c.play_clicked();
    std::string state = f.v().state_text;
    check((state == "Playing" || state == "Completed") && c.take_messages().empty(), "stuck output: Play works again afterwards (" + state + ")");
    c.stop_clicked();
}

// DD-094: when the direct path's driver stopped answering, the status line says so and the next Play uses standard WinMM
// (the real output then refuses to open that driver; here the fake refuses as an unsupported device would).
void test_status_when_driver_stopped_answering(const std::string& fixtureDir)
{
    Fixture f;
    PlayerController& c = *f.controller;
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    check(f.wait_loaded(10000), "fixture loads (stopped answering test)");
    c.toggle_direct_winmm();
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.stop_clicked();
    check(f.v().direct_tooltip.find("Fake.dll") != std::string::npos, "stopped answering: before, the tooltip names the driver in use");
    f.direct->stopped_answering = true;
    check(f.v().direct_tooltip.find("Direct mode has stopped") != std::string::npos && f.v().direct_tooltip.find("did not answer") != std::string::npos,
        "stopped answering: the tooltip says so (" + f.v().direct_tooltip + ")");
    f.direct->refuse = true;
    f.winmm->messages.clear();
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.stop_clicked();
    check(!f.winmm->messages.empty() && f.v().direct_tooltip.find("play through Windows as usual") != std::string::npos,
        "stopped answering: the next playback goes through standard WinMM and the tooltip says why");
    c.take_messages();
}

// The effective-speed window changes within the session; remembering it is tested in test_remembered_settings.
void test_speed_window_session_only()
{
    {
        Fixture f;
        check(f.controller->effective_speed_window() == 250000, "the effective-speed window starts at 250 ms");
        f.controller->set_effective_speed_window(500000);
        check(f.controller->effective_speed_window() == 500000, "the effective-speed window changes within the session");
        f.controller->set_effective_speed_window(1234);   // invalid: ignored
        check(f.controller->effective_speed_window() == 500000, "an invalid speed window is rejected");
    }
    Fixture g;
    check(g.controller->effective_speed_window() == 250000, "a new controller without remembered settings starts at the default again");
}

// Remembered settings (prompt 013, Step 4): the controller applies them at construction and reports what to save.
void test_remembered_settings()
{
    {   // defaults: nothing remembered means the program's own start state, and nothing to say
        Fixture f;
        RememberedSettings d = f.controller->remembered_settings();
        RememberedSettings expected;
        expected.output_name.clear();
        check(d == expected, "defaults: the controller reports the default settings (no output chosen yet)");
        check(f.v().file_info.empty(), "defaults: no note in the file-information area");
    }
    {   // a full set is applied
        RememberedSettings s;
        s.output_name = "Fake device B";
        s.kdmapi = true;
        s.direct_winmm = true;
        s.chase_midi_state = false;
        s.preserve_queue = true;
        s.output_paced = true;
        s.scale_note_interval = true;
        s.queue_age_limit = true;
        s.speed_window_us = 1000000;
        Fixture f(nullptr, false, &s);
        const PlayerView& v = f.v();
        check(v.output_selected == 2, "remembered output: the device with that name is selected");
        check(v.kdmapi_checked && v.direct_checked, "remembered KDMAPI and Direct WinMM ticks are restored");
        check(!v.menu.chase_checked && v.menu.preserve_checked && v.menu.output_paced_checked && v.menu.scale_interval_checked &&
            v.menu.queue_age_checked && !v.menu.apply_queue_limit_checked, "remembered system-menu options are restored");
        check(f.controller->effective_speed_window() == 1000000, "remembered effective-speed window is restored");
        RememberedSettings back = f.controller->remembered_settings();
        check(back == s, "what the controller reports to save equals what was restored");
        f.controller->select_output(1);
        check(f.controller->remembered_settings().output_name == "Fake device A", "choosing an output updates the remembered name");
    }
    {   // apply-without-slowdown (excludes the age limit)
        RememberedSettings s;
        s.apply_queue_limit = true;
        Fixture f(nullptr, false, &s);
        check(f.v().menu.apply_queue_limit_checked && !f.v().menu.queue_age_checked, "remembered 'apply queue limit' is restored");
    }
    {   // a device that is gone: the normal default, but the remembered name stays until the user chooses
        RememberedSettings s;
        s.output_name = "Unplugged synth";
        Fixture f(nullptr, false, &s);
        check(f.v().output_selected == 1, "missing remembered device: the normal default output");
        check(f.controller->remembered_settings().output_name == "Unplugged synth", "missing remembered device: its name is kept for later");
    }
    {   // "None" is a valid remembered choice
        RememberedSettings s;
        s.output_name = "None";
        Fixture f(nullptr, false, &s);
        check(f.v().output_selected == 0, "remembered 'None' is restored");
    }
    {   // the note: a plain sentence in the file-information area while no file is loaded; details in the tooltip
        Fixture f;
        f.controller->set_settings_note("Your saved settings could not be read.", "1 entry");
        f.controller->refresh();
        check(f.v().file_info == "Your saved settings could not be read." && f.v().file_info_tooltip.find("Details: 1 entry") != std::string::npos,
            "settings note: sentence in the information area, 'Details:' below it in the tooltip");
    }
}

// DD-057: with PlayerEnvironment::defer_device_listing the controller lists the output devices at the first moment it needs
// them (not in its constructor), shows only "None" until then, and ends up with the same list, selection and playback
// behaviour as an eager controller; the first refresh() after construction lists them.
void test_deferred_device_listing(const std::string& fixtureDir)
{
    for (int mode = 0; mode < 2; ++mode) {   // 0: eager, 1: deferred
        std::atomic<int> listed{ 0 };
        win32::Win32FileSystem files;
        auto winmm = std::make_shared<platform::RecordingMidiOutput>();
        PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.list_devices = [&listed] {
            ++listed;
            std::vector<platform::OutputDeviceInfo> devices(2);
            devices[0].device_id = 0; devices[0].name = "Fake device A";
            devices[1].device_id = 1; devices[1].name = "Fake device B";
            return devices;
        };
        env.create_winmm = [winmm] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.defer_device_listing = mode == 1;
        PlayerController c(env);
        const std::string label = mode == 0 ? "eager: " : "deferred: ";
        if (mode == 0) check(listed.load() == 1 && c.view().output_items.size() == 3, label + "devices are listed by the constructor");
        else check(listed.load() == 0 && c.view().output_items.size() == 1 && c.view().output_items[0] == "None",
            label + "the constructor lists nothing and the view shows only None");
        c.refresh();
        check(listed.load() == 1, label + "the first refresh lists the devices once");
        check(c.view().output_items.size() == 3 && c.view().output_items[1] == "Fake device A" && c.view().output_selected == 1 &&
            c.output_selection() == 1, label + "the list and the default selection (first real device) are the same");
        c.refresh();
        c.select_output(2);
        check(listed.load() == 1 && c.output_selection() == 2, label + "the devices are listed only once");
    }
    {   // Selecting and playing before anything has refreshed the view must still use the full list (fail closed to None).
        std::atomic<int> listed{ 0 };
        win32::Win32FileSystem files;
        PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.list_devices = [&listed] {
            ++listed;
            std::vector<platform::OutputDeviceInfo> devices(1);
            devices[0].device_id = 0; devices[0].name = "Fake device A";
            return devices;
        };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.defer_device_listing = true;
        PlayerController c(env);
        check(listed.load() == 0, "deferred: nothing listed yet");
        c.select_output(0);
        check(listed.load() == 1 && c.output_selection() == 0, "deferred: choosing None first lists the devices, then selects None");
        c.open_file(fixtureDir + "\\tempo-changes.mid");
        for (int i = 0; i < 2000 && !c.has_song(); ++i) { c.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        check(c.has_song(), "deferred: fixture loads");
        c.play_clicked();
        const std::string state = c.view().state_text;
        check(state == "Playing" || state == "Completed", "deferred: Play on None starts (" + state + ")");
        c.stop_clicked();
    }
}

// U17: refresh_devices() lists the outputs again without rebuilding the controller: the loaded file and the options stay, the
// selection stays on the same device by name, a vanished device falls back to None with a warning, None stays None.
void test_refresh_devices(const std::string& fixtureDir)
{
    std::vector<std::string> present = { "Fake device A", "Fake device B" };
    std::atomic<int> listed{ 0 };
    win32::Win32FileSystem files;
    auto winmm = std::make_shared<platform::RecordingMidiOutput>();
    PlayerEnvironment env;
    env.services = std::make_shared<win32::Win32EngineServices>();
    env.files = &files;
    env.list_devices = [&] {
        ++listed;
        std::vector<platform::OutputDeviceInfo> devices(present.size());
        for (size_t i = 0; i < present.size(); ++i) { devices[i].device_id = static_cast<uint32_t>(i); devices[i].name = present[i]; }
        return devices;
    };
    env.create_winmm = [winmm] { return std::shared_ptr<platform::MidiOutput>(winmm); };
    env.pointer_size = static_cast<int>(sizeof(void*));
    PlayerController c(env);
    c.open_file(fixtureDir + "\\tempo-changes.mid");
    for (int i = 0; i < 2000 && !c.has_song(); ++i) { c.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    check(c.has_song(), "refresh_devices: fixture loads");
    c.select_rate_choice(RateModelChoice::MidiBitrate);
    c.set_queue_limit_checked(true);
    c.set_queue_value(777);
    c.select_output(2);
    check(listed.load() == 1 && c.output_selection() == 2, "refresh_devices: starts on device B");
    c.take_messages();

    // A device is added in front: the selection follows device B to its new place; the file and options stay.
    present = { "New device", "Fake device A", "Fake device B" };
    c.refresh_devices();
    check(listed.load() == 2 && c.view().output_items.size() == 4 && c.view().output_items[1] == "New device", "refresh_devices: the new list is shown");
    check(c.output_selection() == 3 && c.view().output_selected == 3, "refresh_devices: the selection follows device B by name");
    check(c.has_song() && c.view().rate_choice == RateModelChoice::MidiBitrate && c.view().queue_value.value == 777 && c.view().queue_check_checked,
        "refresh_devices: the file and the options stay");
    check(c.take_messages().empty(), "refresh_devices: no message when the device is still there");

    // The selected device vanishes: None, with a warning.
    present = { "New device", "Fake device A" };
    c.refresh_devices();
    check(c.view().output_items.size() == 3 && c.output_selection() == 0 && c.view().output_selected == 0, "refresh_devices: a vanished device falls back to None");
    std::vector<UiMessage> messages = c.take_messages();
    check(messages.size() == 1 && messages[0].kind == UiMessage::Warning && messages[0].text.find("Fake device B") != std::string::npos && messages[0].text.find("None") != std::string::npos,
        "refresh_devices: the warning names the device and says the output is now None");
    check(c.has_song() && c.view().rate_choice == RateModelChoice::MidiBitrate && c.view().queue_value.value == 777, "refresh_devices: the options stay after the fallback");

    // None stays None when the list changes; a device that comes back is not selected on its own.
    present = { "Fake device A", "Fake device B" };
    c.refresh_devices();
    check(c.output_selection() == 0 && c.take_messages().empty(), "refresh_devices: None stays None without a message");
    c.select_output(1);

    // Playing: the list is left alone and the user is told to stop first.
    c.select_output(0);
    c.play_clicked();
    const std::string state = c.view().state_text;
    if (state == "Playing" || state == "Paused") {
        const int before = listed.load();
        present = { "Another" };
        c.refresh_devices();
        check(listed.load() == before && c.view().output_items.size() == 3, "refresh_devices: no change while playing");
        messages = c.take_messages();
        check(messages.size() == 1 && messages[0].kind == UiMessage::Information, "refresh_devices: while playing, an information message");
        c.stop_clicked();
    }
    present = { "Another" };
    c.stop_clicked();
    c.refresh_devices();
    check(c.view().output_items.size() == 2 && c.view().output_items[1] == "Another" && c.output_selection() == 0, "refresh_devices: works again after Stop");
    c.take_messages();

    // A deferred controller lists on the first refresh_devices() and does not list twice.
    {
        std::atomic<int> n{ 0 };
        PlayerEnvironment env2;
        env2.services = std::make_shared<win32::Win32EngineServices>();
        env2.files = &files;
        env2.list_devices = [&n] { ++n; std::vector<platform::OutputDeviceInfo> d(1); d[0].device_id = 0; d[0].name = "Only"; return d; };
        env2.create_winmm = [winmm] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env2.pointer_size = static_cast<int>(sizeof(void*));
        env2.defer_device_listing = true;
        PlayerController d(env2);
        d.refresh_devices();
        check(n.load() == 1 && d.view().output_items.size() == 2 && d.output_selection() == 1, "refresh_devices: a deferred controller lists once, with the default selection");
    }
}

} // namespace

// R1: whether the Direct WinMM box is greyed out is decided when the output selection changes (no Play needed): the MIDI
// Mapper at once without any driver call, a real device by the environment's check on a worker thread (cached per
// device for the session; the box stays enabled until the answer arrives), None and KDMAPI as before.
void test_direct_greying_on_selection()
{
    std::atomic<int> probes{ 0 };
    std::atomic<int> mapperProbes{ 0 };
    std::atomic<bool> hold{ false };
    auto probe = [&](uint32_t id) {
        ++probes;
        if (id == 0xFFFFFFFFu) ++mapperProbes;
        while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        DirectProbeResult r;
        if (id == 0) { r.usable = false; r.reason = "it is probably served by Windows' own MIDI driver"; }   // stand-in: Windows' own driver
        else r.usable = true;                                                                                // stand-in: a usable driver
        return r;
    };
    Fixture f(probe, true);
    PlayerController& c = *f.controller;
    auto settle = [&](int expectedProbes) {
        for (int i = 0; i < 1000 && probes.load() < expectedProbes; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (int i = 0; i < 20; ++i) { c.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    };
    // the initial selection (device 0, "Windows' own driver") is checked at start
    f.v();
    settle(1);
    check(f.v().output_selected == 1 && probes.load() == 1, "greying: the initial selection is checked at start");
    check(!f.v().direct_enabled && !f.v().direct_checked && f.v().direct_tooltip.find("cannot be used with Fake device A") != std::string::npos &&
        f.v().direct_tooltip.find("Windows' own MIDI driver") != std::string::npos, "greying: Windows' own driver: greyed out before any Play, with the reason (" + f.v().direct_tooltip + ")");
    c.toggle_direct_winmm();
    check(f.v().direct_checked && !f.v().direct_enabled && f.v().direct_tooltip.find("The notes play through Windows as usual") != std::string::npos, "greying: ticked and greyed keeps its tick and says what plays");
    c.toggle_direct_winmm();
    // a usable driver: enabled; the check is asked once per device
    hold = true;
    c.select_output(2);
    check(f.v().direct_enabled, "greying: while the check is running the box stays enabled");
    hold = false;
    settle(2);
    check(f.v().direct_enabled && f.v().direct_tooltip.find("Direct mode is off") != std::string::npos, "greying: a usable driver: enabled");
    c.select_output(1);
    c.select_output(2);
    settle(2);
    check(probes.load() == 2 && f.v().direct_enabled, "greying: the answer is cached per device for the session (no second check)");
    c.select_output(1);
    check(!f.v().direct_enabled, "greying: back to Windows' own driver: greyed out again from the cache");
    // the MIDI Mapper: at once, no driver call
    c.select_output(3);
    check(!f.v().direct_enabled && f.v().direct_tooltip.find("MIDI Mapper") != std::string::npos && mapperProbes.load() == 0, "greying: MIDI Mapper: greyed out with no driver call");
    // None and KDMAPI
    c.select_output(0);
    check(!f.v().direct_enabled && f.v().direct_tooltip.find("output is None") != std::string::npos, "greying: None: greyed out");
    c.select_output(2);
    c.set_kdmapi(true);
    c.toggle_direct_winmm();
    check(!f.v().direct_enabled && f.v().direct_checked && f.v().direct_tooltip.find("KDMAPI is ticked") != std::string::npos, "greying: KDMAPI ticked: greyed out, tick kept");
    c.set_kdmapi(false);
    check(f.v().direct_enabled && f.v().direct_checked, "greying: KDMAPI unticked: enabled again with the tick");
}

// U5: every failure message starts with one plain sentence; technical detail comes below it, under "Details:".
void test_error_text_layout()
{
    // the layout and the way back
    check(join_error_text("Plain.", "") == "Plain.", "layout: no details, only the plain sentence");
    check(join_error_text("Plain.", "Code 5") == "Plain.\n\nDetails:\nCode 5", "layout: plain sentence, blank line, Details:, the detail");
    std::string plain, details;
    split_error_text("Plain.\n\nDetails:\nCode 5\nmore", plain, details);
    check(plain == "Plain." && details == "Code 5\nmore", "layout: a frontend can split the two parts again");
    split_error_text("Only plain.", plain, details);
    check(plain == "Only plain." && details.empty(), "layout: a text without details splits into the plain part only");

    // texts of the MIDI outputs (word for word in the details)
    ErrorText e = explain_error("MIDI error while opening MIDI output Fake (device 0, WinMM) (native code 4): The specified device is already in use.", "Playback could not be started.");
    check(e.plain == "The MIDI output could not be opened." && e.details.find("native code 4") != std::string::npos && e.details.find("already in use") != std::string::npos,
        "output error: the opening failure is a plain sentence, code and driver text below");
    e = explain_error("MIDI error while sending a short MIDI message through Fake (native code 67): Text from the driver.", "x");
    check(e.plain == "A message could not be sent to the MIDI output." && e.details.find("Text from the driver.") != std::string::npos, "output error: sending");
    e = explain_error("MIDI System Exclusive failure during midiOutLongMsg (native code 65)\r\nSource: a.mid; event #3", "x");
    check(e.plain.find("System Exclusive") != std::string::npos && e.details.find("event #3") != std::string::npos, "output error: SysEx failure keeps its whole report as details");
    e = explain_error("The KDMAPI provider is not a valid Windows PE image: C:\\Tools\\OmniMIDI.dll", "x");
    check(e.plain == "The KDMAPI provider is not a valid Windows PE image." && e.details == "File: C:\\Tools\\OmniMIDI.dll", "output error: a path moves below the sentence");
    e = explain_error("No KDMAPI provider was found. Place a matching OmniMIDI.dll beside the application, install OmniMIDI, or turn off KDMAPI.", "x");
    check(e.plain.find("Place a matching OmniMIDI.dll") != std::string::npos && e.details.empty(), "output error: an already plain text stays as it is, no details");
    e = explain_error("A MIDI message could not be sent through the ALSA sequencer. (Broken pipe)", "x");
    check(e.plain == "A MIDI message could not be sent through the ALSA sequencer." && e.details == "Broken pipe", "output error (Linux): the system's own text goes below");
    e = explain_error("The ALSA sequencer could not be opened (Invalid argument).", "x");
    check(e.plain.empty() == false && e.plain == "The ALSA sequencer could not be opened." && e.details == "Invalid argument", "output error (Linux): sequencer open failure");
    e = explain_error("something with native code 5 in it", "Fallback sentence.");
    check(e.plain == "Fallback sentence." && e.details == "something with native code 5 in it", "output error: other technical text gets the fallback sentence, nothing is lost");

    // engine error with a cause
    engine::EngineError engineError = engine::EngineError::make("InvalidOperationException", "The scheduler stopped, but MIDI reset/all-notes-off failed while stopping playback.");
    engineError.inner_message = "MIDI error while sending a short MIDI message through Fake (native code 5): Access denied.";
    e = explain_engine_error(engineError, "Playback could not be stopped.");
    check(e.plain.find("The scheduler stopped") == 0 && e.details.find("Cause: MIDI error while sending") != std::string::npos, "engine error: its own sentence first, the cause below");

    // loader errors
    e = explain_load_error("IOException", "Could not open C:\\Songs\\a.mid (Windows error 2).");
    check(e.plain == "The MIDI file could not be opened." && e.details == "Could not open C:\\Songs\\a.mid (Windows error 2).", "load error: the file cannot be opened");
    e = explain_load_error("InvalidDataException", "Invalid running status in track 3.");
    check(e.plain.find("could not be read as a MIDI file") != std::string::npos && e.details == "Invalid running status in track 3.", "load error: damaged data, the position goes below");
    e = explain_load_error("InvalidDataException", "SMPTE time division is not supported; this version requires PPQN timing.");
    check(e.plain.find("SMPTE time division is not supported") == 0 && e.details.empty(), "load error: an unsupported feature is its own plain explanation");
    e = explain_load_error("InvalidDataException", "Unsupported system status 0xF5.");
    check(e.plain.find("could not be read as a MIDI file") != std::string::npos && e.details == "Unsupported system status 0xF5.", "load error: an unknown status byte means a damaged file, the byte goes below");
    e = explain_load_error("InvalidDataException", "Unsupported Standard MIDI File format 3.");
    check(e.plain == "Unsupported Standard MIDI File format 3." && e.details.empty(), "load error: an unsupported file format is its own plain explanation");
    e = explain_load_error("OutOfMemoryException", "There is not enough memory to complete this operation. Close other programs and try again.");
    check(e.plain.find("not enough memory") != std::string::npos && e.details.empty(), "load error: out of memory stays plain");

    // end to end: a file that is not there
    Fixture f;
    f.controller->open_file("Z:\\no such folder\\missing.mid");
    f.wait_loaded(10000);
    std::vector<UiMessage> messages = f.controller->take_messages();
    check(!messages.empty() && messages[0].title == "Unable to load MIDI file" && messages[0].text.find("The MIDI file could not be opened.\n\nDetails:\nCould not open Z:") == 0,
        "controller: a missing file gives the plain sentence, then Details: with the Windows error");
    // the plain messages of the controller are unchanged
    f.controller->play_clicked();
    messages = f.controller->take_messages();
    check(messages.size() == 1 && messages[0].text == "Open a MIDI file first.", "controller: a plain message has no details part");
}

int main(int argc, char** argv)
{
    std::string fixtures = argc > 1 ? argv[1] : "v2\\harness\\fixtures";
    test_initial_state();
    test_rate_models();
    test_queue_settings();
    test_forward_queue_precondition(fixtures);
    test_load_and_transport(fixtures);
    test_kdmapi_uses_recording_output(fixtures);
    test_direct_winmm(fixtures);
    test_direct_greying_on_selection();
    test_error_text_layout();
    test_play_waits_for_stuck_output(fixtures);
    test_status_when_driver_stopped_answering(fixtures);
    test_speed_window_session_only();
    test_remembered_settings();
    test_deferred_device_listing(fixtures);
    test_refresh_devices(fixtures);
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
