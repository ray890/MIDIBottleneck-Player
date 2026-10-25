#include "dialogs.h"

#include "midibn/app/help_text.h"

#include <commctrl.h>
#include <richedit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace midibn {
namespace ui {

namespace {

const wchar_t kDialogClass[] = L"MidiBnDialog";
const wchar_t kHelpClass[] = L"MidiBnHelp";
const wchar_t kProductName[] = L"MIDIBottleneck Player";

enum { IdOk = 1, IdCancel = 2, IdEdit = 10, IdSpin = 11, IdList = 20, IdBody = 21 };

struct DialogState {
    UiContext* ctx = nullptr;
    bool done = false;
    int result = 0;
    HWND edit = nullptr;
    HWND ok = nullptr;
    int64_t minimum_us = 25000, maximum_us = 10000000;
    int64_t value_us = 0;
};

void set_font(HWND window, HFONT font) { SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }

HWND child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font,
    DWORD exStyle = 0)
{
    HWND window = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    set_font(window, font);
    return window;
}

std::wstring edit_text(HWND edit)
{
    int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

LRESULT CALLBACK dialog_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        break;
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IdOk || LOWORD(wParam) == IdCancel) {
            if (LOWORD(wParam) == IdOk && state->edit != nullptr) {
                // Speed window: validate "Measurement window" (ms, 25..10000).
                std::wstring text = edit_text(state->edit);
                wchar_t* end = nullptr;
                wchar_t decimal[8] = L".";
                GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, decimal, 8);
                for (size_t i = 0; i < text.size(); ++i)
                    if (decimal[0] != L'.' && text[i] == decimal[0]) text[i] = L'.';
                double value = wcstod(text.c_str(), &end);
                if (end == text.c_str() || *end != L'\0' || value < 25.0 || value > 10000.0) {
                    MessageBoxW(window, L"Enter a measurement window from 25 through 10,000 ms.",
                        L"Effective playback speed window", MB_OK | MB_ICONINFORMATION);
                    SetFocus(state->edit);
                    return 0;
                }
                state->value_us = static_cast<int64_t>(std::llround(value * 1000.0));
            }
            state->result = LOWORD(wParam) == IdOk ? 1 : 0;
            state->done = true;
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (state && header->idFrom == IdSpin && header->code == UDN_DELTAPOS) {
            NMUPDOWN* delta = reinterpret_cast<NMUPDOWN*>(lParam);
            std::wstring text = edit_text(state->edit);
            wchar_t* end = nullptr;
            double value = wcstod(text.c_str(), &end);
            if (end == text.c_str()) value = 250.0;
            value += (delta->iDelta < 0 ? 25.0 : -25.0);
            value = std::max(25.0, std::min(10000.0, value));
            wchar_t formatted[64];
            swprintf_s(formatted, L"%.3f", value);
            SetWindowTextW(state->edit, formatted);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        if (state) { state->result = 0; state->done = true; }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int run_modal(HWND owner, HWND dialog, DialogState& state)
{
    bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG message;
    while (!state.done) {
        BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) { if (got == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner && ownerWasEnabled) EnableWindow(owner, TRUE);
    DestroyWindow(dialog);
    if (owner) SetForegroundWindow(owner);
    return state.result;
}

const DWORD kDialogStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

// DD-099: the client width at which this dialog's caption shows its whole title.
int title_fit_width(const std::wstring& title) { return caption_fit_client_width(title, kDialogStyle, WS_EX_DLGMODALFRAME); }

HWND make_dialog(HWND owner, const std::wstring& title, int clientW, int clientH, DialogState* state, UiContext& ctx)
{
    (void)ctx;
    DWORD style = kDialogStyle;
    RECT rect = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);
    // v1 CenterParent: centred over the owner, inside the work area of the owner's monitor (DD-098; it used to be
    // centred without a monitor check and only kept right of and below the point 0,0).
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    POINT at = centre_on_owner(owner, width, height);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kDialogClass, title.c_str(), style, at.x, at.y, width, height, owner, nullptr,
        GetModuleHandleW(nullptr), state);
    return dialog;
}

int measure_wrapped(UiContext& ctx, HFONT font, const std::wstring& text, int width)
{
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect = { 0, 0, width, 0 };
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    (void)ctx;
    return rect.bottom - rect.top;
}

} // namespace

void register_dialog_classes(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = dialog_proc;
    wc.lpszClassName = kDialogClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
}

