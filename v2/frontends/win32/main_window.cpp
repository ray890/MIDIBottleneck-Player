#include "main_window.h"

#include "dialogs.h"
#include "display_pacer.h"
#include "thread_report.h"
#include "win32_timing.h"
#include "glue.h"
#include "settings_file.h"
#include "kdmapi_midi_output.h"
#include "winmm_direct_output.h"
#include "midibn/app/help_text.h"
#include "midibn/app/ui_mappings.h"
#include "winmm_midi_output.h"

#include <commctrl.h>
#include <objbase.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace midibn {
namespace ui {

namespace {

const wchar_t kMainClass[] = L"MIDIBottleneckPlayerV2";
const wchar_t kProductName[] = L"MIDIBottleneck Player";
const UINT_PTR kTimerRefresh = 1;
const UINT_PTR kTimerAutoClose = 2;
const UINT_PTR kTimerOverlay = 3;
const UINT_PTR kTimerScreenshot = 4;
const UINT_PTR kTimerTestDialog = 6;
const UINT_PTR kTimerPopupShot = 7;
const UINT_PTR kTimerThreadReport = 8;

// Test option --thread-report FILE: the player's own threads (priority, start module), written once.
void write_thread_report(const std::wstring& path)
{
    std::vector<midibn::win32::ThreadInfo> threads = midibn::win32::snapshot_threads();
    char head[256];
    std::snprintf(head, sizeof(head), "midibn-win32 thread report (player process)\nusable logical processors: %u\nfew-processor options: normal=%d yield-ms=%d\n",
        static_cast<unsigned>(midibn::win32::usable_logical_processors()), midibn::win32::few_cpu_normal_threads_active() ? 1 : 0,
        midibn::win32::few_cpu_yield_milliseconds_active());
    std::string text = head + midibn::win32::format_thread_table(threads, nullptr, 0);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

bool has_midi_extension(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = static_cast<wchar_t>(towlower(ext[i]));
    return ext == L".mid" || ext == L".midi" || ext == L".rmi";
}

// DD-099: the one-line file information ("... events  •  ... tracks  •  ... notes  •  time") takes the compact
// layout's two-line form (a line break after "tracks") when it is wider than its label of `width` pixels. At
// Application scale 50 % in the integrated header the right-aligned line was cut off at its left end.
std::string fit_file_info(const std::string& info, int width, const UiContext& ctx)
{
    if (info.empty() || info.find_first_of("\r\n") != std::string::npos || width <= 1) return info;
    if (ctx.text_width(W(info)) <= width) return info;
    const std::string separator = "  \xE2\x80\xA2  ";
    size_t at = info.find(" tracks" + separator);
    if (at == std::string::npos) return info;
    std::string out = info;
    out.replace(at + 7, separator.size(), "\r\n");
    return out;
}

int client_width(HWND window)
{
    RECT r = {};
    if (window) GetClientRect(window, &r);
    return r.right;
}

void set_text_cached(HWND window, const std::string& utf8)
{
    std::wstring wide = W(utf8);
    int length = GetWindowTextLengthW(window);
    if (length == static_cast<int>(wide.size())) {
        std::wstring current(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(window, &current[0], length + 1);
        current.resize(static_cast<size_t>(length));
        if (current == wide) return;
    }
    SetWindowTextW(window, wide.c_str());
}

bool own_visible(HWND window) { return (GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) != 0; }

void show(HWND window, bool visible)
{
    if (window != nullptr && own_visible(window) != visible) ShowWindow(window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void enable(HWND window, bool enabled)
{
    if (window != nullptr && (IsWindowEnabled(window) != FALSE) != enabled) EnableWindow(window, enabled ? TRUE : FALSE);
}

void set_check(HWND box, bool checked)
{
    if ((SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED) != checked)
        SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void fill_combo(HWND combo, const std::vector<std::string>& items, const std::vector<std::string>& previous, bool force)
{
    if (!force && items == previous) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < items.size(); ++i) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(items[i]).c_str()));
}

void set_combo_selection(HWND combo, int index)
{
    if (static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) != index) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

// Accessible names through IAccPropServices, resolved at run time (oleacc exists from Windows XP).
// DD-058: starting COM and loading the accessibility service takes tens of milliseconds, so the names are collected
// while the window is built and set by flush_accessible_names(), which the window runs from a posted message right
// after it is shown (an accessibility client asks for names only after the window exists, and every name is in place
// a few milliseconds later).
struct PendingName { HWND window; const wchar_t* name; };
std::vector<PendingName>& pending_names()
{
    static std::vector<PendingName> names;
    return names;
}

void set_accessible_name(HWND window, const wchar_t* name) { pending_names().push_back(PendingName{ window, name }); }

void flush_names_now()
{
    typedef HRESULT(WINAPI * SetNameFn)(void*, HWND, DWORD, DWORD, const GUID&, LPCWSTR);
    static const GUID clsidAccPropServices = { 0xb5f8350b, 0x0548, 0x48b1, { 0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7 } };
    static const GUID iidAccPropServices = { 0x6e26e776, 0x04f0, 0x495d, { 0x80, 0xe4, 0x33, 0x30, 0xb1, 0x39, 0x66, 0x55 } };
    static const GUID propName = { 0x608d3df8, 0x8128, 0x4aa7, { 0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91 } };
    static void* services = nullptr;
    static bool tried = GetEnvironmentVariableW(L"MIDIBN_NO_ACC", nullptr, 0) != 0;
    std::vector<PendingName> names;
    names.swap(pending_names());
    if (names.empty()) return;
    if (!tried) {
        tried = true;
        CoInitialize(nullptr);
        if (FAILED(CoCreateInstance(clsidAccPropServices, nullptr, CLSCTX_INPROC_SERVER, iidAccPropServices, &services))) services = nullptr;
    }
    if (!services) return;
    // IAccPropServices::SetHwndPropStr is vtable slot 7 (IUnknown 0-2, SetPropValue 3, SetPropServer 4,
    // ClearProps 5, SetHwndProp 6, SetHwndPropStr 7).
    void** vtable = *reinterpret_cast<void***>(services);
    SetNameFn setName = reinterpret_cast<SetNameFn>(vtable[7]);
    for (size_t i = 0; i < names.size(); ++i)
        if (IsWindow(names[i].window))
            setName(services, names[i].window, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), propName, names[i].name);
}

} // namespace

void flush_accessible_names() { flush_names_now(); }

MainWindow::MainWindow() {}
MainWindow::~MainWindow() {}

bool MainWindow::register_class(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS |
        ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);
    register_control_classes(instance);
    register_dialog_classes(instance);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0;
}

std::atomic<int> MainWindow::direct_probe_standin{ 0 };

bool MainWindow::create(HINSTANCE instance, int showCommand, const TestOptions& options)
{
    instance_ = instance;
    options_ = options;
    if (options.direct_check_standin != 0) direct_probe_standin = options.direct_check_standin;
    if (options.refresh_ms > 0) refresh_interval_ms_ = options.refresh_ms;
    StatisticsApi::set_paint_all(options.display_paint_all);
    DisplayPacer::instance().configure(options.pace_mode == 1 ? PaceMode::Display : options.pace_mode == 2 ? PaceMode::Timer : PaceMode::Auto,
        options.pace_max_hz, options.pace_hz);
    // Remembered settings: read once, before anything is built. Automated launches (self-test, screenshots, reports, auto-close)
    // never read or write the user's file unless they name one with --settings; --no-settings turns them off for any launch.
    app::SettingsLoad remembered;
    const bool automated = options.selftest || options.auto_close_ms > 0 || !options.screenshot_file.empty() || !options.popup_file.empty() ||
        !options.thread_report_file.empty() || options.record_refresh;
    if (!options.no_settings && (!options.settings_path.empty() || options.settings_default || !automated)) {
        settings_store_.reset(new app::SettingsStore(win32::make_settings_io(options.settings_path)));
        remembered = settings_store_->load();
        settings_load_note_ = !settings_store_->note().empty();
    }
    const bool haveSettings = settings_store_ != nullptr;
    few_cpu_stored_ = remembered.settings.few_cpu_yield;
    if (options.scale_percent != 0) ctx_.set_scale(options.scale_percent);
    else if (haveSettings && remembered.settings.scale_percent != 100) ctx_.set_scale(remembered.settings.scale_percent);
    scale_percent_ = ctx_.scale_percent();
    if (haveSettings) {
        show_processing_ = remembered.settings.show_processing;
        show_statistics_ = remembered.settings.show_statistics;
    }

    files_.reset(new win32::Win32FileSystem());
    app::PlayerEnvironment env;
    env.services = std::make_shared<win32::Win32EngineServices>();
    env.files = files_.get();
    env.list_devices = [] { startup_mark("devices_begin"); std::vector<platform::OutputDeviceInfo> d = take_output_devices(); startup_mark("devices_listed"); return d; };
    env.create_winmm = [] { startup_mark("winmm_begin"); std::shared_ptr<platform::MidiOutput> o(new win32::WinMmMidiOutput()); startup_mark("winmm_created"); return o; };
    env.create_kdmapi = [] { startup_mark("kdmapi_begin"); std::shared_ptr<platform::MidiOutput> o(new win32::KdmApiMidiOutput()); startup_mark("kdmapi_created"); return o; };
    env.create_winmm_direct = [] { return std::shared_ptr<platform::MidiOutput>(new win32::WinMmDirectOutput()); };   // DD-085
    // R1: decided when the output selection changes, on the controller's worker thread. resolve_direct_device borrows the
    // drivers winmm already loaded and never opens the device.
    env.probe_direct = [](uint32_t id) {
        app::DirectProbeResult result;
        const int standin = direct_probe_standin.load();
        if (standin == 3) Sleep(1500);   // like 1, but answers late (the visual gate ticks the box first)
        if (standin == 1 || standin == 3) { result.reason = "it is served by Windows' own MIDI driver (stand-in for the self-test)"; return result; }
        if (standin == 2) { result.usable = true; return result; }
        win32::DirectResolution r = win32::resolve_direct_device(id);
        if (r.ok && win32::driver_unresponsive(win32::utf8_to_wide(r.driver_file))) {   // DD-094
            result.reason = "the synthesizer's driver stopped answering earlier, so this program does not call it again";
            return result;
        }
        result.usable = r.ok;
        result.reason = r.reason;
        return result;
    };
    env.defer_device_listing = true;   // DD-057
    env.pointer_size = static_cast<int>(sizeof(void*));
    if (sizeof(void*) == 4) {   // 32-bit build: warn at half of the real user-mode address space (2 GiB on 64-bit Windows, 1 GiB on 32-bit Windows / XP, 1.5 GiB with /3GB)
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        env.memory_warning_bytes = midibn::half_address_space_warning_threshold(reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress));
    }
    env.private_bytes = [] { return static_cast<int64_t>(win32::process_memory().current_private); };
    env.wake_ui = [this] {
        HWND target = window_;
        if (target) PostMessageW(target, WM_MIDIBN_WAKE, 0, 0);
    };
    env.culture = system_number_culture();
    env.has_initial_settings = haveSettings;
    env.initial_settings = remembered.settings;
    startup_mark("environment");
    controller_.reset(new app::PlayerController(env));
    startup_mark("controller");
    if (haveSettings && !settings_store_->note().empty())
        controller_->set_settings_note(settings_store_->note().text, settings_store_->note().detail);
    if (options.force_large_warning) controller_->set_force_large_file_checks_for_tests(true, true);

