// Remembered settings (prompt 013, Step 4): file format, damaged-file handling, store behaviour, the few-processor default.
// Portable; on Windows it also exercises the real file operations (atomic write, replace, a failing write) in a temp folder.
#include "midibn/app/settings_store.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include "settings_file.h"
#include <windows.h>
#endif

using namespace midibn::app;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

RememberedSettings sample()
{
    RememberedSettings s;
    s.output_name = "Microsoft GS Wavetable Synth \xC3\xA4";   // UTF-8 name with a non-ASCII letter
    s.kdmapi = true;
    s.direct_winmm = true;
    s.chase_midi_state = false;
    s.preserve_queue = true;
    s.output_paced = true;
    s.apply_queue_limit = false;
    s.queue_age_limit = true;
    s.scale_note_interval = true;
    s.speed_window_us = 1000000;
    s.always_on_top = true;
    s.borderless = true;
    s.borderless_style = 2;
    s.show_processing = false;
    s.show_statistics = false;
    s.scale_percent = 150;
    s.few_cpu_yield = 0;
    return s;
}

class FakeIo : public SettingsIo {
public:
    ReadResult mode = ReadResult::Missing;
    std::string content;
    bool failWrite = false;
    int writes = 0;
    ReadResult read(std::string& bytes, std::string& detail) override
    {
        if (mode == ReadResult::Failed) detail = "Windows error 5 while opening x";
        bytes = mode == ReadResult::Ok ? content : std::string();
        return mode;
    }
    bool write_atomic(const std::string& bytes, std::string& detail) override
    {
        if (failWrite) { detail = "Windows error 112 while writing x"; return false; }   // the old content stays
        content = bytes;
        mode = ReadResult::Ok;
        ++writes;
        return true;
    }
};

// A FakeIo the test keeps a pointer to while the store owns it.
struct Harness {
    FakeIo* io;
    SettingsStore store;
    Harness(FakeIo* f) : io(f), store(std::unique_ptr<SettingsIo>(f)) {}
};

void test_defaults_and_round_trip()
{
    RememberedSettings d;
    check(d.output_name.empty() && !d.kdmapi && !d.direct_winmm && d.chase_midi_state && !d.preserve_queue && !d.output_paced &&
        !d.apply_queue_limit && !d.queue_age_limit && !d.scale_note_interval && d.speed_window_us == 250000 && !d.always_on_top &&
        !d.borderless && d.borderless_style == 0 && d.show_processing && d.show_statistics && d.scale_percent == 100 && d.few_cpu_yield == -1,
        "defaults are the program's start values");
    SettingsLoad l = parse_settings(serialize_settings(d));
    check(l.state == SettingsLoadState::Ok && l.bad_items == 0 && l.settings == d, "default values round-trip");
    RememberedSettings s = sample();
    l = parse_settings(serialize_settings(s));
    check(l.state == SettingsLoadState::Ok && l.bad_items == 0, "a full file reads without problems");
    check(l.settings == s, "every remembered value round-trips");
    RememberedSettings t = s;
    t.few_cpu_yield = 1;
    check(parse_settings(serialize_settings(t)).settings.few_cpu_yield == 1, "few-processor on round-trips");
    t.few_cpu_yield = -1;
    check(parse_settings(serialize_settings(t)).settings.few_cpu_yield == -1, "few-processor 'not chosen' round-trips");
}

