// A plain text field, as the standard has it (docs/bada-compatible-v2.md §6.3): a list of
// characters, a selection (a caret when empty) and an optional marked range. The tests apply
// replies to it, as the text service applies them to a TSF context.
#pragma once

#include <string>
#include <vector>

#include "harness.h"

namespace test {

inline std::u32string Decode(const std::string& utf8) {
    std::u32string out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

inline std::string Encode(const std::u32string& text) {
    std::string out;
    for (const char32_t cp : text) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

class TextModel final : public bada::HostField {
public:
    std::u32string text;
    size_t from = 0, to = 0;  // the selection; a caret when equal
    bool marked = false;
    size_t mark_from = 0, mark_len = 0;
    bada::Menu menu;
    bool rtl = false;
    std::vector<std::string> commits;
    std::vector<bada::RawKey> forwarded;
    std::string notice;
    int begun = 0, ended = 0;  // replies bracketed

    void Begin() override { ++begun; }
    void End() override { ++ended; }

    void Commit(const std::string& utf8, bool replace_selection) override {
        const std::u32string t = Decode(utf8);
        const bool selection = replace_selection || !marked;
        const size_t a = selection ? from : mark_from, b = selection ? to : mark_from + mark_len;
        text.replace(a, b - a, t);
        from = to = a + t.size();
        marked = false;
        commits.push_back(utf8);
    }

    void SetPreedit(const bada::Preedit& preedit) override {
        if (!preedit.text.empty()) {
            const std::u32string t = Decode(preedit.text);
            const size_t a = marked ? mark_from : from, b = marked ? mark_from + mark_len : to;
            text.replace(a, b - a, t);
            marked = true;
            mark_from = a;
            mark_len = t.size();
            from = to = a + preedit.caret;
        } else if (marked) {
            text.erase(mark_from, mark_len);
            from = to = mark_from;
            marked = false;
        }
    }

    void ShowMenu(const bada::Menu& m, bool right_to_left) override {
        menu = m;
        rtl = right_to_left;
    }

    bada::Around TextAround(size_t before, size_t after) override {
        bada::Around around;
        if (!marked) around.selection = Encode(text.substr(from, to - from));
        const size_t b = before < from ? before : from;
        around.before = Encode(text.substr(from - b, b));
        around.after = Encode(text.substr(to, after));
        return around;
    }

    void ForwardKey(const bada::RawKey& key) override { forwarded.push_back(key); }
    void Notice(const std::string& n) override { notice = n; }

    // The whole text, and the marked part of it.
    std::string Text() const { return Encode(text); }
    std::string Marked() const { return marked ? Encode(text.substr(mark_from, mark_len)) : std::string(); }
    // Puts `s` in the field, selected.
    void Select(const std::string& s) {
        const std::u32string t = Decode(s);
        text.replace(from, to - from, t);
        to = from + t.size();
    }
    // What an app does with a key the harness didn't eat: types `typed`, or deletes backwards.
    void Insert(const std::string& s) {
        const std::u32string t = Decode(s);
        text.replace(from, to - from, t);
        from = to = from + t.size();
    }
};

}  // namespace test