    int clientWidth = options.client_width > 0 ? options.client_width : ctx_.px(790);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT rect = { 0, 0, clientWidth, ctx_.px(500) };
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    // DD-106: v1's CenterScreen: centred in the work area of the monitor that holds the mouse pointer.
    POINT origin = startup_window_origin(width, height);
    int x = origin.x, y = origin.y;
    if (options.has_position) { x = options.position_x; y = options.position_y; }
    CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, kProductName, style, x, y, width, height, nullptr, nullptr, instance, this);
    if (!window_) return false;
    startup_mark("window");
    host_.controller = controller_.get();
    host_.ctx = &ctx_;
    host_.main_window = window_;
    host_.analysis_seek = [this](int64_t microseconds) { controller_->analysis_seek(microseconds); after_action(); };
    host_.toggle_transport = [this] { controller_->play_clicked(); after_action(); };
    host_.open_channels = [this] { open_channels(); };
    host_.request_refresh = [this] { after_action(); };
    if (options.borderless_style >= 0) {
        set_borderless_style(static_cast<BorderlessStyle>(options.borderless_style));
        set_borderless(true);
    }
    else if (haveSettings) {
        set_borderless_style(static_cast<BorderlessStyle>(remembered.settings.borderless_style));
        if (remembered.settings.borderless) set_borderless(true);
    }
    if (haveSettings && remembered.settings.always_on_top) toggle_always_on_top();
    // The few-processor option: offered only on a one-core PC; on by default only with exactly one usable logical processor;
    // a remembered choice wins. The remembered value is kept as read while the option is not offered.
    if (midibn::win32::few_cpu_yield_option_available()) {
        few_cpu_yield_ = app::few_cpu_effective(few_cpu_stored_, midibn::win32::usable_logical_processors());
        controller_->engine().set_behind_yield_microseconds(few_cpu_yield_ ? midibn::win32::kFewCpuYieldOptionMicroseconds : 0);
        update_system_menu_states();
    }
    relayout();
    startup_mark("relayout");
    // Fill every control from the controller's first view (device list, labels, values, enabled states)
    // before the window is shown; relayout() applies the view only when the compact state changes.
    controller_->refresh();
    apply_view(true);
    startup_mark("view");
    ShowWindow(window_, showCommand);
    startup_mark("shown");
    UpdateWindow(window_);
    startup_mark("painted");
    PostMessageW(window_, WM_MIDIBN_ACCESSIBLE_NAMES, 0, 0);   // DD-058
    if (!options.open_file.empty()) {
        controller_->open_file(U(options.open_file));
        test_play_pending_ = options.play;
        after_action();
    }
    if (options.auto_close_ms > 0) SetTimer(window_, kTimerAutoClose, static_cast<UINT>(options.auto_close_ms), nullptr);
    if (!options.popup_file.empty()) SetTimer(window_, kTimerPopupShot, 700, nullptr);
    // --thread-report: with --play the timer starts when playback does (handle_wake); otherwise now.
    if (!options.thread_report_file.empty() && !options.play)
        SetTimer(window_, kTimerThreadReport, static_cast<UINT>(options.thread_report_delay_ms), nullptr);
    if (!options.screenshot_file.empty()) SetTimer(window_, kTimerScreenshot, static_cast<UINT>(options.screenshot_delay_ms), nullptr);
    return true;
}

LRESULT CALLBACK MainWindow::window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    LRESULT result = self->handle(message, wParam, lParam);
    if (message == WM_SYSCOMMAND) self->save_settings_if_changed();   // the title-bar menu's own options (scale, borderless, ...)
    if (message == WM_NCDESTROY) {
        self->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return result;
}

// ------------------------------------------------------------------ construction of the controls

namespace {

// A group frame covers its whole group, so the main window (WS_CLIPCHILDREN) never paints inside it, and the
// standard group box draws only its border and caption: the space between the controls kept whatever the
// window surface held (white on screen). The frame now fills its own interior with the face colour first.
// WS_CLIPSIBLINGS keeps that fill off the controls above it.
LRESULT CALLBACK group_frame_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
        DefSubclassProc(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        EndPaint(window, &ps);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

HWND MainWindow::make_button(int id, const wchar_t* text, DWORD style)
{
    HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    attach_scaled_button(button, &ctx_);   // P10-UI1: check boxes draw at the Application scale (no-op for push buttons)
    return button;
}

HWND MainWindow::make_combo(int id)
{
    return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0,
        100, 200, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
}

HWND MainWindow::make_static_label(int id, int style) { return create_label(window_, id, style, &ctx_); }

void MainWindow::build_controls()
{
    const wchar_t* groupTitles[4] = { L"File and output", L"Processing model", L"Playback", L"Statistics" };
    startup_mark("build_begin");
    for (int i = 0; i < 4; ++i)
        group_[i] = CreateWindowExW(0, L"BUTTON", groupTitles[i], WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 0, 0, 10, 10, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdGroupFile + i)), instance_, nullptr);

    // Caption buttons (borderless chrome): classic strip, integrated header, auto-hide overlay.
    const wchar_t* captionNames[4] = { L"Window menu", L"Minimize window", L"Maximize window", L"Close window" };
    const CaptionGlyph glyphs[4] = { GlyphMenu, GlyphMinimize, GlyphMaximize, GlyphClose };
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 4; ++k) {
            caption_[s][k] = create_caption_button(window_, IdCaptionBase + s * 10 + k, glyphs[k], &ctx_, captionNames[k]);
            SendMessageW(caption_[s][k], WM_USER + 1,
                s == 1 ? GetSysColor(COLOR_BTNFACE) : GetSysColor(COLOR_BTNSHADOW), 0);   // strip and overlay are ControlDark
            ShowWindow(caption_[s][k], SW_HIDE);
            set_accessible_name(caption_[s][k], captionNames[k]);
        }

    startup_mark("captions");
    open_ = make_button(IdOpen, L"Open MIDI...");
    file_label_ = make_static_label(IdFileLabel, LabelLeft | LabelEllipsis | LabelTooltip);
    info_label_ = make_static_label(IdInfoLabel, LabelRight | LabelTooltip);
    load_status_ = make_static_label(IdLoadStatus, LabelLeft | LabelEllipsis | LabelTooltip);
    progress_overall_ = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProgressOverall)), instance_, nullptr);
    progress_stage_ = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProgressStage)), instance_, nullptr);
    SendMessageW(progress_overall_, PBM_SETRANGE32, 0, 1000);
    SendMessageW(progress_stage_, PBM_SETRANGE32, 0, 1000);
    output_label_ = make_static_label(IdOutputLabel, LabelRight);
    SetWindowTextW(output_label_, L"MIDI output:");
    output_ = make_combo(IdOutput);
    direct_ = make_button(IdDirect, L"Direct WinMM", BS_AUTOCHECKBOX);   // created before KDMAPI: Tab goes output list, Direct WinMM, KDMAPI
    kdmapi_ = make_button(IdKdmapi, L"KDMAPI", BS_AUTOCHECKBOX);
    state_ = make_static_label(IdState, LabelRight);

    queue_check_ = make_button(IdQueueCheck, L"Queue length limit:", BS_AUTOCHECKBOX);
    queue_value_ = create_scrub_field(window_, IdQueueValue, &ctx_, &controller_culture_, [this](int64_t start, int64_t pixels) {
        return controller_->scrub_value(false, start, pixels);
    });
    events_label_ = make_static_label(IdEventsLabel, LabelLeft);
    overflow_label_ = make_static_label(IdOverflowLabel, LabelRight);
    SetWindowTextW(overflow_label_, L"Overflow:");
    overflow_ = make_combo(IdOverflow);
    rate_label_ = make_static_label(IdRateLabel, LabelLeft);
    rate_ = make_combo(IdRate);
    for (int i = 0; i < app::kRateModelChoiceCount; ++i)
        SendMessageW(rate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(app::rate_choice_full_label(rate_index_to_choice(i))).c_str()));
    SendMessageW(rate_, CB_SETCURSEL, 0, 0);
    install_rate_popup(rate_, &ctx_);
    service_label_ = make_static_label(IdServiceLabel, LabelLeft);
    processing_value_ = create_scrub_field(window_, IdProcessingValue, &ctx_, &controller_culture_, [this](int64_t start, int64_t pixels) {
        return controller_->scrub_value(true, start, pixels);
    });
    unit_label_ = make_static_label(IdUnitLabel, LabelLeft);
    din_ = make_button(IdDin, L"5-pin DIN");
    slider_ = create_slider(window_, IdSlider, &ctx_);

    timeline_ = create_timeline(window_, IdTimeline, &ctx_);
    play_ = make_button(IdPlay, L"Play");
    stop_ = make_button(IdStop, L"Stop");
    back5_ = make_button(IdBack5, L"\x2212" L"5 sec");
    forward5_ = make_button(IdForward5, L"+5 sec");
    analysis_ = make_button(IdAnalysis, L"Analysis...");
    reset_ = make_button(IdReset, L"Reset stats");
    stats_ = create_statistics(window_, IdStats, &ctx_);

    startup_mark("controls");
    set_all_fonts();
    // The closed height of a native combo box depends on the font.
    RECT r;
    GetWindowRect(output_, &r);
    combo_closed_h_ = r.bottom - r.top;

    startup_mark("fonts");
    tooltips_.create(window_);
    HWND tipped[] = { open_, file_label_, info_label_, output_, direct_, kdmapi_, queue_check_, queue_value_, overflow_, rate_, processing_value_, din_,
        slider_, play_, reset_, back5_, forward5_, analysis_, stop_, load_status_, progress_overall_, progress_stage_ };
    for (size_t i = 0; i < sizeof(tipped) / sizeof(tipped[0]); ++i) tooltips_.add_control(tipped[i]);

    startup_mark("tooltips");
    // The window text of these controls is what accessibility tools read as their name.
    SetWindowTextW(timeline_, L"Playback position");
    SetWindowTextW(slider_, L"Rate");
    SetWindowTextW(stats_, L"Statistics readouts");
    set_accessible_name(output_, L"MIDI output");
    set_accessible_name(rate_, L"Rate model");
    set_accessible_name(overflow_, L"Overflow policy");
    set_accessible_name(slider_, L"Rate");
    set_accessible_name(timeline_, L"Playback position");
    set_accessible_name(stats_, L"Statistics");
    set_accessible_name(queue_value_, L"Queue length limit");
    set_accessible_name(processing_value_, L"Processing time per event");
    set_accessible_name(file_label_, L"File name");
    set_accessible_name(info_label_, L"File information");
    set_accessible_name(state_, L"Playback state");
    startup_mark("access_names");
    // The group frames were created first, which puts them at the top of the sibling z-order. Every control
    // has WS_CLIPSIBLINGS, so a frame above it would clip it away completely: move the frames to the bottom
    // (they take no input and are not tab stops, so the tab order is unchanged).
    for (int i = 0; i < 4; ++i) {
        SetWindowPos(group_[i], HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        SetWindowSubclass(group_[i], &group_frame_proc, 1, 0);
    }
    install_child_hooks();
    startup_mark("hooks");
}

void MainWindow::set_all_fonts()
{
    HWND all[] = { group_[0], group_[1], group_[2], group_[3], open_, output_, direct_, kdmapi_, queue_check_, queue_value_, overflow_, rate_,
        processing_value_, din_, play_, stop_, back5_, forward5_, analysis_, reset_ };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) SendMessageW(all[i], WM_SETFONT, reinterpret_cast<WPARAM>(ctx_.ui_font()), TRUE);
}

void MainWindow::install_child_hooks()
{
    for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        install_edge_passthrough(child, 0);
        DragAcceptFiles(child, TRUE);
    }
}

// ------------------------------------------------------------------ layout

bool MainWindow::compute_compact(int clientWidth) const { return clientWidth < ctx_.px(640); }

int MainWindow::non_client_height() const
{
    if (!window_) return 0;
    RECT window, client;
    GetWindowRect(window_, &window);
    GetClientRect(window_, &client);
    return std::max<LONG>(0, (window.bottom - window.top) - client.bottom);
}

void MainWindow::relayout()
{
    // DD-090: a size change that arrives while the layout runs (the window's own height correction, or a size set by
    // another program while this one is busy) is not dropped: the layout runs again for the final size.
    if (in_layout_) { relayout_again_ = true; return; }
    if (!window_ || IsIconic(window_)) return;
    in_layout_ = true;
    for (int pass = 0; pass < 3; ++pass) {
        relayout_again_ = false;
        RECT c;
        GetClientRect(window_, &c);
        if (c.right > 0) {
            bool compact = compute_compact(c.right);
            if (compact != compact_) {
                compact_ = compact;
                controller_->set_compact(compact_);
                StatisticsApi::set_compact(stats_, compact_);
                controller_->refresh();
                apply_view(true);
            }
            required_client_height_ = layout(c.right, true);
            apply_window_constraints();
        }
        if (!relayout_again_) break;
    }
    in_layout_ = false;
}