void test_parse_cases()
{
    SettingsLoad l = parse_settings("");
    check(l.state == SettingsLoadState::Ok && l.settings == RememberedSettings(), "empty file: all defaults, no complaint");
    l = parse_settings("  \r\n\r\n");
    check(l.state == SettingsLoadState::Ok, "a file of blank lines counts as empty");
    l = parse_settings(std::string("\xEF\xBB\xBF") + "format=1\nkdmapi=1\n");
    check(l.state == SettingsLoadState::Ok && l.settings.kdmapi, "a byte order mark is accepted");
    l = parse_settings("format=1\r\nkdmapi=1\r\nscale_percent=75\r\n");
    check(l.state == SettingsLoadState::Ok && l.settings.kdmapi && l.settings.scale_percent == 75, "Windows line endings");
    l = parse_settings("format=1\rkdmapi=1\r");
    check(l.settings.kdmapi, "old Mac line endings");

    std::string garbage;
    for (int i = 0; i < 300; ++i) garbage += static_cast<char>((i * 37 + 11) & 0xFF);
    l = parse_settings(garbage);
    check(l.state == SettingsLoadState::Damaged && l.settings == RememberedSettings(), "garbage bytes: damaged, defaults");
    l = parse_settings(std::string("format=1\nkdmapi=1\n") + std::string(1, '\0') + "x");
    check(l.state == SettingsLoadState::Damaged && l.settings == RememberedSettings(), "a zero byte: damaged, defaults (not half-trusted)");
    l = parse_settings(std::string(100000, 'a'));
    check(l.state == SettingsLoadState::Damaged, "a huge file is damaged");

    l = parse_settings("kdmapi=1\nscale_percent=75\n");
    check(l.state == SettingsLoadState::Damaged, "no format line: not our file");
    l = parse_settings("format=1\nkdmapi=1\nscale_percent=7");
    check(l.state == SettingsLoadState::Damaged && l.settings.kdmapi && l.settings.scale_percent == 100, "truncated file: good keys kept, the cut one falls back to its default");
    l = parse_settings("format=1\nkdmapi=1\nscale_perc");
    check(l.state == SettingsLoadState::Damaged && l.settings.kdmapi, "truncated inside a key name: the rest is kept");
    l = parse_settings("format=0\nkdmapi=1\n");
    check(l.state == SettingsLoadState::Ok && l.settings.kdmapi, "an old format version is read");
    l = parse_settings("format=7\nkdmapi=1\nfuture_key=3\n");
    check(l.state == SettingsLoadState::Newer && l.settings.kdmapi, "a newer format: known keys read, state Newer");
    l = parse_settings("format=1\nsome_future_option=yes\nkdmapi=1\n");
    check(l.state == SettingsLoadState::Ok && l.settings.kdmapi, "unknown keys are ignored without complaint");
    l = parse_settings("# a comment\n; another\nformat=1\n\nkdmapi=1\n");
    check(l.state == SettingsLoadState::Ok && l.settings.kdmapi, "comments and blank lines");

    l = parse_settings("format=1\nscale_percent=130\nkdmapi=1\nborderless_style=9\nspeed_window_us=5\nchase_midi_state=maybe\n");
    check(l.state == SettingsLoadState::Damaged && l.bad_items == 4, "four wrong values counted");
    check(l.settings.scale_percent == 100 && l.settings.borderless_style == 0 && l.settings.speed_window_us == 250000 && l.settings.chase_midi_state &&
        l.settings.kdmapi, "wrong values fall back per key; the good key is kept");
    l = parse_settings("format=1\nspeed_window_us=0\n");
    check(l.settings.speed_window_us == 0 && l.state == SettingsLoadState::Ok, "instantaneous speed window is valid");
    l = parse_settings("format=1\nspeed_window_us=99999999\n");
    check(l.settings.speed_window_us == 250000 && l.state == SettingsLoadState::Damaged, "speed window over 10 s is rejected");
    l = parse_settings("format=1\nkdmapi = 1 \n  scale_percent =200\n");
    check(l.settings.kdmapi && l.settings.scale_percent == 200, "spaces around keys and values");
    l = parse_settings("format=1\nfew_cpu_yield=auto\n");
    check(l.settings.few_cpu_yield == -1 && l.state == SettingsLoadState::Ok, "few-processor 'auto'");
    l = parse_settings("format=1\nfew_cpu_yield=2\n");
    check(l.settings.few_cpu_yield == -1 && l.state == SettingsLoadState::Damaged, "few-processor wrong value: not chosen");
    l = parse_settings("format=1\napply_queue_limit=1\nqueue_age_limit=1\n");
    check(l.settings.queue_age_limit && !l.settings.apply_queue_limit, "the two exclusive queue options are never both on");
    l = parse_settings("format=1\noutput=\n");
    check(l.settings.output_name.empty() && l.state == SettingsLoadState::Damaged, "an empty output name is a wrong value");
    l = parse_settings("format=1\nformat=zzz\n");
    check(l.state == SettingsLoadState::Damaged && l.bad_items == 1, "a bad second format line only counts as one wrong value");
}

