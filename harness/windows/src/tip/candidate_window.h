// The engine's menu (docs/bada-compatible-v2.md §6.1: menu.rows with their notes, the selected row,
// a strip), drawn under the text. A topmost popup that never takes focus, kept out of screen
// captures. A click on a row picks it. The same window shows the harness's own short notices.
// Adapted from bada-windows src/tip/candidate_window.* (jis-moonshot's).
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "reply.h"

namespace bada::tip {

class CandidateWindow {
public:
    CandidateWindow() = default;
    ~CandidateWindow();
    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    // Shows `menu` below `anchor` (screen coordinates of the text), or above it when there is no
    // room below. Script forms (negative entries) are set off from the readings by a line. A menu
    // with no rows hides the window.
    void Show(const Menu& menu, const RECT& anchor);
    // Shows one line of text that can't be picked.
    void ShowNotice(const std::string& text, const RECT& anchor);
    void Hide();
    bool Visible() const;
    // Called with the row a click landed on.
    void OnPick(std::function<void(size_t row)> pick) { pick_ = std::move(pick); }
    static void Unregister();  // at DLL unload; fails harmlessly while a window exists

private:
    struct Row {
        std::wstring text;
        std::wstring note;
        int font = 0;  // 0 the system's, 1 Japanese, 2 Korean
        int entry = 0;
        RECT cell = {};
    };
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool Create();
    void Layout(const RECT& anchor);
    void Paint(HDC dc);
    void Click(POINT at);
    void UpdateFont();
    HFONT FontFor(const Row& row) const;

    HWND hwnd_ = nullptr;
    HFONT fonts_[3] = {};
    UINT font_dpi_ = 0;
    std::vector<Row> rows_;
    int selected_ = -1;
    bool strip_ = false;
    bool notice_ = false;
    int row_height_ = 0;
    int text_width_ = 0;
    int padding_ = 0;
    std::function<void(size_t)> pick_;
};

}  // namespace bada::tip