void MainWindow::apply_window_constraints()
{
    required_outer_height_ = required_client_height_ + non_client_height();
    if (IsZoomed(window_) || IsIconic(window_)) return;
    RECT rect;
    GetWindowRect(window_, &rect);
    if (rect.bottom - rect.top != required_outer_height_)
        SetWindowPos(window_, nullptr, 0, 0, rect.right - rect.left, required_outer_height_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int MainWindow::layout(int W_, bool apply)
{
    const app::PlayerView& v = last_view_;
    const bool compact = compact_;
    const int fh = ctx_.font_height();
    auto px = [this](int c) { return ctx_.px(c); };
    const int btnH = fh + ctx_.button_padding();   // DD-099
    const int comboH = combo_closed_h_ > 0 ? combo_closed_h_ : fh + px(8);
    const int checkH = fh + px(4);
    const int editH = fh + px(4);
    const int dropH = px(220);

    HDWP defer = apply ? BeginDeferWindowPos(90) : nullptr;
    auto place = [&](HWND window, int x, int y, int w, int h, bool visible, int extraHeight = 0) {
        if (!apply || window == nullptr) return;
        // Leave a control alone when nothing about it changes. While a file loads, every progress text change
        // relays the window out; repositioning a combo box makes it momentarily as tall as its drop-down list
        // (until it shrinks itself back), which covered the controls below it in the visual gate.
        RECT current;
        GetWindowRect(window, &current);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&current), 2);
        const int targetH = std::max(1, h + extraHeight), closedH = std::max(1, h);
        const int curH = current.bottom - current.top;
        if (current.left == x && current.top == y && current.right - current.left == std::max(1, w) &&
            (curH == targetH || (extraHeight > 0 && curH == closedH)) &&
            ((GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) != 0) == visible)
            return;
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        // DD-090: a control whose size changes is repainted completely. Without SWP_NOCOPYBITS Windows keeps the old
        // pixels and repaints only the newly uncovered strip, which left stale text and frame lines behind while the
        // window was resized (right-aligned labels, the statistics columns, the group frames). A control that only
        // moves keeps its pixels (they are still right).
        if (current.right - current.left != std::max(1, w) || (curH != targetH && !(extraHeight > 0 && curH == closedH)))
            flags |= SWP_NOCOPYBITS;
        defer = DeferWindowPos(defer, window, nullptr, x, y, std::max(1, w), std::max(1, h + extraHeight), flags);
    };

    const bool strip = borderless_ && style_ == BorderlessStyle::ClassicStrip;
    const bool integrated = borderless_ && style_ == BorderlessStyle::IntegratedHeader;
    const bool overlay = borderless_ && style_ == BorderlessStyle::AutoHideOverlay;
    const int pad = px(compact ? 2 : 5);
    const int stripH = strip ? px(30) : 0;
    strip_rect_ = { 0, 0, strip ? W_ : 0, stripH };
    int y = stripH + pad;
    const int gx = pad + px(compact ? 1 : 2);
    const int gw = std::max(10, W_ - 2 * gx);
    const int padL = px(compact ? 3 : 6), padT = px(compact ? 3 : 5), padB = px(compact ? 2 : 4);
    const int tp = px(1);
    const int gmTop = px(compact ? 0 : 1), gmBottom = px(compact ? 1 : 3);
    const int ix0 = gx + padL + tp, ix1 = gx + gw - padL - tp;
    const int iw = ix1 - ix0;

    // ---- caption buttons
    const int capW = px(42), capH = px(30);
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 4; ++k) place_hidden_[s][k] = true;

    // ---- File and output
    y += gmTop;
    const int g1top = y;
    const int innerTop1 = g1top + (integrated ? 0 : fh) + padT + tp;
    const int row0 = std::max(px(32), btnH + px(6));
    const int row1 = comboH + px(6);
    const int g1h = (innerTop1 - g1top) + row0 + row1 + tp + padB;

    // DD-091 (user-approved layout change, prompt 009): the Open MIDI... button fits its text with normal push-button
    // padding (8 canonical pixels a side), measured with the button's own font; it keeps one width for both of its texts
    // ("Open MIDI..." and "Cancel" while loading) and Windows' 75-pixel minimum, all scaled.
    HFONT openFont = open_ ? reinterpret_cast<HFONT>(SendMessageW(open_, WM_GETFONT, 0, 0)) : nullptr;
    if (openFont == nullptr) openFont = ctx_.ui_font();
    int openW = std::max(ctx_.text_width(openFont, L"Open MIDI..."), ctx_.text_width(openFont, L"Cancel")) + px(16);
    openW = std::max(openW, px(75));
    int menuW = integrated ? px(30) : 0;
    int outputLabelW = ctx_.text_width(L"MIDI output:") + px(6);
    int block0 = (integrated ? menuW + px(2) : 0) + openW + px(6);
    int col0W = std::max(block0, outputLabelW + px(3));
    int winBtnW = px(30);
    int winBtnsW = integrated ? 3 * winBtnW + px(4) : 0;

    int rowTop0 = innerTop1;
    int rowTop1 = rowTop0 + row0;
    file_row_rect_ = { ix0, rowTop0, ix1, rowTop0 + row0 };

    {
        int x = ix0;
        int btnY = rowTop0 + (row0 - btnH) / 2;
        if (integrated) {
            place(caption_[1][0], x, btnY, menuW, btnH, true);
            place_hidden_[1][0] = false;
            x += menuW + px(2);
        }
        place(open_, x, btnY, openW, btnH, true);
        // header: file name / info or loading status
        int hx = ix0 + col0W;
        int hEnd = ix1 - winBtnsW;
        int hw = std::max(10, hEnd - hx);
        if (v.loading) {
            int fileMin = compact ? 145 : 240, statusMin = compact ? 165 : 215;
            int statusIdeal = px(statusMin);
            std::wstring status = W(v.load_status);
            size_t cut = 0;
            while (cut <= status.size()) {
                size_t next = status.find_first_of(L"\r\n", cut);
                std::wstring line = status.substr(cut, next == std::wstring::npos ? std::wstring::npos : next - cut);
                statusIdeal = std::max(statusIdeal, ctx_.text_width(line) + px(12));
                if (next == std::wstring::npos) break;
                cut = next + 1;
            }
            statusIdeal = std::min(statusIdeal, px(compact ? 215 : 300));
            int statusW = std::min(statusIdeal, std::max(px(statusMin), hw - px(fileMin)));
            if (hw < px(fileMin) + px(statusMin)) statusW = std::max(1, hw / 2);
            place(file_label_, hx, rowTop0, hw - statusW, row0, true);
            place(load_status_, hx + hw - statusW + px(2), rowTop0, statusW - px(2), row0, true);
            place(info_label_, hx, rowTop0, 1, row0, false);
        }
        else {
            // DD-103: in the integrated header the short direct-status text sits at the top right, just left of the
            // minimize button, and takes its share out of the file-information space (at most 40 % of the header; the full
            // sentence is its tooltip; the file information keeps its two-line form and its tooltip).
            // (revised, P11-DS: the status moved to the Playback frame's caption row; the file information is back here,
            // fitted as in every other header style, so it never moves into the tooltip.)
            int fw = hw;
            int fileW = fw * (compact ? 45 : 48) / 100;
            info_in_file_tooltip_ = false;
            if (!v.file_info.empty()) {
                // P11-DS: decided by measured width, never by clipping: when even the two-line form is wider than the
                // room, the file information moves into the file name's tooltip and the name gets the room.
                std::wstring fitted = W(fit_file_info(v.file_info, fw - fileW, ctx_));
                int widest = 0;
                for (size_t cut = 0; cut <= fitted.size();) {
                    size_t next = fitted.find_first_of(L"\r\n", cut);
                    widest = std::max(widest, ctx_.text_width(fitted.substr(cut, next == std::wstring::npos ? std::wstring::npos : next - cut)));
                    if (next == std::wstring::npos) break;
                    cut = next + 1;
                }
                if (widest > fw - fileW - px(4)) { fileW = fw; info_in_file_tooltip_ = true; }
            }
            place(file_label_, hx, rowTop0, fileW, row0, true);
            place(info_label_, hx + fileW, rowTop0, fw - fileW, row0, !info_in_file_tooltip_);
            if (apply) set_text_cached(info_label_, fit_file_info(v.file_info, fw - fileW, ctx_));   // DD-099
            place(load_status_, hx, rowTop0, 1, row0, false);
        }
        if (integrated) {
            int bx = ix1 - 3 * winBtnW;
            for (int k = 1; k <= 3; ++k) {
                place(caption_[1][k], bx + (k - 1) * winBtnW, btnY, winBtnW, btnH, true);
                place_hidden_[1][k] = false;
            }
        }
        // second row
        int labelY = rowTop1 + (row1 - fh) / 2;
        place(output_label_, ix0, rowTop1, col0W - px(3), row1, true);
        (void)labelY;
        int kdW = ctx_.text_width(L"KDMAPI") + ctx_.check_box_space();   // DD-099
        int directW = ctx_.text_width(L"Direct WinMM") + ctx_.check_box_space();
        int kdMargin = px(compact ? 4 : 10);
        // P12-CHK: the playback state text moved to the Playback heading line, so only the loading panel's progress bars
        // use the right end of this row; the output list, "Direct WinMM" and KDMAPI share the rest.
        int rightW = v.loading ? px(compact ? 118 : 180) + px(compact ? 3 : 7) : 0;
        int rightX = ix1 - rightW;
        int kdX = rightX - kdW - px(2);
        int directX = kdX - directW - px(compact ? 6 : 10);
        int comboX = ix0 + col0W;
        int comboW = std::max(40, directX - kdMargin - comboX);
        int comboY = rowTop1 + (row1 - comboH) / 2;
        place(output_, comboX, comboY, comboW, comboH, true, dropH);
        place(direct_, directX, rowTop1 + (row1 - checkH) / 2, directW, checkH, true);
        place(kdmapi_, kdX, rowTop1 + (row1 - checkH) / 2, kdW, checkH, true);
        if (v.loading) {
            int pw = px(compact ? 118 : 180);
            int px0 = ix1 - pw - px(compact ? 1 : 3);
            int top = rowTop1 + (row1 - px(23)) / 2;
            place(progress_overall_, px0, top + px(1), pw, px(14), true);
            place(progress_stage_, px0, top + px(16), pw, px(6), true);
        }
        else {
            place(progress_overall_, rightX, rowTop1, 1, 1, false);
            place(progress_stage_, rightX, rowTop1, 1, 1, false);
        }
    }
    place(group_[0], gx, integrated ? g1top - fh / 2 : g1top, gw, g1h + (integrated ? fh / 2 : 0), true);
    y = g1top + g1h + gmBottom;

    // ---- Processing model
    HWND processing[] = { queue_check_, queue_value_, events_label_, overflow_label_, overflow_, rate_label_, rate_, service_label_,
        processing_value_, unit_label_, din_, slider_ };
    if (show_processing_) {
        y += gmTop;
        const int g2top = y;
        const int tp2 = compact ? 0 : tp;
        const int innerTop2 = g2top + fh + padT + tp2;
        const int rowA = comboH + px(6), rowB = comboH + px(6);
        const int sliderH = px(compact ? 32 : 36);
        const int rowC = sliderH + px(compact ? 2 : 6);
        const int g2h = (innerTop2 - g2top) + rowA + rowB + rowC + tp2 + padB;
        const int colSplit = ix0 + iw * (compact ? 51 : 48) / 100;
        const int rightStart = colSplit + px(compact ? 0 : 4);
        const int leftW = colSplit - ix0;
        const int rightW = ix1 - rightStart;
        int rowAy = innerTop2, rowBy = rowAy + rowA, rowCy = rowBy + rowB;

        // queue cluster
        int checkW = ctx_.text_width(W(v.queue_check_text)) + ctx_.check_box_space();   // DD-099
        int valueW = scrub_field_preferred_width(queue_value_);
        std::wstring eventsText = W(v.events_label);
        int eventsW = ctx_.text_width(eventsText) + px(4);
        int x = ix0;
        place(queue_check_, x, rowAy + (rowA - checkH) / 2, checkW, checkH, true);
        x += checkW + px(1);
        place(queue_value_, x, rowAy + (rowA - editH) / 2, valueW, editH, true);
        x += valueW + px(compact ? 3 : 4);
        bool eventsVisible = !compact || (x + eventsW <= ix0 + leftW);
        place(events_label_, x, rowAy, std::max(1, eventsW), rowA, eventsVisible);
        // overflow cluster
        int ovLabelW = ctx_.text_width(L"Overflow:") + px(4);
        int ovX = rightStart + px(8);
        place(overflow_label_, ovX, rowAy, ovLabelW, rowA, true);
        int ovComboX = ovX + ovLabelW + px(3);
        int ovComboW = compact ? std::max(40, ix1 - ovComboX - px(3)) : std::min(px(235), std::max(40, ix1 - ovComboX - px(3)));
        place(overflow_, ovComboX, rowAy + (rowA - comboH) / 2, ovComboW, comboH, true, dropH);
        // rate cluster
        int rateLabelW = ctx_.text_width(W(v.rate_label)) + px(5);
        place(rate_label_, ix0, rowBy, rateLabelW, rowB, true);
        int rateComboX = ix0 + rateLabelW + px(2);
        int rateComboW = compact ? std::max(40, ix0 + leftW - rateComboX - px(3)) : px(210);
        place(rate_, rateComboX, rowBy + (rowB - comboH) / 2, rateComboW, comboH, true, dropH);
        last_rate_width_ = rateComboW;
        // service cluster
        bool serviceVisible = v.processing_value.visible;
        int svcLabelW = ctx_.text_width(W(v.service_value_label)) + px(5);
        int sx = rightStart + px(compact ? 0 : 12);
        place(service_label_, sx, rowBy, svcLabelW, rowB, serviceVisible);
        sx += svcLabelW + px(2);
        int pvW = scrub_field_preferred_width(processing_value_);
        place(processing_value_, sx, rowBy + (rowB - editH) / 2, pvW, editH, serviceVisible);
        sx += pvW + px(3);
        int unitW = ctx_.text_width(W(v.service_unit)) + px(4);
        place(unit_label_, sx, rowBy, std::max(1, unitW), rowB, serviceVisible && v.service_unit_visible);
        sx += unitW + px(3);
        int dinW = ctx_.text_width(L"5-pin DIN") + ctx_.check_box_space();   // DD-099
        place(din_, sx, rowBy + (rowB - btnH) / 2, dinW, btnH, v.din_visible);
        place(slider_, ix0 + px(3), rowCy + px(compact ? 2 : 5), iw - px(6), sliderH, true);
        place(group_[1], gx, g2top, gw, g2h, true);
        (void)rightW;
        y = g2top + g2h + gmBottom;
    }
    else {
        for (size_t i = 0; i < sizeof(processing) / sizeof(processing[0]); ++i) place(processing[i], 0, 0, 1, 1, false);
        place(group_[1], 0, 0, 1, 1, false);
    }

    // ---- Playback
    {
        y += gmTop;
        const int g3top = y;
        const int top3 = compact ? 0 : padT;
        const int innerTop3 = g3top + fh + top3 + tp;
        const int tlMargin = px(compact ? 1 : 3);
        const int tlH = px(compact ? 20 : 38);
        const int g3h = (innerTop3 - g3top) + (tlH + 2 * tlMargin) + (btnH + 2 * tlMargin) + tp + padB;
        place(timeline_, ix0 + tlMargin, innerTop3 + tlMargin, iw - 2 * tlMargin, tlH, true);
        int by = innerTop3 + tlH + 2 * tlMargin + tlMargin;
        int bx = ix0 + tlMargin;
        const int gap = tlMargin;
        auto buttonWidth = [&](const wchar_t* text, int minimum) { return std::max(px(minimum), ctx_.text_width(text) + px(compact ? 14 : 24)); };
        int playW = std::max(buttonWidth(L"Resume", 54), buttonWidth(L"Pause", 54));
        int stopW = buttonWidth(L"Stop", 54);
        int backW = buttonWidth(L"\x2212" L"5 sec", 59), fwdW = buttonWidth(L"+5 sec", 59);
        int anW = buttonWidth(L"Analysis...", 76), rsW = buttonWidth(L"Reset stats", 76);
        place(play_, bx, by, playW, btnH, true); bx += playW + 2 * gap;
        place(stop_, bx, by, stopW, btnH, true); bx += stopW + 2 * gap;
        place(back5_, bx, by, backW, btnH, true); bx += backW + 2 * gap;
        place(forward5_, bx, by, fwdW, btnH, true); bx += fwdW + 2 * gap;
        place(analysis_, bx, by, anW, btnH, true);
        place(reset_, ix1 - tlMargin - rsW, by, rsW, btnH, true);
        place(group_[2], gx, g3top, gw, g3h, true);
        {
            // P12-CHK (prompt 012): the playback state (Stopped / Paused / Playing / Completed) sits at the right end of the
            // Playback frame's heading line in every view (it used to sit at the right of the output row, which the loading
            // panel replaced while a file loaded). As wide as the longest state word, so the box never changes size;
            // it never touches the "Playback" heading. It shows while loading too.
            int headingEnd = gx + px(10) + ctx_.text_width(L"Playback") + px(10);
            int stateRight = gx + gw - px(8);
            int widest = 0;
            for (const wchar_t* word : { L"Stopped", L"Playing", L"Paused", L"Completed" }) widest = std::max(widest, ctx_.text_width(word));
            int stateW = std::min(widest + px(8), std::max(1, stateRight - headingEnd));
            place(state_, stateRight - stateW, g3top, std::max(1, stateW), fh, true);
        }
        y = g3top + g3h + gmBottom;
    }

    // ---- Statistics
    if (show_statistics_) {
        y += gmTop;
        const int g4top = y;
        const int top4 = compact ? 0 : padT;
        const int innerTop4 = g4top + fh + top4;
        const int statsH = StatisticsApi::preferred_height(stats_);
        const int g4h = (innerTop4 - g4top) + statsH + padB;
        place(stats_, ix0, innerTop4, iw, statsH, true);
        place(group_[3], gx, g4top, gw, g4h, true);
        y = g4top + g4h + gmBottom;
    }
    else {
        place(stats_, 0, 0, 1, 1, false);
        place(group_[3], 0, 0, 1, 1, false);
    }

    // ---- strip and overlay buttons
    if (strip) {
        place(caption_[0][0], 0, 0, capW, capH, true);
        place_hidden_[0][0] = false;
        place(caption_[0][3], W_ - capW, 0, capW, capH, true);
        place(caption_[0][2], W_ - 2 * capW, 0, capW, capH, true);
        place(caption_[0][1], W_ - 3 * capW, 0, capW, capH, true);
        for (int k = 1; k < 4; ++k) place_hidden_[0][k] = false;
    }
    if (overlay) {
        int inset = px(4);
        int ox = std::max(0, W_ - 4 * capW - inset);
        for (int k = 0; k < 4; ++k) {
            place(caption_[2][k], ox + k * capW, inset, capW, capH, overlay_visible_);
            place_hidden_[2][k] = !overlay_visible_;
        }
    }
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 4; ++k)
            if (place_hidden_[s][k]) place(caption_[s][k], 0, 0, 1, 1, false);

    if (apply && defer) EndDeferWindowPos(defer);
    if (apply && strip_rect_.bottom > 0) InvalidateRect(window_, nullptr, FALSE);
    if (apply) {
        // Overlay and strip buttons must be above everything else.
        if (overlay && overlay_visible_)
            for (int k = 0; k < 4; ++k) SetWindowPos(caption_[2][k], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return y + pad;
}

// ------------------------------------------------------------------ presentation

void MainWindow::update_rate_combo_labels()
{
    // v1 RateModelComboBox.UpdateCollapsedLabels: show the long name when it fits the collapsed field.
    RECT r;
    GetClientRect(rate_, &r);
    int usable = std::max<int>(0, r.right - GetSystemMetrics(SM_CXVSCROLL) - px_(4));
    int selected = static_cast<int>(SendMessageW(rate_, CB_GETCURSEL, 0, 0));
    for (int i = 0; i < app::kRateModelChoiceCount; ++i) {
        app::RateModelChoice choice = rate_index_to_choice(i);
        std::wstring full = W(app::rate_choice_full_label(choice));
        std::wstring display = ctx_.text_width(full) <= usable ? full : W(app::rate_choice_fallback_label(choice));
        if (last_rate_labels_[i] == display) continue;
        last_rate_labels_[i] = display;
        SendMessageW(rate_, CB_DELETESTRING, static_cast<WPARAM>(i), 0);
        SendMessageW(rate_, CB_INSERTSTRING, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(display.c_str()));
    }
    SendMessageW(rate_, CB_SETCURSEL, static_cast<WPARAM>(std::max(0, selected)), 0);
}

int MainWindow::px_(int canonical) const { return ctx_.px(canonical); }

void MainWindow::apply_view(bool force)
{
    applying_view_ = true;
    const app::PlayerView& v = controller_->view();
    const bool first = !view_valid_ || force;
    const app::PlayerView& o = last_view_;

    // Fast path (every display-refresh tick while playing): only the readouts changed.
    if (!first && v.same_controls(o)) {
        TimelineApi::set_timeline(timeline_, v.position_us, v.duration_us);
        set_statistics_metered(v.statistics);
        if (v.state_text != o.state_text) set_text_cached(state_, v.state_text);
        last_view_.statistics = v.statistics;
        last_view_.position_us = v.position_us;
        last_view_.state_text = v.state_text;
        applying_view_ = false;
        if (play_started_tick_ != 0 && play_completed_tick_ == 0 && v.state_text == "Completed") {
            play_completed_tick_ = GetTickCount();
            completed_events_ = controller_->engine().snapshot().processed_events;
        }
        return;
    }

    // layout-affecting signature
    std::wstring signature = std::to_wstring(v.loading) + L"|" + W(v.service_value_label) + L"|" + W(v.service_unit) + L"|" +
        std::to_wstring(v.service_unit_visible) + std::to_wstring(v.processing_value.visible) + std::to_wstring(v.din_visible) + L"|" +
        W(v.queue_check_text) + L"|" + W(v.events_label) + L"|" + W(v.rate_label) + L"|" + W(v.open_button_text) + L"|" +
        std::to_wstring(scrub_field_preferred_width(queue_value_)) + L"|" + std::to_wstring(scrub_field_preferred_width(processing_value_));
    size_t cut = v.load_status.find_first_of("\r\n");
    if (v.loading) signature += L"|" + W(v.load_status.substr(0, cut));
    // the status line is sized to its short text; in the integrated header it shares a row with the output list (DD-088)
    signature += L"|p|" + std::to_wstring(v.output_items.size());

    set_text_cached(open_, v.open_button_text);
    set_text_cached(file_label_, v.file_text);
    set_text_cached(info_label_, fit_file_info(v.file_info, client_width(info_label_), ctx_));   // DD-099
    set_text_cached(load_status_, v.load_status);
    set_text_cached(state_, v.state_text);
    set_check(direct_, v.direct_checked);
    enable(direct_, v.direct_enabled);
    set_text_cached(queue_check_, v.queue_check_text);
    set_text_cached(events_label_, v.events_label);
    set_text_cached(rate_label_, v.rate_label);
    set_text_cached(service_label_, v.service_value_label);
    set_text_cached(unit_label_, v.service_unit);
    set_text_cached(play_, v.play_text);

    if (first || v.output_items != o.output_items) fill_combo(output_, v.output_items, o.output_items, true);
    set_combo_selection(output_, v.output_selected);
    enable(output_, v.output_enabled);
    set_check(kdmapi_, v.kdmapi_checked);
    set_check(queue_check_, v.queue_check_checked);
    enable(queue_check_, v.queue_check_enabled);
    scrub_field_apply(queue_value_, v.queue_value);
    if (first || v.overflow_items != o.overflow_items) fill_combo(overflow_, v.overflow_items, o.overflow_items, true);
    set_combo_selection(overflow_, v.overflow_selected);
    enable(overflow_, v.overflow_enabled);
    rate_popup_set_forward_only(rate_, v.rate_forward_heading);
    set_combo_selection(rate_, rate_choice_to_index(v.rate_choice));
    scrub_field_apply(processing_value_, v.processing_value);
    if (static_cast<int>(SendMessageW(slider_, TBM_GETPOS, 0, 0)) != v.slider_value) SendMessageW(slider_, TBM_SETPOS, TRUE, v.slider_value);
    enable(slider_, v.slider_enabled);
    enable(din_, v.din_enabled);
    enable(play_, v.play_enabled);
    enable(stop_, v.stop_enabled);
    enable(back5_, v.seek_enabled);
    enable(forward5_, v.seek_enabled);
    enable(analysis_, v.seek_enabled);
    enable(timeline_, v.seek_enabled);
    TimelineApi::set_timeline(timeline_, v.position_us, v.duration_us);
    set_statistics_metered(v.statistics);
    if (first || v.load_overall_permille != o.load_overall_permille)
        SendMessageW(progress_overall_, PBM_SETPOS, static_cast<WPARAM>(v.load_overall_permille), 0);
    if (first || v.load_stage_permille != o.load_stage_permille)
        SendMessageW(progress_stage_, PBM_SETPOS, static_cast<WPARAM>(v.load_stage_permille), 0);

    bool relayoutNeeded = first || signature != layout_signature_;
    layout_signature_ = signature;
    last_view_ = v;
    view_valid_ = true;
    applying_view_ = false;
    if (play_started_tick_ != 0 && play_completed_tick_ == 0 && v.state_text == "Completed") {
        play_completed_tick_ = GetTickCount();
        completed_events_ = controller_->engine().snapshot().processed_events;
    }
    if (relayoutNeeded) {
        update_rate_combo_labels();
        relayout();
        update_rate_combo_labels();
    }
}

std::string MainWindow::compute_report()
{
    std::string result;
    if (play_started_tick_ != 0) {
        DWORD end = play_completed_tick_ != 0 ? play_completed_tick_ : GetTickCount();
        int64_t events = play_completed_tick_ != 0 ? completed_events_ : controller_->engine().snapshot().processed_events;
        char text[256];
        std::snprintf(text, sizeof(text), "play_completed=%d\nplay_ms=%lu\nevents_processed=%lld\n", play_completed_tick_ != 0 ? 1 : 0,
            static_cast<unsigned long>(end - play_started_tick_), static_cast<long long>(events));
        result = text;
    }
    if (options_.record_refresh) {
        // Evenness of the live updates (time between two consecutive updates) and what each update costs the UI thread.
        std::vector<int32_t> sorted = refresh_intervals_us_;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0;
        for (size_t i = 0; i < sorted.size(); ++i) sum += sorted[i];
        size_t n = sorted.size();
        double mean = n ? sum / static_cast<double>(n) : 0;
        double variance = 0;
        for (size_t i = 0; i < n; ++i) variance += (sorted[i] - mean) * (sorted[i] - mean);
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        int64_t posted = 0, coalesced = 0;
        DisplayPacer::instance().counters(posted, coalesced);
        char text[768];
        std::snprintf(text, sizeof(text),
            "refresh_pacing=%s\nrefresh_updates=%lld\nrefresh_interval_ms mean=%.3f sd=%.3f p50=%.3f p99=%.3f max=%.3f\n"
            "refresh_ui_ms_total=%.1f per_update=%.3f\npacer_posted=%lld pacer_coalesced=%lld\n",
            options_.refresh_ms > 0 ? "old Windows timer (test switch)" : DisplayPacer::instance().description().c_str(),
            static_cast<long long>(refresh_count_), mean / 1000.0, n ? std::sqrt(variance / static_cast<double>(n)) / 1000.0 : 0.0,
            n ? sorted[n / 2] / 1000.0 : 0.0, n ? sorted[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99))] / 1000.0 : 0.0,
            n ? sorted[n - 1] / 1000.0 : 0.0, refresh_busy_qpc_ * 1000.0 / static_cast<double>(frequency.QuadPart),
            refresh_count_ ? refresh_busy_qpc_ * 1000.0 / static_cast<double>(frequency.QuadPart) / static_cast<double>(refresh_count_) : 0.0,
            static_cast<long long>(posted), static_cast<long long>(coalesced));
        result += text;
        // Adaptive display rate: what an update costs and where, per update, and the tiers the run went through.
        const double ticksToUs = 1000000.0 / static_cast<double>(frequency.QuadPart);
        const double perN = static_cast<double>(std::max<int64_t>(1, meter_total_.updates));
        LARGE_INTEGER nowCounter;
        QueryPerformanceCounter(&nowCounter);
        int64_t inTier[app::DisplayLadder::MaxTiers];
        for (int i = 0; i < app::DisplayLadder::MaxTiers; ++i) inTier[i] = tier_time_qpc_[i];
        if (tier_since_qpc_ != 0 && tier_state_.tier < app::DisplayLadder::MaxTiers) inTier[tier_state_.tier] += nowCounter.QuadPart - tier_since_qpc_;
        char cost[900];
        int used = std::snprintf(cost, sizeof(cost),
            "display_adaptive=%s budget_pct=%.1f native_hz=%d final_tier=%d tier_changes=%d\n"
            "display_cost_us_per_update tick=%.1f (statistics_values=%.1f) paint_timeline=%.1f paint_statistics=%.1f paint_other=%.1f total=%.1f\n"
            "display_tier_ms",
            options_.display_fixed_tier >= 0 ? "fixed tier" : options_.adaptive_display ? "on" : "off", tier_params_.budget * 100.0,
            tier_native_hz_, tier_state_.tier, tier_changes_, meter_total_.tick * ticksToUs / perN, meter_total_.stats_set * ticksToUs / perN,
            meter_total_.paint_timeline * ticksToUs / perN, meter_total_.paint_stats * ticksToUs / perN, meter_total_.paint_other * ticksToUs / perN,
            (meter_total_.tick + meter_total_.paint_timeline + meter_total_.paint_stats + meter_total_.paint_other) * ticksToUs / perN);
        for (int i = 0; i < ladder_.count && used > 0 && used < static_cast<int>(sizeof(cost)) - 40; ++i)
            used += std::snprintf(cost + used, sizeof(cost) - static_cast<size_t>(used), " t%d=%.0f", i, inTier[i] * 1000.0 / static_cast<double>(frequency.QuadPart));
        if (used > 0 && used < static_cast<int>(sizeof(cost)) - 2) { cost[used++] = '\n'; cost[used] = 0; }
        result += cost;
        if (!tier_log_.empty()) result += tier_log_;
    }
    return result;
}