void test_few_cpu_default_rule()
{
    // Simulated counts of usable logical processors (process affinity at start).
    check(few_cpu_default_on(1), "1 usable logical processor: on by default");
    check(!few_cpu_default_on(2), "Hyper-Threaded single core (2 logical): off by default");
    check(!few_cpu_default_on(4), "4 logical: off by default");
    check(!few_cpu_default_on(0), "0 (unknown): off by default");
    check(few_cpu_effective(-1, 1) && !few_cpu_effective(-1, 2), "not chosen: the automatic default applies");
    check(!few_cpu_effective(0, 1), "the user's 'off' wins over the automatic 'on'");
    check(few_cpu_effective(1, 2) && few_cpu_effective(1, 8), "the user's 'on' wins over the automatic 'off'");
}

void test_store()
{
    {   // no file: nothing said, nothing written at start
        Harness h(new FakeIo());
        SettingsLoad l = h.store.load();
        check(l.state == SettingsLoadState::Missing && h.store.note().empty(), "missing file: defaults, no note");
        check(!h.store.save_if_changed(l.settings) && h.io->writes == 0, "nothing is written while nothing changed");
        RememberedSettings s = l.settings;
        s.kdmapi = true;
        check(h.store.save_if_changed(s) && h.io->writes == 1, "a change writes once");
        check(!h.store.save_if_changed(s) && h.io->writes == 1, "the same state is not written again");
        s.scale_percent = 50;
        check(h.store.save_if_changed(s) && h.io->writes == 2, "the next change writes again");
        check(parse_settings(h.io->content).settings == s, "the written file reads back to the same settings");
    }
    {   // damaged file: a note, defaults, not rewritten at start, rewritten on a change
        FakeIo* f = new FakeIo();
        f->mode = SettingsIo::ReadResult::Ok;
        f->content = "format=1\nkdmapi=1\nscale_percent=oops\n";
        Harness h(f);
        SettingsLoad l = h.store.load();
        check(l.state == SettingsLoadState::Damaged && l.settings.kdmapi && l.settings.scale_percent == 100, "damaged file: good keys kept");
        check(!h.store.note().empty() && h.store.note().detail.find("1 entry") != std::string::npos, "damaged file: one note with detail");
        check(!h.store.save_if_changed(l.settings) && f->writes == 0, "damaged file is not rewritten at start");
        RememberedSettings s = l.settings;
        s.preserve_queue = true;
        check(h.store.save_if_changed(s), "damaged file is repaired by the next change");
        check(parse_settings(f->content).state == SettingsLoadState::Ok, "the repaired file is clean");
    }
    {   // garbage
        FakeIo* f = new FakeIo();
        f->mode = SettingsIo::ReadResult::Ok;
        f->content = std::string("\x01\x02\xFF\xFE garbage \x00\x00", 16);
        Harness h(f);
        SettingsLoad l = h.store.load();
        check(l.state == SettingsLoadState::Damaged && l.settings == RememberedSettings() && !h.store.note().empty(), "garbage file: defaults and a note");
    }
    {   // unreadable
        FakeIo* f = new FakeIo();
        f->mode = SettingsIo::ReadResult::Failed;
        Harness h(f);
        SettingsLoad l = h.store.load();
        check(l.state == SettingsLoadState::Unreadable && l.settings == RememberedSettings(), "unreadable file: defaults");
        check(!h.store.note().empty() && h.store.note().detail.find("Windows error 5") != std::string::npos, "unreadable file: note with the technical detail");
    }
    {   // newer file: never overwritten
        FakeIo* f = new FakeIo();
        f->mode = SettingsIo::ReadResult::Ok;
        f->content = "format=2\nkdmapi=1\nfuture=1\n";
        Harness h(f);
        SettingsLoad l = h.store.load();
        check(l.state == SettingsLoadState::Newer && l.settings.kdmapi && h.store.read_only() && !h.store.note().empty(), "newer file: read, note, read-only");
        RememberedSettings s = l.settings;
        s.scale_percent = 200;
        check(!h.store.save_if_changed(s) && f->writes == 0 && f->content.find("future=1") != std::string::npos, "newer file is left exactly as it was");
    }
    {   // failing writes (full disk, read-only folder): noted once, no retry storm, the old file survives
        FakeIo* f = new FakeIo();
        f->mode = SettingsIo::ReadResult::Ok;
        f->content = serialize_settings(RememberedSettings());
        const std::string before = f->content;
        Harness h(f);
        SettingsLoad l = h.store.load();
        check(h.store.note().empty(), "clean file: no note");
        f->failWrite = true;
        RememberedSettings s = l.settings;
        s.kdmapi = true;
        check(!h.store.save_if_changed(s), "failed write reports false");
        check(!h.store.note().empty() && h.store.note().detail.find("Windows error 112") != std::string::npos, "failed write: noted with detail");
        const std::string first = h.store.note().text;
        s.direct_winmm = true;
        h.store.save_if_changed(s);
        check(h.store.note().text == first, "failed write is noted once");
        check(f->content == before, "failed write leaves the old file intact");
        f->failWrite = false;
        s.scale_percent = 75;
        check(h.store.save_if_changed(s), "a later write succeeds when the problem is gone");
    }
    {   // a store without a file (--no-settings is "no store" in the frontend; an empty store never writes)
        SettingsStore none((std::unique_ptr<SettingsIo>()));
        check(!none.enabled() && !none.save_if_changed(sample()) && none.load().state == SettingsLoadState::Missing, "no io: nothing read, nothing written");
    }
}

