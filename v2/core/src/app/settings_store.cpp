#include "midibn/app/settings_store.h"

#include <cstdlib>

namespace midibn {
namespace app {
namespace {

const size_t kMaximumFileBytes = 64 * 1024;

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool parse_bool(const std::string& v, bool& out)
{
    if (v == "1") { out = true; return true; }
    if (v == "0") { out = false; return true; }
    return false;
}

bool parse_int(const std::string& v, long long minimum, long long maximum, long long& out)
{
    if (v.empty() || v.size() > 12) return false;
    size_t i = (v[0] == '-') ? 1 : 0;
    if (i == v.size()) return false;
    for (size_t k = i; k < v.size(); ++k)
        if (v[k] < '0' || v[k] > '9') return false;
    long long n = std::atoll(v.c_str());
    if (n < minimum || n > maximum) return false;
    out = n;
    return true;
}

bool valid_scale(long long p) { return p == 50 || p == 75 || p == 100 || p == 125 || p == 150 || p == 200; }

// A name is text without control characters (a line break would break the file).
bool valid_name(const std::string& v)
{
    if (v.empty() || v.size() > 512) return false;
    for (size_t i = 0; i < v.size(); ++i)
        if (static_cast<unsigned char>(v[i]) < 0x20) return false;
    return true;
}

} // namespace

bool RememberedSettings::operator==(const RememberedSettings& o) const
{
    return output_name == o.output_name && kdmapi == o.kdmapi && direct_winmm == o.direct_winmm &&
        chase_midi_state == o.chase_midi_state && preserve_queue == o.preserve_queue && output_paced == o.output_paced &&
        apply_queue_limit == o.apply_queue_limit && queue_age_limit == o.queue_age_limit &&
        scale_note_interval == o.scale_note_interval && speed_window_us == o.speed_window_us &&
        always_on_top == o.always_on_top && borderless == o.borderless && borderless_style == o.borderless_style &&
        show_processing == o.show_processing && show_statistics == o.show_statistics && scale_percent == o.scale_percent &&
        few_cpu_yield == o.few_cpu_yield;
}

SettingsLoad parse_settings(const std::string& bytes)
{
    SettingsLoad result;
    RememberedSettings& s = result.settings;
    std::string text = bytes;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a UTF-8 byte order mark from an editor
    {
        bool blank = true;
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') { blank = false; break; }
        if (blank) { result.state = SettingsLoadState::Ok; return result; }   // empty file: all defaults (also how portable mode is switched on)
    }
    if (text.size() > kMaximumFileBytes || text.find('\0') != std::string::npos) {
        result.state = SettingsLoadState::Damaged;
        result.bad_items = 1;
        return result;
    }
    int version = -1;
    int bad = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of("\r\n", pos);   // \n, \r\n or a lone \r
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) { ++bad; continue; }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        bool b = false;
        long long n = 0;
        if (key == "format") {
            if (parse_int(value, 0, 1000000, n)) version = static_cast<int>(n);
            else ++bad;
        }
        else if (key == "output") { if (valid_name(value)) s.output_name = value; else ++bad; }
        else if (key == "kdmapi") { if (parse_bool(value, b)) s.kdmapi = b; else ++bad; }
        else if (key == "direct_winmm") { if (parse_bool(value, b)) s.direct_winmm = b; else ++bad; }
        else if (key == "chase_midi_state") { if (parse_bool(value, b)) s.chase_midi_state = b; else ++bad; }
        else if (key == "preserve_queue") { if (parse_bool(value, b)) s.preserve_queue = b; else ++bad; }
        else if (key == "output_paced") { if (parse_bool(value, b)) s.output_paced = b; else ++bad; }
        else if (key == "apply_queue_limit") { if (parse_bool(value, b)) s.apply_queue_limit = b; else ++bad; }
        else if (key == "queue_age_limit") { if (parse_bool(value, b)) s.queue_age_limit = b; else ++bad; }
        else if (key == "scale_note_interval") { if (parse_bool(value, b)) s.scale_note_interval = b; else ++bad; }
        else if (key == "speed_window_us") {
            if (parse_int(value, 0, 10000000, n) && (n == 0 || n >= 25000)) s.speed_window_us = n; else ++bad;
        }
        else if (key == "always_on_top") { if (parse_bool(value, b)) s.always_on_top = b; else ++bad; }
        else if (key == "borderless") { if (parse_bool(value, b)) s.borderless = b; else ++bad; }
        else if (key == "borderless_style") { if (parse_int(value, 0, 2, n)) s.borderless_style = static_cast<int>(n); else ++bad; }
        else if (key == "show_processing") { if (parse_bool(value, b)) s.show_processing = b; else ++bad; }
        else if (key == "show_statistics") { if (parse_bool(value, b)) s.show_statistics = b; else ++bad; }
        else if (key == "scale_percent") { if (parse_int(value, 1, 1000, n) && valid_scale(n)) s.scale_percent = static_cast<int>(n); else ++bad; }
        else if (key == "few_cpu_yield") {
            if (value == "auto") s.few_cpu_yield = -1;
            else if (parse_bool(value, b)) s.few_cpu_yield = b ? 1 : 0;
            else ++bad;
        }
        // any other key: ignored (a newer or older version may have written it)
    }
    if (s.apply_queue_limit && s.queue_age_limit) s.apply_queue_limit = false;   // the two exclude each other
    result.bad_items = bad;
    if (version < 0) { result.state = SettingsLoadState::Damaged; if (bad == 0) result.bad_items = 1; }   // no version line: not our file
    else if (version > kSettingsFormatVersion) result.state = SettingsLoadState::Newer;
    else result.state = bad > 0 ? SettingsLoadState::Damaged : SettingsLoadState::Ok;
    return result;
}