std::wstring MainWindow::tooltip_for(HWND control)
{
    const app::PlayerView& v = last_view_;
    if (control == open_) return L"";
    if (control == file_label_) return info_in_file_tooltip_ ? W(v.file_tooltip + "\r\n\r\n" + v.file_info_tooltip) : W(v.file_tooltip);
    if (control == info_label_) return W(v.file_info_tooltip);
    if (control == direct_) return W(v.direct_tooltip);   // whether Direct mode is in use, or why not (P12-CHK)
    // v1 shows the loading stage and how to read the two bars on the loading panel and its status text (DD-089).
    if (control == load_status_ || control == progress_overall_ || control == progress_stage_) return v.loading ? W(v.load_tooltip) : L"";
    if (control == output_)
        return L"Choose a Windows MIDI device or None for scheduler-only diagnostics. None processes events and simulator statistics normally but makes no native MIDI calls. Changing output during playback restarts at the current source position and clears backlog/statistics.";
    if (control == kdmapi_)
        return L"Send directly through the application-local or installed KDMAPI provider. While checked, KDMAPI is the active output and the WinMM/None list is inactive. Changing output during playback restarts at the current source position and clears backlog/statistics.";
    if (control == queue_check_) return W(v.queue_check_tooltip);
    if (control == queue_value_) return W(v.queue_value.tooltip);
    if (control == overflow_) return W(v.overflow_tooltip);
    if (control == rate_) return W(v.rate_tooltip);
    if (control == processing_value_) return W(v.processing_value.tooltip);
    if (control == slider_) return W(v.slider_tooltip);
    if (control == din_) return L"Set the standard MIDI DIN rate of 31,250 bit/s.";
    if (control == play_) return W(v.play_tooltip);
    if (control == reset_) return L"Reset counters and maximum baselines without seeking, clearing backlog, or resetting MIDI output.";
    return L"";
}