#ifdef _WIN32
std::string slurp(const std::wstring& path)
{
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

void test_windows_file_io()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring folder = std::wstring(temp) + L"midibn-settings-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring file = folder + L"\\settings.ini";
    {
        auto io = midibn::win32::make_settings_io(file);
        std::string bytes, detail;
        check(io->read(bytes, detail) == SettingsIo::ReadResult::Missing, "real io: a missing file reads as Missing");
        check(io->write_atomic("format=1\nkdmapi=1\n", detail), "real io: first write");
        check(io->read(bytes, detail) == SettingsIo::ReadResult::Ok && bytes == "format=1\nkdmapi=1\n", "real io: reads back");
        check(io->write_atomic("format=1\nkdmapi=0\n", detail) && slurp(file) == "format=1\nkdmapi=0\n", "real io: replaces an existing file");
        check(GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "real io: no temp file is left behind");
    }
    {   // a folder that cannot be created: the write fails with a detail and nothing else happens
        auto io = midibn::win32::make_settings_io(folder + L"\\no\\such\\folder\\settings.ini");
        std::string detail;
        check(!io->write_atomic("format=1\n", detail) && detail.find("Windows error") != std::string::npos, "real io: an impossible folder fails with a detail");
    }
    {   // the target is held open without sharing: the replace fails and the old content survives
        HANDLE hold = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        auto io = midibn::win32::make_settings_io(file);
        std::string detail;
        const bool ok = io->write_atomic("format=1\nkdmapi=1\n", detail);
        if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
        check(!ok, "real io: a locked target makes the write fail");
        check(slurp(file) == "format=1\nkdmapi=0\n", "real io: the old file survives a failed replace");
        check(GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "real io: a failed replace removes its temp file");
    }
    {   // a directory in the place of the file: unreadable, reported (not a crash)
        const std::wstring dirAsFile = folder + L"\\adir";
        CreateDirectoryW(dirAsFile.c_str(), nullptr);
        auto io = midibn::win32::make_settings_io(dirAsFile);
        std::string bytes, detail;
        check(io->read(bytes, detail) == SettingsIo::ReadResult::Failed && !detail.empty(), "real io: a folder where the file should be is Failed with a detail");
        RemoveDirectoryW(dirAsFile.c_str());
    }
    DeleteFileW(file.c_str());
    RemoveDirectoryW(folder.c_str());
}
#endif

} // namespace

int main()
{
    test_defaults_and_round_trip();
    test_parse_cases();
    test_few_cpu_default_rule();
    test_store();
#ifdef _WIN32
    test_windows_file_io();
#endif
    std::printf("settings tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