void show_about_dialog(HWND owner, UiContext& ctx)
{
    DialogState state;
    state.ctx = &ctx;
    const wchar_t* version = L"Release 2.0.0  \x2022  version 2.0.0.0";
    const wchar_t* tagline = L"A MIDI workload, queue, timing, and bottleneck laboratory.";
    // DD-099: at least as wide as its longest line (at Application scale 50 % the font shrinks less than the layout,
    // and the last line wrapped onto a second line that was cut off).
    int textW = std::max(ctx.text_width(ctx.ui_bold_font(), kProductName),
        std::max(ctx.text_width(ctx.ui_font(), version), ctx.text_width(ctx.ui_font(), tagline))) + 4;
    std::wstring title = std::wstring(L"About ") + kProductName;
    int w = std::max(std::max(ctx.px(390), textW + ctx.px(36)), title_fit_width(title)), h = ctx.px(165);
    HWND dialog = make_dialog(owner, title, w, h, &state, ctx);
    HWND name = child(dialog, L"STATIC", kProductName, SS_LEFT | SS_NOPREFIX, ctx.px(18), ctx.px(18), w - ctx.px(36), ctx.font_height() + 4, 0,
        ctx.ui_bold_font());
    (void)name;
    child(dialog, L"STATIC", version, SS_LEFT | SS_NOPREFIX, ctx.px(18), ctx.px(50), w - ctx.px(36), ctx.font_height() + 4, 0, ctx.ui_font());
    child(dialog, L"STATIC", tagline, SS_LEFT | SS_NOPREFIX, ctx.px(18), ctx.px(78), w - ctx.px(36), ctx.font_height() + 4, 0, ctx.ui_font());
    HWND ok = child(dialog, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, w - ctx.px(94), h - ctx.px(44), ctx.px(75), ctx.font_height() + ctx.px(8),
        IdOk, ctx.ui_font());
    SetFocus(ok);
    run_modal(owner, dialog, state);
}

bool show_large_file_dialog(HWND owner, UiContext& ctx, const std::string& title, const std::string& heading, const std::string& text)
{
    DialogState state;
    state.ctx = &ctx;
    int pad = ctx.px(16);
    int textWidth = ctx.px(540);
    std::wstring wideHeading = W(heading), wideText = W(text);
    int headingH = measure_wrapped(ctx, ctx.ui_bold_font(), wideHeading, textWidth);
    // DD-105: the message is laid out line by line. The estimated memory while the MIDI is open goes on its own
    // line in bold (a line break after the colon), and the 32-bit warning line (x86 build) is bold as a whole line.
    struct Row { std::wstring text; bool bold; };
    std::vector<Row> rows;
    {
        const std::wstring memoryLabel = L"Estimated memory while the MIDI is open: ";
        size_t at = 0;
        while (at <= wideText.size()) {
            size_t next = wideText.find(L'\n', at);
            std::wstring line = wideText.substr(at, next == std::wstring::npos ? std::wstring::npos : next - at);
            if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);
            Row row;
            if (line.compare(0, memoryLabel.size(), memoryLabel) == 0) {
                row.text = memoryLabel.substr(0, memoryLabel.size() - 1);
                row.bold = false;
                rows.push_back(row);
                row.text = line.substr(memoryLabel.size());
                row.bold = true;
                rows.push_back(row);
            }
            else {
                row.text = line;
                row.bold = line.compare(0, 11, L"32-bit warn") == 0;
                rows.push_back(row);
            }
            if (next == std::wstring::npos) break;
            at = next + 1;
        }
    }
    std::vector<int> heights;
    int textH = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        int rh = rows[i].text.empty() ? ctx.font_height()
            : measure_wrapped(ctx, rows[i].bold ? ctx.ui_bold_font() : ctx.ui_font(), rows[i].text, textWidth);
        heights.push_back(rh);
        textH += rh;
    }
    int buttonH = ctx.font_height() + ctx.px(8);
    int w = std::max(textWidth + 2 * pad, title_fit_width(W(title)));   // DD-099
    int h = pad + headingH + ctx.px(12) + textH + ctx.px(14) + buttonH + pad;
    HWND dialog = make_dialog(owner, W(title), w, h, &state, ctx);
    child(dialog, L"STATIC", wideHeading.c_str(), SS_LEFT | SS_NOPREFIX, pad, pad, textWidth, headingH, 0, ctx.ui_bold_font());
    int ry = pad + headingH + ctx.px(12);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].text.empty())
            child(dialog, L"STATIC", rows[i].text.c_str(), SS_LEFT | SS_NOPREFIX, pad, ry, textWidth, heights[i], 0,
                rows[i].bold ? ctx.ui_bold_font() : ctx.ui_font());
        ry += heights[i];
    }
    int by = h - pad - buttonH;
    int bw = ctx.px(80);
    HWND ok = child(dialog, L"BUTTON", L"Continue", BS_DEFPUSHBUTTON | WS_TABSTOP, pad, by, bw, buttonH, IdOk, ctx.ui_font());
    child(dialog, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, pad + bw + ctx.px(8), by, bw, buttonH, IdCancel, ctx.ui_font());
    SetFocus(ok);
    return run_modal(owner, dialog, state) == 1;
}

