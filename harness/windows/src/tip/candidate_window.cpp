// Adapted from bada-windows src/tip/candidate_window.* (jis-moonshot's).
#include "candidate_window.h"

#include <windowsx.h>

#include <algorithm>

#include "dll.h"
#include "shared_session.h"

namespace bada::tip {
namespace {

constexpr wchar_t kClassName[] = L"com.teammirae.bada.menu";

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

// The font a row is drawn in, by its language (BCP 47): kanji take their Japanese forms in a
// Japanese row, and Hangul its own font in a Korean one.
int FontOf(const std::string& lang) {
    if (lang.rfind("ja", 0) == 0) return 1;
    if (lang.rfind("ko", 0) == 0) return 2;
    return 0;
}

}  // namespace

CandidateWindow::~CandidateWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT font : fonts_) {
        if (font) DeleteObject(font);
    }
}

void CandidateWindow::UpdateFont() {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    if (fonts_[0] && dpi == font_dpi_) return;
    for (HFONT& font : fonts_) {
        if (font) DeleteObject(font);
        font = nullptr;
    }
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfHeight = -MulDiv(11, static_cast<int>(dpi), 72);  // 11 pt
    // The system's UI font (Segoe UI, with font linking for what it lacks).
    fonts_[0] = CreateFontIndirectW(&lf);
    lf.lfCharSet = SHIFTJIS_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Yu Gothic UI");
    fonts_[1] = CreateFontIndirectW(&lf);
    lf.lfCharSet = HANGUL_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Malgun Gothic");
    fonts_[2] = CreateFontIndirectW(&lf);
    font_dpi_ = dpi;
}

HFONT CandidateWindow::FontFor(const Row& row) const { return fonts_[row.font] ? fonts_[row.font] : fonts_[0]; }

bool CandidateWindow::Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

bool CandidateWindow::Create() {
    if (hwnd_) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = DllInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly when already registered
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP | WS_BORDER,
                            0, 0, 1, 1, nullptr, nullptr, DllInstance(), nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Never in screen recordings, screenshots or screen sharing: the menu shows what is being typed.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    return true;
}

void CandidateWindow::Show(const Menu& menu, const RECT& anchor) {
    if (menu.rows.empty()) {
        Hide();
        return;
    }
    if (!Create()) return;
    rows_.clear();
    for (const bada::Row& row : menu.rows) {
        rows_.push_back({Wide(row.text), Wide(row.note), FontOf(row.lang), row.entry, {}});
    }
    selected_ = menu.selected ? static_cast<int>(*menu.selected) : -1;
    strip_ = menu.strip;
    notice_ = false;
    Layout(anchor);
}

void CandidateWindow::ShowNotice(const std::string& text, const RECT& anchor) {
    if (text.empty()) {
        if (notice_) Hide();
        return;
    }
    if (!Create()) return;
    rows_.clear();
    rows_.push_back({Wide(text), {}, 0, 0, {}});
    selected_ = -1;
    strip_ = false;
    notice_ = true;
    Layout(anchor);
}

void CandidateWindow::Layout(const RECT& anchor) {
    UpdateFont();
    const UINT dpi = font_dpi_;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, fonts_[0]);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    padding_ = Scale(6, dpi);
    row_height_ = tm.tmHeight + Scale(4, dpi);
    SIZE size;
    text_width_ = 0;
    std::vector<int> widths;
    for (const Row& r : rows_) {
        SelectObject(dc, FontFor(r));
        GetTextExtentPoint32W(dc, r.text.c_str(), static_cast<int>(r.text.size()), &size);
        text_width_ = std::max(text_width_, static_cast<int>(size.cx));
        widths.push_back(static_cast<int>(size.cx) + 2 * padding_);
    }
    SelectObject(dc, fonts_[0]);
    int width = 0;
    if (strip_) {
        for (int w : widths) width += w;
    } else {
        for (const Row& r : rows_) {
            int w = text_width_;
            if (!r.note.empty()) {
                GetTextExtentPoint32W(dc, r.note.c_str(), static_cast<int>(r.note.size()), &size);
                w += Scale(12, dpi) + size.cx;
            }
            width = std::max(width, w);
        }
        width += padding_ * 2;
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    width += Scale(2, dpi);
    const int lines = strip_ ? 1 : static_cast<int>(rows_.size());
    const int height = padding_ * 2 + row_height_ * lines + Scale(2, dpi);

    // Each row's place in the window: what is painted, and what a click lands on.
    int x = 1, y = padding_;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (strip_) {
            rows_[i].cell = {x, y, x + widths[i], y + row_height_};
            x += widths[i];
        } else {
            rows_[i].cell = {padding_ / 2, y, width - Scale(2, dpi) - padding_ / 2, y + row_height_};
            y += row_height_;
        }
    }

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const int left = std::clamp(static_cast<int>(anchor.left), static_cast<int>(mi.rcWork.left),
                                std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - width));
    int top = anchor.bottom + Scale(2, dpi);
    if (top + height > mi.rcWork.bottom) top = anchor.top - height - Scale(2, dpi);
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CandidateWindow::Unregister() { UnregisterClassW(kClassName, DllInstance()); }

void CandidateWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    notice_ = false;
}

void CandidateWindow::Paint(HDC dc) {
    RECT client;
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    HGDIOBJ old = SelectObject(dc, fonts_[0]);
    SetBkMode(dc, TRANSPARENT);
    const int flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const bool focused = static_cast<int>(i) == selected_;
        if (focused) FillRect(dc, &r.cell, GetSysColorBrush(COLOR_HIGHLIGHT));
        // Script forms (negative entries) are set off from the readings after them by a line.
        if (!strip_ && i > 0 && rows_[i - 1].entry < 0 && r.entry >= 0) {
            RECT line = {client.left + padding_, r.cell.top - 1, client.right - padding_, r.cell.top};
            FillRect(dc, &line, GetSysColorBrush(COLOR_GRAYTEXT));
        }
        SetTextColor(dc, GetSysColor(focused ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        SelectObject(dc, FontFor(r));
        RECT text = r.cell;
        if (!strip_) text.left = client.left + padding_;
        DrawTextW(dc, r.text.c_str(), -1, &text, flags | (strip_ ? DT_CENTER : DT_LEFT));
        SelectObject(dc, fonts_[0]);
        if (!strip_ && !r.note.empty()) {
            SetTextColor(dc, GetSysColor(focused ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
            RECT note = {client.left + padding_ + text_width_ + Scale(12, font_dpi_), r.cell.top,
                         client.right - padding_, r.cell.bottom};
            DrawTextW(dc, r.note.c_str(), -1, &note, flags | DT_LEFT);
        }
    }
    SelectObject(dc, old);
}

void CandidateWindow::Click(POINT at) {
    if (notice_ || !pick_) return;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (PtInRect(&rows_[i].cell, at)) {
            // The callback may show another menu, or none: nothing of this one is read after it.
            const auto pick = pick_;
            pick(i);
            return;
        }
    }
}

LRESULT CALLBACK CandidateWindow::Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // a click never takes focus from the app
        case WM_LBUTTONUP:
            if (self) self->Click({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) self->Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace bada::tip