void MainWindow::update_refresh_timer()
{
    bool need = controller_->needs_periodic_refresh();
    if (need && !timer_running_) {
        if (options_.refresh_ms > 0) SetTimer(window_, kTimerRefresh, static_cast<UINT>(refresh_interval_ms_), nullptr);   // test: the old timer
        else refresh_token_ = DisplayPacer::instance().subscribe(window_);
        refresh_last_qpc_ = 0;
        timer_running_ = true;
        // A new run of live updates: the measuring window starts afresh (the tier is kept: a PC that needed it before
        // will need it again; it steps back up by itself).
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (subscribe_qpc_ == 0) { subscribe_qpc_ = now.QuadPart; tier_since_qpc_ = now.QuadPart; }
        window_start_qpc_ = now.QuadPart;
        meter_window_ = DisplayMeter();
    }
    else if (!need && timer_running_) {
        if (options_.refresh_ms > 0) KillTimer(window_, kTimerRefresh);
        else {
            DisplayPacer::instance().unsubscribe(refresh_token_);
            refresh_token_ = 0;
        }
        timer_running_ = false;
        controller_->refresh();   // final sample
        apply_view(false);
    }
}

// One live update of the main window and the companions (called once per display refresh while playing or loading).
void MainWindow::refresh_tick()
{
    LARGE_INTEGER start = {}, end;
    QueryPerformanceCounter(&start);
    if (qpc_frequency_ <= 1) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        qpc_frequency_ = frequency.QuadPart;
    }
    if (options_.record_refresh) {
        if (refresh_last_qpc_ != 0 && refresh_intervals_us_.size() < 4000000)
            refresh_intervals_us_.push_back(static_cast<int32_t>((start.QuadPart - refresh_last_qpc_) * 1000000 / qpc_frequency_));
        refresh_last_qpc_ = start.QuadPart;
    }
    // Adaptive display rate: in the half-statistics tier each update draws half of the statistics cells (cells 0-3 (the top two rows)
    // on one update, 4-7 (the bottom two rows) on the next); the moving parts (bar, time) are always drawn.
    stats_cells_drawn_ = ladder_.count > 0 && ladder_.half_statistics[tier_state_.tier] ? (stats_phase_ ? 0xF0u : 0x0Fu) : 0xFFu;
    stats_phase_ ^= 1u;
    controller_->refresh();
    apply_view(false);
    stats_cells_drawn_ = 0xFFu;
    if (options_.display_cost_us > 0 && pretend_cost_active()) busy_wait_us(options_.display_cost_us);   // test: pretend the update costs more
    update_refresh_timer();
    show_pending_messages();
    refresh_companions();
    QueryPerformanceCounter(&end);
    meter_total_.tick += end.QuadPart - start.QuadPart;
    meter_window_.tick += end.QuadPart - start.QuadPart;
    ++meter_total_.updates;
    ++meter_window_.updates;
    if (options_.record_refresh) {
        refresh_busy_qpc_ += end.QuadPart - start.QuadPart;
        ++refresh_count_;
    }
    adapt_display(end.QuadPart);
}

void MainWindow::set_statistics_metered(const app::StatisticsText& text)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    StatisticsApi::set_values(stats_, text, stats_cells_drawn_);
    if (options_.display_stats_cost_us > 0 && pretend_cost_active()) {   // test: pretend drawing the statistics costs more, in proportion to the cells drawn
        unsigned drawn = stats_cells_drawn_ & 0xFFu;
        int cells = 0;
        for (int i = 0; i < 8; ++i) cells += (drawn >> i) & 1u;
        if (qpc_frequency_ > 1) busy_wait_us(options_.display_stats_cost_us * cells / 8);
    }
    QueryPerformanceCounter(&b);
    meter_total_.stats_set += b.QuadPart - a.QuadPart;
    meter_window_.stats_set += b.QuadPart - a.QuadPart;
}

bool MainWindow::pretend_cost_active() const
{
    return options_.display_cost_for_ms <= 0 || (play_started_tick_ != 0 && GetTickCount() - play_started_tick_ < static_cast<DWORD>(options_.display_cost_for_ms));
}

void MainWindow::busy_wait_us(int us)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const int64_t ticks = static_cast<int64_t>(us) * qpc_frequency_ / 1000000;
    do QueryPerformanceCounter(&b); while (b.QuadPart - a.QuadPart < ticks);
}

// The message loop reports every dispatched message; only painting is attributed (the updates time themselves).
void MainWindow::note_dispatch(const MSG& message, int64_t ticks)
{
    if (message.message != WM_PAINT) return;
    int64_t* total = message.hwnd == timeline_ ? &meter_total_.paint_timeline : message.hwnd == stats_ ? &meter_total_.paint_stats : &meter_total_.paint_other;
    int64_t* window = message.hwnd == timeline_ ? &meter_window_.paint_timeline : message.hwnd == stats_ ? &meter_window_.paint_stats : &meter_window_.paint_other;
    *total += ticks;
    *window += ticks;
}

void MainWindow::set_display_tier(int tier, int64_t nowQpc, const char* why)
{
    if (tier == tier_state_.tier && tier_since_qpc_ != 0 && ladder_.count > 0) return;
    if (tier_since_qpc_ != 0) tier_time_qpc_[tier_state_.tier] += nowQpc - tier_since_qpc_;
    char line[160];
    std::snprintf(line, sizeof(line), "t=%.2fs tier %d->%d rate=%d half_stats=%d (%s)\n",
        subscribe_qpc_ ? static_cast<double>(nowQpc - subscribe_qpc_) / static_cast<double>(qpc_frequency_) : 0.0, tier_state_.tier, tier,
        app::display_tier_rate(ladder_, tier, tier_native_hz_), ladder_.half_statistics[tier] ? 1 : 0, why);
    tier_log_ += line;
    ++tier_changes_;
    tier_state_.tier = tier;
    tier_since_qpc_ = nowQpc;
    DisplayPacer::instance().set_rate_limit(ladder_.rate_hz[tier]);
}