bool show_speed_window_dialog(HWND owner, UiContext& ctx, int64_t currentMicroseconds, int64_t& newMicroseconds)
{
    DialogState state;
    state.ctx = &ctx;
    // DD-099: the label is at least as wide as its text, and what follows it moves right by the difference (at
    // Application scale 50 % the font shrinks less than the layout, and the label wrapped onto a cut-off second line).
    const wchar_t* label = L"Measurement window:";
    int labelW = std::max(ctx.px(130), ctx.text_width(label) + 4);
    int shift = labelW - ctx.px(130);
    const wchar_t* title = L"Effective playback speed window";
    int extra = std::max(0, title_fit_width(title) - (ctx.px(330) + shift));   // OK and Cancel stay at the right edge
    int w = ctx.px(330) + shift + extra, h = ctx.px(105);
    HWND dialog = make_dialog(owner, title, w, h, &state, ctx);
    int fh = ctx.font_height();
    child(dialog, L"STATIC", label, SS_LEFT | SS_NOPREFIX, ctx.px(12), ctx.px(18), labelW, fh + 4, 0, ctx.ui_font());
    double ms = (currentMicroseconds == 0 ? 250000 : currentMicroseconds) / 1000.0;
    ms = std::max(25.0, std::min(10000.0, ms));
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.3f", ms);
    state.edit = child(dialog, L"EDIT", buffer, ES_AUTOHSCROLL | WS_TABSTOP, ctx.px(145) + shift, ctx.px(15), ctx.px(105), fh + ctx.px(6), IdEdit,
        ctx.ui_font(), WS_EX_CLIENTEDGE);
    HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSpin)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(state.edit), 0);
    child(dialog, L"STATIC", L"ms", SS_LEFT | SS_NOPREFIX, ctx.px(256) + shift, ctx.px(18), ctx.px(30), fh + 4, 0, ctx.ui_font());
    int bh = fh + ctx.px(8);
    HWND ok = child(dialog, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, ctx.px(164) + shift + extra, h - bh - ctx.px(14), ctx.px(75), bh, IdOk,
        ctx.ui_font());
    child(dialog, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ctx.px(245) + shift + extra, h - bh - ctx.px(14), ctx.px(75), bh, IdCancel,
        ctx.ui_font());
    (void)ok;
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);
    if (run_modal(owner, dialog, state) != 1) return false;
    newMicroseconds = state.value_us;
    return true;
}

// ------------------------------------------------------------------ Help

