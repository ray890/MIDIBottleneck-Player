// Remembered settings (prompt 013, Step 4): the few options the player keeps between runs, their plain-text file format,
// and a small store that loads them once and saves them atomically when one changes. Portable: the file operations come
// from a SettingsIo the frontend supplies (Win32 and GTK each have one; tests use a fake).
//
// Not remembered, by design: window position and size, the loaded file, the playback / bottleneck values on the main
// window (queue limit, overflow, rate model, processing values), and everything in Analysis, Channels and Track routing.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace midibn {
namespace app {

constexpr int kSettingsFormatVersion = 1;

struct RememberedSettings {
    // Output: the MIDI device by name ("None" for no output). Empty = nothing chosen yet: the normal default.
    std::string output_name;
    bool kdmapi = false;
    bool direct_winmm = false;
    // System menu options (the controller's part)
    bool chase_midi_state = true;
    bool preserve_queue = false;
    bool output_paced = false;
    bool apply_queue_limit = false;
    bool queue_age_limit = false;
    bool scale_note_interval = false;
    int64_t speed_window_us = 250000;   // Effective speed window (0 = instantaneous, else 25 ms .. 10 s)
    // System menu options (the frontend's part)
    bool always_on_top = false;
    bool borderless = false;
    int borderless_style = 0;           // 0 classic strip, 1 integrated header, 2 auto-hide overlay
    bool show_processing = true;
    bool show_statistics = true;
    int scale_percent = 100;            // 50, 75, 100, 125, 150, 200
    // "Let other programs run while playback is behind": -1 not chosen yet (the automatic default applies), 0 off, 1 on
    int few_cpu_yield = -1;

    bool operator==(const RememberedSettings& o) const;
    bool operator!=(const RememberedSettings& o) const { return !(*this == o); }
};

enum class SettingsLoadState {
    Missing,      // no file: all defaults, nothing to say
    Ok,           // read; every key was understood or unknown (unknown keys are ignored)
    Damaged,      // not our format, or some values were wrong: defaults were used for those keys
    Unreadable,   // the file exists but could not be read
    Newer         // written by a newer version: known keys used, the file is never overwritten
};

struct SettingsLoad {
    RememberedSettings settings;
    SettingsLoadState state = SettingsLoadState::Missing;
    int bad_items = 0;   // wrong or unreadable lines/values
};

// Parses the bytes of a settings file. Never fails: whatever cannot be understood falls back to the default of that key.
SettingsLoad parse_settings(const std::string& bytes);
// UTF-8 text: a comment line, "format=1", then one "key=value" per line.
std::string serialize_settings(const RememberedSettings& settings);

// The few-processor option: on by default only when exactly one logical processor is usable by the process; a
// Hyper-Threaded single core (two usable) stays off. A stored explicit choice (0 or 1) wins over the automatic default.
bool few_cpu_default_on(uint32_t usableLogicalProcessors);
bool few_cpu_effective(int stored, uint32_t usableLogicalProcessors);

// What the user is told, in plain language, once, without a box. `detail` is shown below "Details:" in the tooltip.
struct SettingsNote {
    std::string text;
    std::string detail;
    bool empty() const { return text.empty(); }
};

// The file operations. read() returns Missing when there is no file, Failed when it exists but cannot be read.
class SettingsIo {
public:
    enum class ReadResult { Ok, Missing, Failed };
    virtual ~SettingsIo() {}
    virtual ReadResult read(std::string& bytes, std::string& detail) = 0;
    // Writes the whole file so that a crash or a full disk leaves the old file intact (temp file, then replace).
    virtual bool write_atomic(const std::string& bytes, std::string& detail) = 0;
};

class SettingsStore {
public:
    explicit SettingsStore(std::unique_ptr<SettingsIo> io) : io_(std::move(io)) {}
    // Reads the file once. The loaded values count as "already saved": nothing is written until a setting changes.
    SettingsLoad load();
    // Writes `current` when it differs from what was loaded or last saved. A failed write is reported once through note().
    // Returns true when a write happened and succeeded.
    bool save_if_changed(const RememberedSettings& current);
    // The note to show (empty when there is none). Set by load() and by the first failed write.
    const SettingsNote& note() const { return note_; }
    bool enabled() const { return io_ != nullptr; }
    bool read_only() const { return read_only_; }
    int write_count() const { return writes_; }

private:
    std::unique_ptr<SettingsIo> io_;
    RememberedSettings saved_;
    bool read_only_ = false;
    bool write_failed_ = false;
    int writes_ = 0;
    SettingsNote note_;
};

} // namespace app
} // namespace midibn