// Once per half second of live updates: turn what the interface thread spent drawing into a tier (display_tier.h).
void MainWindow::adapt_display(int64_t nowQpc)
{
    if (ladder_.count == 0) {   // first update: the ladder for the screen's rate
        int native = options_.refresh_ms > 0 ? 1000 / std::max(1, options_.refresh_ms) : DisplayPacer::instance().native_hz();
        tier_native_hz_ = native > 0 ? native : 60;
        ladder_ = app::make_display_ladder(tier_native_hz_);
        tier_params_ = app::DisplayTierParams();
        DWORD_PTR processMask = 0, systemMask = 0;
        int processors = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
            for (; processMask; processMask &= processMask - 1) ++processors;
        tier_params_.budget = app::display_budget_for_processors(processors);
        tier_params_.behind_backoff = processors <= 2;
        if (options_.display_budget_permille > 0) tier_params_.budget = options_.display_budget_permille / 1000.0;
        if (subscribe_qpc_ == 0) subscribe_qpc_ = nowQpc;
        if (tier_since_qpc_ == 0) tier_since_qpc_ = nowQpc;
        if (options_.display_fixed_tier >= 0) set_display_tier(std::min(options_.display_fixed_tier, ladder_.count - 1), nowQpc, "fixed by test switch");
    }
    if (nowQpc - window_start_qpc_ < qpc_frequency_ / 2) return;
    const DisplayMeter w = meter_window_;
    meter_window_ = DisplayMeter();
    const int64_t windowTicks = nowQpc - window_start_qpc_;
    window_start_qpc_ = nowQpc;
    if (options_.display_fixed_tier >= 0 || !options_.adaptive_display) return;
    // The screen's rate can only be read after a few frames: rebuild the ladder when it differs by more than 15 %.
    const int native = options_.refresh_ms > 0 ? 1000 / std::max(1, options_.refresh_ms) : DisplayPacer::instance().native_hz();
    if (native > 0 && (native * 100 > tier_native_hz_ * 115 || native * 115 < tier_native_hz_ * 100)) {
        tier_native_hz_ = native;
        ladder_ = app::make_display_ladder(native);
        if (tier_state_.tier >= ladder_.count) tier_state_.tier = ladder_.count - 1;
    }
    app::DisplaySample sample;
    sample.window_us = windowTicks * 1000000 / qpc_frequency_;
    sample.updates = static_cast<int>(w.updates);
    const int64_t statisticsTicks = w.stats_set + w.paint_stats;
    sample.statistics_us = statisticsTicks * 1000000 / qpc_frequency_;
    sample.moving_us = (w.tick - w.stats_set + w.paint_timeline + w.paint_other) * 1000000 / qpc_frequency_;
    // Playback behind: the engine's current lag (how late the last event went out). Read once per window.
    bool behind = tier_params_.behind_backoff &&
        controller_->engine().snapshot().current_lag_microseconds >= tier_params_.behind_lag_us;
    if (options_.display_behind_after_ms >= 0 && play_started_tick_ != 0 &&
        GetTickCount() - play_started_tick_ >= static_cast<DWORD>(options_.display_behind_after_ms) &&
        (options_.display_behind_until_ms < 0 || GetTickCount() - play_started_tick_ < static_cast<DWORD>(options_.display_behind_until_ms)))
        behind = true;
    sample.behind = behind;
    app::DisplayTierState next = app::next_display_tier(ladder_, tier_native_hz_, tier_state_, sample, tier_params_);
    if (next.tier != tier_state_.tier) {
        char why[96];
        std::snprintf(why, sizeof(why), behind ? "playback behind, drawing %.1f%%" : "drawing %.1f%% of the interface thread",
            100.0 * app::display_busy_share(sample));
        const int tier = next.tier;
        next.tier = tier_state_.tier;
        tier_state_ = next;
        set_display_tier(tier, nowQpc, why);
        tier_state_.tier = tier;
    }
    else tier_state_ = next;
}

void MainWindow::show_pending_messages()
{
    std::vector<app::UiMessage> messages = controller_->take_messages();
    for (size_t i = 0; i < messages.size(); ++i)
        show_message(window_, messages[i].title, messages[i].text,
            messages[i].kind == app::UiMessage::Error ? 2 : messages[i].kind == app::UiMessage::Warning ? 1 : 0);
}

void MainWindow::after_action()
{
    controller_->refresh();
    apply_view(false);
    update_refresh_timer();
    show_pending_messages();
    refresh_companions();
    save_settings_if_changed();
}

// The values to remember now: the controller's part plus the window options of the system menu.
app::RememberedSettings MainWindow::current_settings() const
{
    app::RememberedSettings s = controller_->remembered_settings();
    s.always_on_top = always_on_top_;
    s.borderless = borderless_;
    s.borderless_style = style_ == BorderlessStyle::IntegratedHeader ? 1 : style_ == BorderlessStyle::AutoHideOverlay ? 2 : 0;
    s.show_processing = show_processing_;
    s.show_statistics = show_statistics_;
    s.scale_percent = scale_percent_;
    s.few_cpu_yield = few_cpu_stored_;
    return s;
}

// Writes the settings file when a remembered option changed (cheap compare; nothing is written at start). A failure never
// shows a box: the first one is noted once in the file-information area and its tooltip.
void MainWindow::save_settings_if_changed()
{
    if (!settings_store_ || !controller_) return;
    const bool wrote = settings_store_->save_if_changed(current_settings());
    if (wrote && settings_load_note_) {   // a fresh file replaced the unreadable one
        settings_load_note_ = false;
        controller_->set_settings_note(std::string(), std::string());
        controller_->refresh();
        apply_view(false);
    }
    else if (!wrote && !settings_load_note_ && !settings_store_->note().empty() && !settings_store_->read_only()) {
        if (!settings_failure_noted_) {
            settings_failure_noted_ = true;
            controller_->set_settings_note(settings_store_->note().text, settings_store_->note().detail);
            controller_->refresh();
            apply_view(false);
        }
    }
}

void MainWindow::handle_wake()
{
    controller_->pump();
    std::string title, heading, text;
    if (controller_->large_file_decision_pending(title, heading, text)) {
        apply_view(false);
        if (options_.force_large_risk && text.find("32-bit warning") == std::string::npos)
            text += "\r\n\r\n32-bit warning: this MIDI may be too large for the x86 version. Use the x64 version if possible.";
        bool accepted = show_large_file_dialog(window_, ctx_, title, heading, text);
        controller_->answer_large_file(accepted);
    }
    // A loaded file may be played at once (test switch).
    if (test_play_pending_ && controller_->has_song() && !controller_->loading()) {
        test_play_pending_ = false;
        if (options_.select_rate >= 0) controller_->select_rate_choice(rate_index_to_choice(options_.select_rate));
        controller_->select_output(0);   // output None: tests never open a device
        // Fail closed: read the selection back and refuse to play unless the active output is None (and KDMAPI is off).
        if (controller_->output_selection() != 0 || controller_->view().kdmapi_checked) {
            test_play_refusal_ = "play_refused=output is not None\n";
            OutputDebugStringA("midibn-win32: --play refused: the active output is not None\n");
        }
        else {
            controller_->play_clicked();
            play_started_tick_ = GetTickCount();
            if (!options_.thread_report_file.empty())
                SetTimer(window_, kTimerThreadReport, static_cast<UINT>(options_.thread_report_delay_ms), nullptr);
        }
    }
    after_action();
}

void MainWindow::choose_file()
{
    wchar_t path[32768] = L"";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrTitle = L"Open Standard MIDI File";
    dialog.lpstrFilter = L"MIDI files (*.mid;*.midi;*.rmi)\0*.mid;*.midi;*.rmi\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&dialog)) {
        controller_->open_file(U(path));
        after_action();
    }
}

void MainWindow::open_dropped(HDROP drop)
{
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    wchar_t path[32768];
    if (count == 1 && DragQueryFileW(drop, 0, path, static_cast<UINT>(sizeof(path) / sizeof(path[0]))) > 0 && has_midi_extension(path)) {
        DWORD attributes = GetFileAttributesW(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t full[32768];
            DWORD n = GetFullPathNameW(path, static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
            controller_->open_file(U(n > 0 ? std::wstring(full) : std::wstring(path)));
            after_action();
        }
    }
    DragFinish(drop);
}

// ------------------------------------------------------------------ system menu

void MainWindow::build_system_menu()
{
    HMENU menu = GetSystemMenu(window_, FALSE);
    if (!menu) return;
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdAbout, L"About MIDIBottleneck Player\x2026");
    AppendMenuW(menu, MF_STRING, CmdHelp, L"&Help (F1)\x2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdTrackRouting, L"Track &routing\x2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdAlwaysOnTop, L"Always on top");
    AppendMenuW(menu, MF_STRING, CmdBorderless, L"Borderless main window");
    style_menu_ = CreatePopupMenu();
    AppendMenuW(style_menu_, MF_STRING, CmdStyleClassic, L"Classic strip");
    AppendMenuW(style_menu_, MF_STRING, CmdStyleIntegrated, L"Integrated header");
    AppendMenuW(style_menu_, MF_STRING, CmdStyleOverlay, L"Auto-hide overlay");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(style_menu_), L"Borderless style");
    AppendMenuW(menu, MF_STRING, CmdShowProcessing, L"Show &Processing model");
    AppendMenuW(menu, MF_STRING, CmdShowStatistics, L"Show &Statistics");
    scale_menu_ = CreatePopupMenu();
    AppendMenuW(scale_menu_, MF_STRING, CmdScale50, L"50%");
    AppendMenuW(scale_menu_, MF_STRING, CmdScale75, L"75%");
    AppendMenuW(scale_menu_, MF_STRING, CmdScale100, L"100%");
    AppendMenuW(scale_menu_, MF_STRING, CmdScale125, L"125%");
    AppendMenuW(scale_menu_, MF_STRING, CmdScale150, L"150%");
    AppendMenuW(scale_menu_, MF_STRING, CmdScale200, L"200%");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scale_menu_), L"Application &scale");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdChase, L"Chase MIDI state on Play/Seek");
    AppendMenuW(menu, MF_STRING, CmdPreserve, L"Preserve playback queue on Pause and Seek");
    AppendMenuW(menu, MF_STRING, CmdOutputPaced, L"Output-paced timeline");
    if (midibn::win32::few_cpu_yield_option_available())   // only a PC with one physical processor core (DD-112)
        AppendMenuW(menu, MF_STRING, CmdFewCpuYield, L"Let other programs run while playback is behind");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdApplyQueueLimit, L"Apply queue limit without slowdown");
    AppendMenuW(menu, MF_STRING, CmdQueueAge, L"Limit queue by waiting time (\xB5s)");
    AppendMenuW(menu, MF_STRING, CmdScaleInterval, L"Scale note interval by note traffic");
    update_system_menu_states();
}