namespace {

struct HelpState {
    UiContext* ctx = nullptr;
    HWND list = nullptr, body = nullptr, close = nullptr;
    bool rich = false;
};

int twips(const UiContext& ctx, int canonicalPx)
{
    return static_cast<int>((static_cast<long long>(ctx.px(canonicalPx)) * 1440) / std::max(1, ctx.dpi()));
}

// Draws Help topic `index` into the body: headings, bold words and bullet / numbered lists as rich text. The text is the
// portable markup (app::parse_help_markup); sizes come from the scaled UI font, so they follow the Application scale.
// Falls back to plain text if the rich edit control is not available.
void help_render(HelpState* h, int index)
{
    if (index < 0 || index >= app::kHelpTopicCount) index = 0;
    const std::vector<app::HelpBlock> blocks = app::parse_help_markup(app::help_text(static_cast<app::HelpTopic>(index)));

    struct Piece { LONG start, end; };
    struct Para { LONG start, end; app::HelpBlockKind kind; };
    std::wstring text;
    std::vector<Piece> bolds;
    std::vector<Para> paras;
    for (size_t b = 0; b < blocks.size(); ++b) {
        const app::HelpBlock& block = blocks[b];
        if (b > 0) text += h->rich ? L"\r" : L"\r\n";
        if (!h->rich && b > 0 && (block.kind == app::HelpBlockKind::Heading || block.kind == app::HelpBlockKind::Subheading)) text += L"\r\n";
        LONG start = static_cast<LONG>(text.size());
        if (block.kind == app::HelpBlockKind::Bullet) text += h->rich ? L"\x2022\t" : L"\x2022 ";
        if (block.kind == app::HelpBlockKind::Step) text += std::to_wstring(block.number) + (h->rich ? L".\t" : L". ");
        for (size_t r = 0; r < block.runs.size(); ++r) {
            LONG from = static_cast<LONG>(text.size());
            text += W(block.runs[r].text);
            if (block.runs[r].bold) bolds.push_back({ from, static_cast<LONG>(text.size()) });
        }
        paras.push_back({ start, static_cast<LONG>(text.size()), block.kind });
    }

    if (!h->rich) {
        SetWindowTextW(h->body, text.c_str());
        SendMessageW(h->body, EM_SETSEL, 0, 0);
        return;
    }

    LOGFONTW lf = {};
    GetObjectW(h->ctx->ui_font(), sizeof(lf), &lf);
    const LONG baseTwips = static_cast<LONG>((static_cast<long long>(std::abs(static_cast<long>(lf.lfHeight))) * 1440) / std::max(1, h->ctx->dpi()));

    SendMessageW(h->body, WM_SETREDRAW, FALSE, 0);
    SendMessageW(h->body, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(GetSysColor(COLOR_WINDOW)));
    SetWindowTextW(h->body, text.c_str());

    CHARFORMAT2W base = {};
    base.cbSize = sizeof(base);
    base.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_COLOR | CFM_UNDERLINE;
    base.dwEffects = 0;                       // not bold, not italic, and no automatic colour
    base.yHeight = baseTwips;
    base.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
    wcsncpy_s(base.szFaceName, lf.lfFaceName, _TRUNCATE);
    SendMessageW(h->body, EM_SETSEL, 0, -1);
    SendMessageW(h->body, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

    for (size_t i = 0; i < paras.size(); ++i) {
        const Para& p = paras[i];
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT | PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_TABSTOPS;
        pf.dxRightIndent = twips(*h->ctx, 8);
        pf.dySpaceAfter = twips(*h->ctx, 3);
        pf.cTabCount = 1;
        pf.rgxTabs[0] = twips(*h->ctx, 34);
        pf.dxStartIndent = twips(*h->ctx, 8);
        CHARFORMAT2W cf = base;
        cf.dwMask = CFM_SIZE | CFM_BOLD | CFM_COLOR;
        switch (p.kind) {
        case app::HelpBlockKind::Heading:
            cf.yHeight = baseTwips * 3 / 2;
            cf.dwEffects = CFE_BOLD;
            cf.crTextColor = RGB(0, 51, 128);
            pf.dySpaceAfter = twips(*h->ctx, 6);
            break;
        case app::HelpBlockKind::Subheading:
            cf.yHeight = baseTwips * 6 / 5;
            cf.dwEffects = CFE_BOLD;
            cf.crTextColor = RGB(0, 51, 128);
            pf.dySpaceBefore = twips(*h->ctx, 10);
            break;
        case app::HelpBlockKind::Bullet:
        case app::HelpBlockKind::Step:
            pf.dxStartIndent = twips(*h->ctx, 14);
            pf.dxOffset = twips(*h->ctx, 20);
            pf.dySpaceAfter = twips(*h->ctx, 2);
            break;
        case app::HelpBlockKind::Paragraph:
            pf.dySpaceBefore = twips(*h->ctx, 2);
            pf.dySpaceAfter = twips(*h->ctx, 4);
            break;
        }
        SendMessageW(h->body, EM_SETSEL, p.start, p.end);
        SendMessageW(h->body, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
        SendMessageW(h->body, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }
    for (size_t i = 0; i < bolds.size(); ++i) {
        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_BOLD;
        cf.dwEffects = CFE_BOLD;
        SendMessageW(h->body, EM_SETSEL, bolds[i].start, bolds[i].end);
        SendMessageW(h->body, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }
    SendMessageW(h->body, EM_SETSEL, 0, 0);
    SendMessageW(h->body, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(h->body, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h->body, nullptr, TRUE);
}

void help_layout(HWND window, HelpState* h)
{
    RECT c;
    GetClientRect(window, &c);
    int pad = h->ctx->px(10);
    int listW = h->ctx->px(155);
    int buttonH = h->ctx->font_height() + h->ctx->px(8);
    int buttonW = h->ctx->px(75);
    MoveWindow(h->list, pad, pad, listW, c.bottom - 2 * pad, TRUE);
    MoveWindow(h->body, pad + listW + pad, pad, c.right - 3 * pad - listW, c.bottom - 3 * pad - buttonH, TRUE);
    MoveWindow(h->close, c.right - pad - buttonW, c.bottom - pad - buttonH, buttonW, buttonH, TRUE);
}

void help_select(HelpState* h)
{
    int index = static_cast<int>(SendMessageW(h->list, LB_GETCURSEL, 0, 0));
    if (index < 0) index = 0;
    help_render(h, index);
}

LRESULT CALLBACK help_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    HelpState* h = reinterpret_cast<HelpState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        break;
    case WM_SIZE:
        if (h && h->list) help_layout(window, h);
        return 0;
    case WM_COMMAND:
        if (!h) break;
        if (LOWORD(wParam) == IdList && HIWORD(wParam) == LBN_SELCHANGE) { help_select(h); return 0; }
        if (LOWORD(wParam) == IdCancel || LOWORD(wParam) == IdOk) { DestroyWindow(window); return 0; }
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (h) {
            RECT r = { 0, 0, h->ctx->px(420), h->ctx->px(300) };
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
            info->ptMinTrackSize.x = r.right - r.left;
            info->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        break;
    case WM_NCDESTROY:
        delete h;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool g_help_registered = false;

} // namespace

HWND show_help_window(HWND owner, UiContext& ctx, HWND existing, int topic)
{
    if (existing && IsWindow(existing)) {
        HelpState* h = reinterpret_cast<HelpState*>(GetWindowLongPtrW(existing, GWLP_USERDATA));
        if (h) {
            SendMessageW(h->list, LB_SETCURSEL, static_cast<WPARAM>(topic), 0);
            help_select(h);
        }
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return existing;
    }
    if (!g_help_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = help_proc;
        wc.lpszClassName = kHelpClass;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        g_help_registered = true;
    }
    HelpState* h = new HelpState();
    h->ctx = &ctx;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect = { 0, 0, ctx.px(640), ctx.px(390) };
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    HWND window = CreateWindowExW(0, kHelpClass, (std::wstring(kProductName) + L" Help").c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, GetModuleHandleW(nullptr), h);
    h->list = child(window, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | WS_BORDER, 0, 0, 10, 10, IdList, ctx.ui_font());
    for (int i = 0; i < 8; ++i)
        SendMessageW(h->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(app::help_topic_title(static_cast<app::HelpTopic>(i))).c_str()));
    // Rich text (Riched20.dll, present on Windows XP and later); if it cannot be loaded the body is a plain edit box.
    static HMODULE richModule = LoadLibraryW(L"Riched20.dll");
    const DWORD bodyStyle = ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP | WS_BORDER;
    if (richModule) h->body = child(window, L"RichEdit20W", L"", bodyStyle, 0, 0, 10, 10, IdBody, ctx.ui_font());
    h->rich = h->body != nullptr;
    if (!h->body) h->body = child(window, L"EDIT", L"", bodyStyle, 0, 0, 10, 10, IdBody, ctx.ui_font());
    h->close = child(window, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, IdCancel, ctx.ui_font());
    SendMessageW(h->list, LB_SETCURSEL, static_cast<WPARAM>(topic), 0);
    help_select(h);
    help_layout(window, h);
    // v1 CenterParent, at most the size of the work area of its screen (OnShown, Screen.FromControl): centred over the
    // owner inside the work area of the owner's monitor (DD-098; it used the primary monitor's work area, which pulled
    // the window onto the primary monitor when the main window was on another one).
    RECT work = monitor_work_area(owner);
    RECT outer;
    GetWindowRect(window, &outer);
    int width = std::min<LONG>(outer.right - outer.left, work.right - work.left);
    int height = std::min<LONG>(outer.bottom - outer.top, work.bottom - work.top);
    POINT at = centre_on_owner(owner, width, height);
    SetWindowPos(window, nullptr, at.x, at.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOW);
    return window;
}

void help_window_scale_changed(HWND help, UiContext& ctx)
{
    if (!help || !IsWindow(help)) return;
    HelpState* h = reinterpret_cast<HelpState*>(GetWindowLongPtrW(help, GWLP_USERDATA));
    if (!h) return;
    h->ctx = &ctx;
    HWND controls[3] = { h->list, h->body, h->close };
    for (int i = 0; i < 3; ++i) SendMessageW(controls[i], WM_SETFONT, reinterpret_cast<WPARAM>(ctx.ui_font()), TRUE);
    help_layout(help, h);
    help_select(h);   // the rich text carries its own sizes, so it is drawn again at the new scale
}

} // namespace ui
} // namespace midibn
