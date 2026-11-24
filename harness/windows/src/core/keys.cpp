#include "keys.h"

namespace bada {
namespace {

// Scan code set 1 → KeyboardEvent.code, for keys that aren't extended: the 48 typing keys, and the
// keys that are told apart by position alone. nullptr: not in the table.
struct ScanTable {
    const char* codes[0x80] = {};
    constexpr ScanTable() {
        const char* digits[] = {"Digit1", "Digit2", "Digit3", "Digit4", "Digit5",
                                "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"};
        for (int i = 0; i < 10; ++i) codes[0x02 + i] = digits[i];
        codes[0x0C] = "Minus";
        codes[0x0D] = "Equal";  // JIS ^
        const char* q_row[] = {"KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY",
                               "KeyU", "KeyI", "KeyO", "KeyP", "BracketLeft", "BracketRight"};
        for (int i = 0; i < 12; ++i) codes[0x10 + i] = q_row[i];
        const char* a_row[] = {"KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH",
                               "KeyJ", "KeyK", "KeyL", "Semicolon", "Quote"};
        for (int i = 0; i < 11; ++i) codes[0x1E + i] = a_row[i];
        codes[0x29] = "Backquote";  // 半角/全角 on JIS
        codes[0x2B] = "Backslash";  // JIS ]
        const char* z_row[] = {"KeyZ", "KeyX", "KeyC", "KeyV", "KeyB",
                               "KeyN", "KeyM", "Comma", "Period", "Slash"};
        for (int i = 0; i < 10; ++i) codes[0x2C + i] = z_row[i];
        codes[0x56] = "IntlBackslash";  // ISO only
        codes[0x73] = "IntlRo";         // ろ
        codes[0x7D] = "IntlYen";        // ¥
    }
};
constexpr ScanTable kScan;

// The language keys and the lock, by scan code.
const char* HarnessKeyByScan(unsigned scan) {
    switch (scan) {
        case 0x7B: return "NonConvert";  // 無変換
        case 0x79: return "Convert";     // 変換
        case 0x70: return "KanaMode";    // カタカナ/ひらがな
        // HID LANG1 and LANG2: an Apple JIS keyboard's かな and 英数, and a Korean keyboard's 한/영
        // and 한자. Windows gives them 0x72 and 0x71; a PS/2 Korean keyboard sends 0xF2 and 0xF1.
        case 0x72: case 0xF2: return "Lang1";
        case 0x71: case 0xF1: return "Lang2";
        case 0x3A: return "CapsLock";
    }
    return nullptr;
}

const char* NamedByVk(unsigned v) {
    switch (v) {
        case vk::kBack: return "Backspace";
        case vk::kTab: return "Tab";
        case vk::kReturn: return "Enter";  // the keypad's too (§4)
        case vk::kEscape: return "Escape";
        case vk::kSpace: return "Space";
        case vk::kLeft: return "ArrowLeft";
        case vk::kUp: return "ArrowUp";
        case vk::kRight: return "ArrowRight";
        case vk::kDown: return "ArrowDown";
        case vk::kHome: return "Home";
        case vk::kEnd: return "End";
        case vk::kPrior: return "PageUp";
        case vk::kNext: return "PageDown";
        case vk::kInsert: return "Insert";
        case vk::kDelete: return "Delete";
        case vk::kPause: return "Pause";
        case vk::kApps: return "ContextMenu";
        case vk::kMultiply: return "NumpadMultiply";
        case vk::kAdd: return "NumpadAdd";
        case vk::kSeparator: return "NumpadComma";
        case vk::kSubtract: return "NumpadSubtract";
        case vk::kDecimal: return "NumpadDecimal";
        case vk::kDivide: return "NumpadDivide";
        case vk::kNumLock: return "NumLock";
        case vk::kScroll: return "ScrollLock";
        case vk::kLWin: return "MetaLeft";
        case vk::kRWin: return "MetaRight";
    }
    return nullptr;
}

}  // namespace

std::string CodeOf(const RawKey& key) {
    if (key.vk == vk::kPacket) return "Unidentified";  // commits first, then reaches the app
    if (key.vk == vk::kProcessKey) return "";
    // The modifiers. The text service gets VK_SHIFT, VK_CONTROL and VK_MENU: the side is in the
    // scan code (Shift) or the extended flag (Control, Alt).
    switch (key.vk) {
        case vk::kShift: return key.scan == 0x36 ? "ShiftRight" : "ShiftLeft";
        case vk::kLShift: return "ShiftLeft";
        case vk::kRShift: return "ShiftRight";
        case vk::kControl: return key.extended ? "ControlRight" : "ControlLeft";
        case vk::kLControl: return "ControlLeft";
        case vk::kRControl: return "ControlRight";
        case vk::kMenu: return key.extended ? "AltRight" : "AltLeft";
        case vk::kLMenu: return "AltLeft";
        case vk::kRMenu: return "AltRight";
    }
    // The language keys and the lock go by position, whatever virtual key the layout gives them.
    if (!key.extended) {
        if (const char* code = HarnessKeyByScan(key.scan)) return code;
    }
    if (key.vk >= vk::kF1 && key.vk <= vk::kF24) return "F" + std::to_string(key.vk - vk::kF1 + 1);
    if (key.vk >= vk::kNumpad0 && key.vk <= vk::kNumpad0 + 9) return "Numpad" + std::to_string(key.vk - vk::kNumpad0);
    if (const char* named = NamedByVk(key.vk)) return named;
    // The typing keys, by position. 半角/全角 is the Backquote position: under the Japanese layout
    // its virtual key is VK_OEM_AUTO or VK_OEM_ENLW, and VK_KANJI with Alt.
    if (!key.extended && key.scan < 0x80 && kScan.codes[key.scan]) return kScan.codes[key.scan];
    // A key that came without a scan code the tables know: by its virtual key.
    switch (key.vk) {
        case vk::kNonConvert: return "NonConvert";
        case vk::kConvert: return "Convert";
        case vk::kDbeKatakana: case vk::kDbeHiragana: return "KanaMode";
        case vk::kOemAuto: case vk::kOemEnlw: return "Backquote";
        case vk::kDbeAlphanumeric: case vk::kCapital: return "CapsLock";
        case vk::kHangul: return "Lang1";
        case vk::kHanja: return "Lang2";
    }
    return "Unidentified";
}

bool IsModifierCode(const std::string& code) {
    return code == "ShiftLeft" || code == "ShiftRight" || code == "ControlLeft" || code == "ControlRight" ||
           code == "AltLeft" || code == "AltRight" || code == "MetaLeft" || code == "MetaRight" ||
           code == "NumLock" || code == "ScrollLock";
}

bool IsLanguageKey(const std::string& code) {
    return code == "Backquote" || code == "NonConvert" || code == "Convert" || code == "KanaMode" ||
           code == "Lang2" || code == "Lang1";
}

bool IsHarnessKey(const std::string& code) { return code == "CapsLock" || IsLanguageKey(code); }

const std::vector<std::string>& DetectedKeys() {
    // TSF hands the text service every key of the focused app by its scan code, so all of them are
    // seen wherever the keyboard has them.
    static const std::vector<std::string> keys = {"Backquote", "NonConvert", "Convert", "KanaMode",
                                                  "Lang2",     "Lang1",      "CapsLock"};
    return keys;
}

bool ReleasesLate(unsigned v) { return v >= vk::kDbeAlphanumeric && v <= vk::kDbeNoRoman; }

std::string Capitals(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        } else if (c == 0xC3 && i + 1 < text.size()) {
            // Latin-1: à (C3 A0) … þ (C3 BE) are 0x20 above their capitals, but for ÷ (C3 B7).
            unsigned char d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) d = static_cast<unsigned char>(d - 0x20);
            out.push_back(static_cast<char>(c));
            out.push_back(static_cast<char>(d));
            ++i;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

unsigned ScanOrMapped(unsigned v, unsigned scan, unsigned (*scan_of)(unsigned)) {
    if (scan == 0 && v != vk::kPacket && scan_of) return scan_of(v) & 0xFF;
    return scan;
}

bool SameKeyDown(std::uintptr_t wparam, std::intptr_t lparam, std::int32_t time, std::uint64_t tick,
                 std::uintptr_t wparam2, std::intptr_t lparam2, std::int32_t time2, std::uint64_t tick2) {
    if (wparam != wparam2 || lparam != lparam2) return false;
    if (time == time2) return true;
    // A missed key-up (focus moved to another thread in between) must not swallow a later press.
    constexpr std::uint64_t kRetest = 500;
    const bool first_press = (lparam & (std::intptr_t{1} << 30)) == 0;
    return first_press && tick2 >= tick && tick2 - tick < kRetest;
}

}  // namespace bada