void MainWindow::update_system_menu_states()
{
    HMENU menu = GetSystemMenu(window_, FALSE);
    if (!menu) return;
    const app::PlayerView& v = controller_->view();
    auto check = [&](UINT id, bool on, bool enabled) {
        CheckMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(menu, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    check(CmdAlwaysOnTop, always_on_top_, true);
    check(CmdBorderless, borderless_, true);
    check(CmdShowProcessing, show_processing_, true);
    check(CmdShowStatistics, show_statistics_, true);
    check(CmdChase, v.menu.chase_checked, v.menu.chase_enabled);
    check(CmdPreserve, v.menu.preserve_checked, v.menu.preserve_enabled);
    check(CmdOutputPaced, v.menu.output_paced_checked, v.menu.output_paced_enabled);
    if (GetMenuState(menu, CmdFewCpuYield, MF_BYCOMMAND) != static_cast<UINT>(-1)) check(CmdFewCpuYield, few_cpu_yield_, true);
    check(CmdApplyQueueLimit, v.menu.apply_queue_limit_checked, v.menu.apply_queue_limit_enabled);
    check(CmdQueueAge, v.menu.queue_age_checked, v.menu.queue_age_enabled);
    check(CmdScaleInterval, v.menu.scale_interval_checked, v.menu.scale_interval_enabled);
    check(CmdTrackRouting, v.menu.track_routing_checked, v.menu.track_routing_enabled);
    if (style_menu_) {
        CheckMenuItem(style_menu_, CmdStyleClassic, MF_BYCOMMAND | (style_ == BorderlessStyle::ClassicStrip ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(style_menu_, CmdStyleIntegrated, MF_BYCOMMAND | (style_ == BorderlessStyle::IntegratedHeader ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(style_menu_, CmdStyleOverlay, MF_BYCOMMAND | (style_ == BorderlessStyle::AutoHideOverlay ? MF_CHECKED : MF_UNCHECKED));
    }
    if (scale_menu_) {
        const UINT ids[6] = { CmdScale50, CmdScale75, CmdScale100, CmdScale125, CmdScale150, CmdScale200 };
        const int percents[6] = { 50, 75, 100, 125, 150, 200 };
        for (int i = 0; i < 6; ++i)
            CheckMenuItem(scale_menu_, ids[i], MF_BYCOMMAND | (percents[i] == scale_percent_ ? MF_CHECKED : MF_UNCHECKED));
    }
}

void MainWindow::toggle_always_on_top()
{
    always_on_top_ = !always_on_top_;
    SetWindowPos(window_, always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::set_section_visible(bool processing, bool visible)
{
    bool& flag = processing ? show_processing_ : show_statistics_;
    if (flag == visible) return;
    HWND group = processing ? group_[1] : group_[3];
    if (!visible) {
        HWND focus = GetFocus();
        if (focus && (focus == group || IsChild(group, focus) || IsOverGroupControl(focus, processing))) SetFocus(open_);
    }
    flag = visible;
    relayout();
    if (visible) after_action();
}

bool MainWindow::IsOverGroupControl(HWND focus, bool processing) const
{
    HWND processingControls[] = { queue_check_, queue_value_, overflow_, rate_, processing_value_, din_, slider_ };
    if (processing) {
        for (size_t i = 0; i < sizeof(processingControls) / sizeof(processingControls[0]); ++i)
            if (processingControls[i] == focus) return true;
        return false;
    }
    return focus == stats_;
}

void MainWindow::set_scale(int percent)
{
    if (percent != 50 && percent != 75 && percent != 100 && percent != 125 && percent != 150 && percent != 200) return;
    if (percent == scale_percent_) return;
    RECT c;
    GetClientRect(window_, &c);
    double canonicalWidth = c.right * 100.0 / ctx_.scale_percent() / (ctx_.dpi() / 96.0);
    ctx_.set_scale(percent);
    scale_percent_ = percent;
    set_all_fonts();
    for (int i = 0; i < 8; ++i) last_rate_labels_[i].clear();
    RECT r;
    GetWindowRect(output_, &r);
    combo_closed_h_ = r.bottom - r.top;
    StatisticsApi::scale_changed(stats_);
    TimelineApi::scale_changed(timeline_);
    help_window_scale_changed(help_window_, ctx_);
    for (size_t i = 0; i < analysis_windows_.size(); ++i) analysis_windows_[i]->on_scale_changed();
    if (channels_window_) channels_window_->on_scale_changed();
    int target = std::max(1, static_cast<int>(std::lround(canonicalWidth * percent / 100.0 * ctx_.dpi() / 96.0)));
    RECT window;
    GetWindowRect(window_, &window);
    int ncW = (window.right - window.left) - c.right;
    SetWindowPos(window_, nullptr, 0, 0, target + ncW, window.bottom - window.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    controller_->refresh();
    apply_view(true);
    relayout();
    InvalidateRect(window_, nullptr, TRUE);
    update_system_menu_states();
}

// ------------------------------------------------------------------ borderless chrome

void MainWindow::set_borderless_style(BorderlessStyle style)
{
    style_ = style;
    if (borderless_) {
        apply_chrome_visibility();
        relayout();
    }
    update_system_menu_states();
}

void MainWindow::set_borderless(bool enabled)
{
    if (borderless_ == enabled) return;
    if (IsZoomed(window_)) ShowWindow(window_, SW_RESTORE);
    RECT client;
    GetClientRect(window_, &client);
    POINT origin = { 0, 0 };
    ClientToScreen(window_, &origin);
    LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
    borderless_ = enabled;
    if (enabled) {
        style = (style & ~static_cast<LONG_PTR>(WS_CAPTION)) | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
        SetWindowLongPtrW(window_, GWL_STYLE, style);
        set_edge_band(std::max(4, ctx_.px(6)));
        SetWindowPos(window_, nullptr, origin.x, origin.y, client.right, client.bottom,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else {
        set_edge_band(0);
        style |= WS_OVERLAPPEDWINDOW;
        SetWindowLongPtrW(window_, GWL_STYLE, style);
        RECT outer = { origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
        AdjustWindowRectEx(&outer, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)));
        SetWindowPos(window_, nullptr, outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    apply_chrome_visibility();
    relayout();
    update_caption_glyphs();
    update_system_menu_states();
}

void MainWindow::apply_chrome_visibility()
{
    bool overlay = borderless_ && style_ == BorderlessStyle::AutoHideOverlay;
    if (overlay) SetTimer(window_, kTimerOverlay, 100, nullptr);
    else {
        KillTimer(window_, kTimerOverlay);
        overlay_visible_ = false;
    }
    // The group heading text goes away in the integrated style.
    bool integrated = borderless_ && style_ == BorderlessStyle::IntegratedHeader;
    SetWindowTextW(group_[0], integrated ? L"" : L"File and output");
    InvalidateRect(window_, nullptr, TRUE);
}

void MainWindow::update_overlay_visibility()
{
    if (!(borderless_ && style_ == BorderlessStyle::AutoHideOverlay)) return;
    POINT p;
    GetCursorPos(&p);
    RECT r;
    GetWindowRect(window_, &r);
    bool inside = PtInRect(&r, p) != FALSE;
    if (inside != overlay_visible_) {
        overlay_visible_ = inside;
        relayout();
    }
}

void MainWindow::update_caption_glyphs()
{
    bool zoomed = IsZoomed(window_) != FALSE;
    for (int s = 0; s < 3; ++s) set_caption_glyph(caption_[s][2], zoomed ? GlyphRestore : GlyphMaximize);
}

void MainWindow::toggle_maximize()
{
    ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
    update_caption_glyphs();
}

void MainWindow::show_system_menu_for_chrome()
{
    HMENU menu = GetSystemMenu(window_, FALSE);
    if (!menu) return;
    update_system_menu_states();
    POINT p = { 0, strip_rect_.bottom };
    if (borderless_ && style_ == BorderlessStyle::IntegratedHeader) p = { file_row_rect_.left, file_row_rect_.bottom };
    else if (borderless_ && style_ == BorderlessStyle::AutoHideOverlay) p = { 0, ctx_.px(34) };
    ClientToScreen(window_, &p);
    UINT command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, window_, nullptr);
    if (command != 0) SendMessageW(window_, WM_SYSCOMMAND, command, 0);
}

int MainWindow::resize_hit(POINT p) const
{
    RECT c;
    GetClientRect(window_, &c);
    int edge = std::max(4, ctx_.px(6));
    bool left = p.x >= 0 && p.x < edge;
    bool right = p.x < c.right && p.x >= c.right - edge;
    bool top = p.y >= 0 && p.y < edge;
    bool bottom = p.y < c.bottom && p.y >= c.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return 0;
}

LRESULT MainWindow::hit_test(LPARAM lParam)
{
    POINT p = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
    ScreenToClient(window_, &p);
    if (!IsZoomed(window_)) {
        int hit = resize_hit(p);
        if (hit != 0) return hit;
    }
    bool dragZone = false;
    if (style_ == BorderlessStyle::ClassicStrip) dragZone = PtInRect(&strip_rect_, p) != FALSE;
    else if (style_ == BorderlessStyle::IntegratedHeader) {
        RECT zone = file_row_rect_;
        zone.left = 0;
        zone.right = INT_MAX / 2;
        dragZone = PtInRect(&zone, p) != FALSE;
    }
    return dragZone ? HTCAPTION : HTCLIENT;
}

// ------------------------------------------------------------------ small windows

int MainWindow::help_topic_for_focus()
{
    HWND focus = GetFocus();
    auto among = [&](std::initializer_list<HWND> list) {
        for (HWND h : list) if (h == focus) return true;
        return false;
    };
    if (focus == analysis_) return static_cast<int>(app::HelpTopic::Analysis);
    if (among({ queue_check_, queue_value_, overflow_ })) return static_cast<int>(app::HelpTopic::QueueAndOverflow);
    if (among({ open_, output_, direct_, kdmapi_ })) return static_cast<int>(app::HelpTopic::FileAndOutput);
    if (among({ rate_, processing_value_, din_, slider_ })) return static_cast<int>(app::HelpTopic::RateModel);
    if (among({ timeline_, play_, stop_, back5_, forward5_, reset_ })) return static_cast<int>(app::HelpTopic::Playback);
    if (focus == stats_) return static_cast<int>(app::HelpTopic::Statistics);
    return static_cast<int>(app::HelpTopic::Overview);
}

void MainWindow::show_help(int topic) { help_window_ = show_help_window(window_, ctx_, help_window_, topic); }

void MainWindow::show_about() { show_about_dialog(window_, ctx_); }

void MainWindow::custom_speed_window()
{
    int64_t value = 0;
    if (show_speed_window_dialog(window_, ctx_, controller_->effective_speed_window(), value)) {
        controller_->set_effective_speed_window(value);
        after_action();
    }
}

void MainWindow::show_speed_menu(POINT screen)
{
    HMENU menu = CreatePopupMenu();
    struct Item { const wchar_t* text; int64_t us; };
    const Item items[5] = { { L"Instantaneous", 0 }, { L"100 ms", 100000 }, { L"250 ms", 250000 }, { L"500 ms", 500000 }, { L"1.5 s", 1500000 } };
    int64_t current = controller_->effective_speed_window();
    for (int i = 0; i < 5; ++i)
        AppendMenuW(menu, MF_STRING | (items[i].us == current ? MF_CHECKED : 0), 100 + i, items[i].text);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 200, L"Custom...");
    int command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
    DestroyMenu(menu);
    if (command >= 100 && command < 105) {
        controller_->set_effective_speed_window(items[command - 100].us);
        after_action();
    }
    else if (command == 200) custom_speed_window();
}

void MainWindow::open_analysis()
{
    if (!controller_->has_song()) return;
    reap_companions();
    std::unique_ptr<CompanionWindow> window = create_analysis_window(host_);
    if (window) analysis_windows_.push_back(std::move(window));
    after_action();
}

void MainWindow::open_channels()
{
    reap_companions();
    if (channels_window_ && IsWindow(channels_window_->hwnd())) {
        if (IsIconic(channels_window_->hwnd())) ShowWindow(channels_window_->hwnd(), SW_RESTORE);
        SetForegroundWindow(channels_window_->hwnd());
        return;
    }
    channels_window_ = create_channels_window(host_);
    after_action();
}

void MainWindow::open_routing()
{
    if (!controller_->track_routing_available()) return;
    show_track_routing_dialog(host_);
    after_action();
}

void MainWindow::reap_companions()
{
    for (size_t i = analysis_windows_.size(); i > 0; --i)
        if (!analysis_windows_[i - 1] || !IsWindow(analysis_windows_[i - 1]->hwnd())) analysis_windows_.erase(analysis_windows_.begin() + static_cast<long>(i - 1));
    if (channels_window_ && !IsWindow(channels_window_->hwnd())) channels_window_.reset();
}

void MainWindow::refresh_companions()
{
    reap_companions();
    for (size_t i = 0; i < analysis_windows_.size(); ++i) analysis_windows_[i]->on_refresh();
    if (channels_window_) channels_window_->on_refresh();
}

void MainWindow::close_companions()
{
    for (size_t i = 0; i < analysis_windows_.size(); ++i) analysis_windows_[i]->close();
    if (channels_window_) channels_window_->close();
    analysis_windows_.clear();
    channels_window_.reset();
}

void MainWindow::take_screenshot(const std::wstring& path)
{
    RECT r;
    GetClientRect(window_, &r);
    int w = r.right, h = r.bottom;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    // Render the client area from the window's own paint code (never the screen): the window's
    // background first, then every visible child from the bottom of the z-order up.
    SendMessageW(window_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    for (HWND child = GetWindow(GetWindow(window_, GW_CHILD), GW_HWNDLAST); child; child = GetWindow(child, GW_HWNDPREV)) {
        if (!own_visible(child)) continue;
        RECT cr;
        GetWindowRect(child, &cr);
        POINT origin = { cr.left, cr.top };
        ScreenToClient(window_, &origin);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        SetViewportOrgEx(dc, 0, 0, nullptr);
    }
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + w * h * 4;
    BITMAPINFOHEADER bih = info.bmiHeader;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, &header, sizeof(header), &written, nullptr);
        WriteFile(file, &bih, sizeof(bih), &written, nullptr);
        WriteFile(file, bits, w * h * 4, &written, nullptr);
        CloseHandle(file);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

bool MainWindow::pre_translate(MSG* message)
{
    if (message->message == WM_KEYDOWN && message->wParam == VK_F1) {
        HWND root = GetAncestor(message->hwnd, GA_ROOT);
        if (root == window_) {
            show_help(help_topic_for_focus());
            return true;
        }
        // P11-HELP: F1 inside the companion windows opens the topic that covers them (v1 had F1 in the main window only).
        // Help and About themselves are left alone.
        if (root && root != help_window_) {
            for (const auto& w : analysis_windows_)
                if (w && w->hwnd() == root) { show_help(static_cast<int>(app::HelpTopic::Analysis)); return true; }
            if (channels_window_ && channels_window_->hwnd() == root) { show_help(static_cast<int>(app::HelpTopic::Channels)); return true; }
        }
    }
    if (message->message == WM_SYSKEYDOWN && message->wParam == VK_SPACE && borderless_ && GetAncestor(message->hwnd, GA_ROOT) == window_) {
        show_system_menu_for_chrome();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ the window procedure

LRESULT MainWindow::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == DisplayPacer::message()) {
        // One display-paced live update. A message posted just before the subscription ended is ignored.
        if (timer_running_ && static_cast<int>(wParam) == refresh_token_ && refresh_token_ != 0) refresh_tick();
        DisplayPacer::instance().finished(static_cast<int>(wParam));
        return 0;
    }
    switch (message) {
    case WM_MIDIBN_HELP:   // posted by the Track routing editor's own message loop when F1 is pressed there
        show_help(static_cast<int>(wParam));
        return 0;
    case WM_MIDIBN_ACCESSIBLE_NAMES:
        flush_accessible_names();
        return 0;
    case WM_CREATE:
        controller_culture_ = system_number_culture();
        build_controls();
        build_system_menu();
        DragAcceptFiles(window_, TRUE);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            relayout();
            update_caption_glyphs();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT window, client;
        GetWindowRect(window_, &window);
        GetClientRect(window_, &client);
        int ncW = (window.right - window.left) - client.right;
        info->ptMinTrackSize.x = ctx_.px(416) + std::max(0, ncW);
        if (required_outer_height_ > 0) {
            info->ptMinTrackSize.y = required_outer_height_;
            info->ptMaxTrackSize.y = required_outer_height_;
            info->ptMaxSize.y = required_outer_height_;
        }
        info->ptMaxTrackSize.x = 10000;
        if (borderless_) {
            HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(monitor, &mi)) {
                info->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                info->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                info->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            }
        }
        return 0;
    }
    case WM_NCCALCSIZE:
        if (borderless_) return 0;   // the whole window is client area; resizing is by hit-testing
        break;
    case WM_NCHITTEST:
        if (borderless_) return hit_test(lParam);
        break;
    case WM_NCACTIVATE:
        if (borderless_) return TRUE;
        break;
    case WM_ERASEBKGND:
        break;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC dc = message == WM_PAINT ? BeginPaint(window_, &ps) : reinterpret_cast<HDC>(wParam);
        RECT client;
        GetClientRect(window_, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        if (borderless_ && style_ == BorderlessStyle::ClassicStrip && strip_rect_.bottom > 0) {
            FillRect(dc, &strip_rect_, reinterpret_cast<HBRUSH>(COLOR_BTNSHADOW + 1));
            RECT text = strip_rect_;
            text.left += ctx_.px(42) + ctx_.px(6);
            text.right -= 3 * ctx_.px(42);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            HGDIOBJ old = SelectObject(dc, ctx_.ui_font());
            DrawTextW(dc, kProductName, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, old);
        }
        if (message == WM_PAINT) EndPaint(window_, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HWND control = reinterpret_cast<HWND>(lParam);
        if (control == queue_value_ || control == processing_value_) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            bool enabled = IsWindowEnabled(control) != FALSE;
            SetTextColor(dc, enabled ? GetSysColor(COLOR_WINDOWTEXT) : ctx_.disabled_text_color());
            SetBkColor(dc, scrub_field_back_color(enabled));
            return reinterpret_cast<LRESULT>(scrub_field_back_brush(enabled));
        }
        // Group frames, check boxes and other static-looking controls ask their parent for a background. A plain
        // window's default answer is white, which showed as white gaps between controls inside every group frame
        // (seen by the user on screen). Answer with the window's own face colour, as a dialog does.
        if (message == WM_CTLCOLOREDIT) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_MEASUREITEM:
        if (rate_popup_measure(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam), &ctx_)) return TRUE;
        break;
    case WM_DRAWITEM:
        if (rate_popup_draw(reinterpret_cast<DRAWITEMSTRUCT*>(lParam), &ctx_)) return TRUE;
        break;
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (tooltips_.handle_notify(header, [this](HWND control) { return tooltip_for(control); })) return 0;
        break;
    }
    case WM_INITMENUPOPUP:
        if (HIWORD(lParam)) update_system_menu_states();
        break;
    case WM_TIMER:
        if (wParam == kTimerRefresh) refresh_tick();
        else if (wParam == kTimerAutoClose) {
            KillTimer(window_, kTimerAutoClose);
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        else if (wParam == kTimerOverlay) update_overlay_visibility();
        else if (wParam == kTimerPopupShot) {
            // Test-only: open the Rate model popup, copy its window to a BMP, close it.
            HWND menu = FindWindowW(L"#32768", nullptr);
            if (menu == nullptr) {
                PostMessageW(rate_, CB_SHOWDROPDOWN, TRUE, 0);
            }
            else {
                KillTimer(window_, kTimerPopupShot);
                RECT r;
                GetWindowRect(menu, &r);
                int w = r.right - r.left, h = r.bottom - r.top;
                HDC screen = GetDC(nullptr);
                HDC dc = CreateCompatibleDC(screen);
                BITMAPINFO info = {};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = w;
                info.bmiHeader.biHeight = -h;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                HGDIOBJ old = SelectObject(dc, bitmap);
                PrintWindow(menu, dc, 0);
                BITMAPFILEHEADER header = {};
                header.bfType = 0x4D42;
                header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                header.bfSize = header.bfOffBits + w * h * 4;
                HANDLE file = CreateFileW(options_.popup_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD written;
                    WriteFile(file, &header, sizeof(header), &written, nullptr);
                    WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
                    WriteFile(file, bits, w * h * 4, &written, nullptr);
                    CloseHandle(file);
                }
                SelectObject(dc, old);
                DeleteObject(bitmap);
                DeleteDC(dc);
                ReleaseDC(nullptr, screen);
                EndMenu();
                PostMessageW(window_, WM_CLOSE, 0, 0);
            }
        }
        else if (wParam == kTimerTestDialog) {
            // Self-test only: answer the large-file warning when it appears.
            if (test_dialog_answer_ == 3) {
                // Self-test: dismiss the Track routing editor (Cancel through the close box).
                HWND routing = FindWindowW(nullptr, L"Track routing \x2014 MIDIBottleneck Player");
                if (routing) { test_dialog_seen_ = true; PostMessageW(routing, WM_CLOSE, 0, 0); }
                return 0;
            }
            HWND dialog = FindWindowW(L"MidiBnDialog", L"Large MIDI memory warning");
            if (dialog && test_dialog_answer_ != 0) {
                test_dialog_seen_ = true;
                PostMessageW(dialog, WM_COMMAND, test_dialog_answer_ == 1 ? 1 : 2, 0);
            }
        }
        else if (wParam == kTimerThreadReport) {
            KillTimer(window_, kTimerThreadReport);
            write_thread_report(options_.thread_report_file);
        }
        else if (wParam == kTimerScreenshot) {
            KillTimer(window_, kTimerScreenshot);
            take_screenshot(options_.screenshot_file);
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_MIDIBN_WAKE:
        handle_wake();
        return 0;
    case WM_MIDIBN_FIELD: {
        int64_t value = *reinterpret_cast<int64_t*>(lParam);
        if (wParam == IdQueueValue) controller_->set_queue_value(value);
        else if (wParam == IdProcessingValue) controller_->set_processing_value(value);
        after_action();
        return 0;
    }
    case WM_MIDIBN_SEEK:
        controller_->seek_to(*reinterpret_cast<int64_t*>(lParam));
        after_action();
        return 0;
    case WM_MIDIBN_DRAGGING:
        controller_->set_timeline_dragging(wParam != 0);
        return 0;
    case WM_MIDIBN_SPEEDMENU: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        show_speed_menu(p);
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == slider_) {
            controller_->set_slider(static_cast<int>(SendMessageW(slider_, TBM_GETPOS, 0, 0)));
            after_action();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (applying_view_) return 0;
        switch (id) {
        case IdOpen: {
            bool wantsDialog = false;
            controller_->open_button_clicked(wantsDialog);
            after_action();
            if (wantsDialog) choose_file();
            return 0;
        }
        case IdPlay: controller_->play_clicked(); after_action(); return 0;
        case IdStop: controller_->stop_clicked(); after_action(); return 0;
        case IdBack5: controller_->seek_by(-5000000); after_action(); return 0;
        case IdForward5: controller_->seek_by(5000000); after_action(); return 0;
        case IdAnalysis: open_analysis(); return 0;
        case IdReset: controller_->reset_statistics_clicked(); after_action(); return 0;
        case IdDin: controller_->din_preset(); after_action(); return 0;
        case IdDirect:
            // Ticking it does exactly what the old system-menu item did: the controller flips the session-only choice.
            // The box's own tick is then set from the controller's state (a click on a greyed box never arrives).
            if (code == BN_CLICKED) {
                controller_->toggle_direct_winmm();
                after_action();
            }
            return 0;
        case IdKdmapi:
            if (code == BN_CLICKED) {
                controller_->set_kdmapi(SendMessageW(kdmapi_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                after_action();
            }
            return 0;
        case IdQueueCheck:
            if (code == BN_CLICKED) {
                controller_->set_queue_limit_checked(SendMessageW(queue_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                after_action();
            }
            return 0;
        case IdOutput:
            if (code == CBN_SELCHANGE) {
                controller_->select_output(static_cast<int>(SendMessageW(output_, CB_GETCURSEL, 0, 0)));
                after_action();
            }
            return 0;
        case IdOverflow:
            if (code == CBN_SELCHANGE) {
                controller_->set_overflow_selection(static_cast<int>(SendMessageW(overflow_, CB_GETCURSEL, 0, 0)));
                after_action();
            }
            return 0;
        case IdRate:
            if (code == CBN_SELCHANGE) {
                controller_->select_rate_choice(rate_index_to_choice(static_cast<int>(SendMessageW(rate_, CB_GETCURSEL, 0, 0))));
                after_action();
            }
            return 0;
        default:
            if (id >= IdCaptionBase && id < IdCaptionBase + 30 && code == BN_CLICKED) {
                int kind = id % 10;
                if (kind == 0) show_system_menu_for_chrome();
                else if (kind == 1) ShowWindow(window_, SW_MINIMIZE);
                else if (kind == 2) toggle_maximize();
                else if (kind == 3) PostMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        }
        break;
    }
    case WM_SYSCOMMAND: {
        UINT command = static_cast<UINT>(wParam) & 0xFFF0;
        switch (command) {
        case CmdAbout: show_about(); return 0;
        case CmdHelp: show_help(help_topic_for_focus()); return 0;
        case CmdTrackRouting: open_routing(); return 0;
        case CmdAlwaysOnTop: toggle_always_on_top(); return 0;
        case CmdBorderless: set_borderless(!borderless_); return 0;
        case CmdStyleClassic: set_borderless_style(BorderlessStyle::ClassicStrip); return 0;
        case CmdStyleIntegrated: set_borderless_style(BorderlessStyle::IntegratedHeader); return 0;
        case CmdStyleOverlay: set_borderless_style(BorderlessStyle::AutoHideOverlay); return 0;
        case CmdApplyQueueLimit: controller_->toggle_apply_queue_limit_without_slowdown(); after_action(); return 0;
        case CmdQueueAge: controller_->toggle_queue_age_limit(); after_action(); return 0;
        case CmdChase: controller_->toggle_state_chase(); after_action(); return 0;
        case CmdPreserve: controller_->toggle_preserve_pause_seek(); after_action(); return 0;
        case CmdOutputPaced: controller_->toggle_output_paced_timeline(); after_action(); return 0;
        case CmdFewCpuYield:
            few_cpu_yield_ = !few_cpu_yield_;
            few_cpu_stored_ = few_cpu_yield_ ? 1 : 0;   // from now on the user's choice wins over the automatic default
            controller_->engine().set_behind_yield_microseconds(few_cpu_yield_ ? midibn::win32::kFewCpuYieldOptionMicroseconds : 0);
            update_system_menu_states();
            after_action();
            return 0;
        case CmdScaleInterval: controller_->toggle_scale_note_interval_by_traffic(); after_action(); return 0;
        case CmdShowProcessing: set_section_visible(true, !show_processing_); return 0;
        case CmdShowStatistics: set_section_visible(false, !show_statistics_); return 0;
        case CmdScale50: set_scale(50); return 0;
        case CmdScale75: set_scale(75); return 0;
        case CmdScale100: set_scale(100); return 0;
        case CmdScale125: set_scale(125); return 0;
        case CmdScale150: set_scale(150); return 0;
        case CmdScale200: set_scale(200); return 0;
        }
        break;
    }
    case WM_DROPFILES:
        open_dropped(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_CLOSE:
        if (report_text_.empty()) report_text_ = compute_report();
        close_companions();
        if (!controller_->shutdown()) {
            show_pending_messages();
            return 0;   // the output is still stopping; try again later
        }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        if (help_window_ && IsWindow(help_window_)) DestroyWindow(help_window_);
        if (refresh_token_ != 0) {
            DisplayPacer::instance().unsubscribe(refresh_token_);
            refresh_token_ = 0;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace ui
} // namespace midibn
