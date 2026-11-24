#include "keys.h"

namespace bada {
namespace {

// evdev code → KeyboardEvent.code. nullptr: not in the table.
struct CodeTable {
    const char* codes[0x100] = {};
    constexpr CodeTable() {
        codes[1] = "Escape";
        const char* digits[] = {"Digit1", "Digit2", "Digit3", "Digit4", "Digit5",
                                "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"};
        for (int i = 0; i < 10; ++i) codes[2 + i] = digits[i];
        codes[12] = "Minus";
        codes[13] = "Equal";  // JIS ^
        codes[14] = "Backspace";
        codes[15] = "Tab";
        const char* q_row[] = {"KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY",
                               "KeyU", "KeyI", "KeyO", "KeyP", "BracketLeft", "BracketRight"};
        for (int i = 0; i < 12; ++i) codes[16 + i] = q_row[i];
        codes[28] = "Enter";
        codes[29] = "ControlLeft";
        const char* a_row[] = {"KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH",
                               "KeyJ", "KeyK", "KeyL", "Semicolon", "Quote"};
        for (int i = 0; i < 11; ++i) codes[30 + i] = a_row[i];
        codes[41] = "Backquote";  // 半角/全角 on JIS (KEY_GRAVE)
        codes[42] = "ShiftLeft";
        codes[43] = "Backslash";  // JIS ]
        const char* z_row[] = {"KeyZ", "KeyX", "KeyC", "KeyV", "KeyB",
                               "KeyN", "KeyM", "Comma", "Period", "Slash"};
        for (int i = 0; i < 10; ++i) codes[44 + i] = z_row[i];
        codes[54] = "ShiftRight";
        codes[55] = "NumpadMultiply";
        codes[56] = "AltLeft";
        codes[57] = "Space";
        codes[58] = "CapsLock";
        const char* f_keys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"};
        for (int i = 0; i < 10; ++i) codes[59 + i] = f_keys[i];
        codes[69] = "NumLock";
        codes[70] = "ScrollLock";
        const char* pad[] = {"Numpad7", "Numpad8", "Numpad9", "NumpadSubtract", "Numpad4", "Numpad5", "Numpad6",
                             "NumpadAdd", "Numpad1", "Numpad2", "Numpad3", "Numpad0", "NumpadDecimal"};
        for (int i = 0; i < 13; ++i) codes[71 + i] = pad[i];
        codes[85] = "Backquote";      // KEY_ZENKAKUHANKAKU, from keyboards that send it
        codes[86] = "IntlBackslash";  // ISO only
        codes[87] = "F11";
        codes[88] = "F12";
        codes[89] = "IntlRo";    // ろ
        codes[90] = "KanaMode";  // KEY_KATAKANA
        codes[91] = "KanaMode";  // KEY_HIRAGANA
        codes[92] = "Convert";   // 変換
        codes[93] = "KanaMode";  // カタカナ/ひらがな
        codes[94] = "NonConvert";  // 無変換
        codes[95] = "NumpadComma";
        codes[96] = "Enter";  // the keypad's too
        codes[97] = "ControlRight";
        codes[98] = "NumpadDivide";
        codes[100] = "AltRight";
        codes[102] = "Home";
        codes[103] = "ArrowUp";
        codes[104] = "PageUp";
        codes[105] = "ArrowLeft";
        codes[106] = "ArrowRight";
        codes[107] = "End";
        codes[108] = "ArrowDown";
        codes[109] = "PageDown";
        codes[110] = "Insert";
        codes[111] = "Delete";
        codes[119] = "Pause";
        codes[121] = "NumpadComma";
        // An Apple JIS keyboard's かな and 英数 are HID LANG1 and LANG2, which Linux names after
        // the Korean keys.
        codes[122] = "Lang1";  // KEY_HANGEUL
        codes[123] = "Lang2";  // KEY_HANJA
        codes[124] = "IntlYen";  // ¥
        codes[125] = "MetaLeft";
        codes[126] = "MetaRight";
        codes[127] = "ContextMenu";
        const char* f_more[] = {"F13", "F14", "F15", "F16", "F17", "F18",
                                "F19", "F20", "F21", "F22", "F23", "F24"};
        for (int i = 0; i < 12; ++i) codes[183 + i] = f_more[i];
    }
};
constexpr CodeTable kCodes;

// Keysyms (X11 keysymdef.h), for keys that come without a code.
const char* BySym(unsigned sym) {
    switch (sym) {
        case 0xff22: return "NonConvert";  // Muhenkan
        case 0xff23: return "Convert";     // Henkan_Mode
        case 0xff25: case 0xff26: case 0xff27: return "KanaMode";  // Hiragana, Katakana, Hiragana_Katakana
        case 0xff2a: return "Backquote";   // Zenkaku_Hankaku
        case 0xff30: return "CapsLock";    // Eisu_toggle: the jp layout's unshifted Caps Lock key
        case 0xff31: return "Lang1";       // Hangul
        case 0xff34: return "Lang2";       // Hangul_Hanja
        case 0xffe5: return "CapsLock";
        case 0xff08: return "Backspace";
        case 0xff09: return "Tab";
        case 0xff0d: case 0xff8d: return "Enter";
        case 0xff1b: return "Escape";
        case 0x0020: return "Space";
        case 0xff50: return "Home";
        case 0xff51: return "ArrowLeft";
        case 0xff52: return "ArrowUp";
        case 0xff53: return "ArrowRight";
        case 0xff54: return "ArrowDown";
        case 0xff57: return "End";
        case 0xffff: return "Delete";
        case 0xffe1: return "ShiftLeft";
        case 0xffe2: return "ShiftRight";
        case 0xffe3: return "ControlLeft";
        case 0xffe4: return "ControlRight";
        case 0xffe9: return "AltLeft";
        case 0xffea: return "AltRight";
        case 0xffeb: return "MetaLeft";
        case 0xffec: return "MetaRight";
    }
    return nullptr;
}

}  // namespace

std::string CodeOf(unsigned evdev, unsigned sym) {
    if (evdev > 0 && evdev < 0x100 && kCodes.codes[evdev]) return kCodes.codes[evdev];
    if (evdev == 0) {
        if (const char* code = BySym(sym)) return code;
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
    // Fcitx5 and IBus hand the input method every key by its evdev code, so all of them are seen
    // wherever the keyboard has them.
    static const std::vector<std::string> keys = {"Backquote", "NonConvert", "Convert", "KanaMode",
                                                  "Lang2",     "Lang1",      "CapsLock"};
    return keys;
}

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

}  // namespace bada