std::string serialize_settings(const RememberedSettings& s)
{
    auto flag = [](bool v) { return v ? "1" : "0"; };
    std::string name;
    for (size_t i = 0; i < s.output_name.size(); ++i)
        if (static_cast<unsigned char>(s.output_name[i]) >= 0x20) name += s.output_name[i];
    std::string out;
    out += "# MIDIBottleneck Player settings. Safe to edit or delete; a missing or unreadable key uses its standard value.\n";
    out += "format=" + std::to_string(kSettingsFormatVersion) + "\n";
    if (!name.empty()) out += "output=" + name + "\n";
    out += std::string("kdmapi=") + flag(s.kdmapi) + "\n";
    out += std::string("direct_winmm=") + flag(s.direct_winmm) + "\n";
    out += std::string("chase_midi_state=") + flag(s.chase_midi_state) + "\n";
    out += std::string("preserve_queue=") + flag(s.preserve_queue) + "\n";
    out += std::string("output_paced=") + flag(s.output_paced) + "\n";
    out += std::string("apply_queue_limit=") + flag(s.apply_queue_limit) + "\n";
    out += std::string("queue_age_limit=") + flag(s.queue_age_limit) + "\n";
    out += std::string("scale_note_interval=") + flag(s.scale_note_interval) + "\n";
    out += "speed_window_us=" + std::to_string(s.speed_window_us) + "\n";
    out += std::string("always_on_top=") + flag(s.always_on_top) + "\n";
    out += std::string("borderless=") + flag(s.borderless) + "\n";
    out += "borderless_style=" + std::to_string(s.borderless_style) + "\n";
    out += std::string("show_processing=") + flag(s.show_processing) + "\n";
    out += std::string("show_statistics=") + flag(s.show_statistics) + "\n";
    out += "scale_percent=" + std::to_string(s.scale_percent) + "\n";
    out += s.few_cpu_yield < 0 ? "few_cpu_yield=auto\n" : s.few_cpu_yield ? "few_cpu_yield=1\n" : "few_cpu_yield=0\n";
    return out;
}

bool few_cpu_default_on(uint32_t usableLogicalProcessors) { return usableLogicalProcessors == 1; }

bool few_cpu_effective(int stored, uint32_t usableLogicalProcessors)
{
    if (stored == 0) return false;
    if (stored == 1) return true;
    return few_cpu_default_on(usableLogicalProcessors);
}

SettingsLoad SettingsStore::load()
{
    SettingsLoad result;
    if (!io_) return result;
    std::string bytes, detail;
    SettingsIo::ReadResult r = io_->read(bytes, detail);
    if (r == SettingsIo::ReadResult::Missing) {
        result.state = SettingsLoadState::Missing;
    }
    else if (r == SettingsIo::ReadResult::Failed) {
        result.state = SettingsLoadState::Unreadable;
        note_.text = "Your saved settings could not be read, so the standard settings are used.";
        note_.detail = detail.empty() ? std::string("The settings file could not be opened.") : detail;
    }
    else {
        result = parse_settings(bytes);
        if (result.state == SettingsLoadState::Damaged) {
            note_.text = "Your saved settings could not be fully read, so the standard settings are used for those options.";
            note_.detail = std::to_string(result.bad_items) + " entr" + (result.bad_items == 1 ? "y" : "ies") +
                " in the settings file could not be understood. The file is rewritten the next time you change an option.";
        }
        else if (result.state == SettingsLoadState::Newer) {
            note_.text = "Your saved settings were written by a newer version of this program and are left unchanged.";
            note_.detail = "Options this version knows are used. Changes made now are not saved, so the newer version keeps its file.";
            read_only_ = true;
        }
    }
    saved_ = result.settings;
    return result;
}

bool SettingsStore::save_if_changed(const RememberedSettings& current)
{
    if (!io_ || read_only_ || current == saved_) return false;
    std::string detail;
    if (io_->write_atomic(serialize_settings(current), detail)) {
        saved_ = current;
        ++writes_;
        return true;
    }
    saved_ = current;   // do not retry on every refresh of the same state; a later different change tries again
    if (!write_failed_) {
        write_failed_ = true;
        note_.text = "Your settings could not be saved, so they are kept only until the program closes.";
        note_.detail = detail.empty() ? std::string("The settings file could not be written.") : detail;
    }
    return false;
}

} // namespace app
} // namespace midibn
