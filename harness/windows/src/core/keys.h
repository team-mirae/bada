// Windows key messages → the standard's key codes (docs/bada-compatible-v2.md §4).
//
// A key is named by its position: its scan code, which is what a W3C KeyboardEvent.code names. The
// virtual key depends on the keyboard layout Windows has loaded, so it is read only for keys whose
// position doesn't matter (Enter, the arrows, F keys) and for injected keys that come without a
// scan code. No Windows headers here: the tests run anywhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bada {

// Virtual keys read here, as in winuser.h.
namespace vk {
inline constexpr unsigned kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kShift = 0x10, kControl = 0x11, kMenu = 0x12,
                          kPause = 0x13, kCapital = 0x14, kHangul = 0x15, kHanja = 0x19, kEscape = 0x1B,
                          kConvert = 0x1C, kNonConvert = 0x1D, kSpace = 0x20, kPrior = 0x21, kNext = 0x22,
                          kEnd = 0x23, kHome = 0x24, kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28,
                          kInsert = 0x2D, kDelete = 0x2E, kLWin = 0x5B, kRWin = 0x5C, kApps = 0x5D,
                          kNumpad0 = 0x60, kMultiply = 0x6A, kAdd = 0x6B, kSeparator = 0x6C, kSubtract = 0x6D,
                          kDecimal = 0x6E, kDivide = 0x6F, kF1 = 0x70, kF24 = 0x87, kNumLock = 0x90,
                          kScroll = 0x91, kLShift = 0xA0, kRShift = 0xA1, kLControl = 0xA2, kRControl = 0xA3,
                          kLMenu = 0xA4, kRMenu = 0xA5, kProcessKey = 0xE5, kPacket = 0xE7,
                          // The Japanese layout's (kbd106) language keys.
                          kDbeAlphanumeric = 0xF0, kDbeKatakana = 0xF1, kDbeHiragana = 0xF2, kOemAuto = 0xF3,
                          kOemEnlw = 0xF4, kDbeRoman = 0xF5, kDbeNoRoman = 0xF6;
}  // namespace vk

struct Mods {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;  // the Windows key
    bool Any() const { return ctrl || alt || shift || meta; }
    bool operator==(const Mods& o) const {
        return ctrl == o.ctrl && alt == o.alt && shift == o.shift && meta == o.meta;
    }
};

// A key press or release, as the text service sees it.
struct RawKey {
    unsigned vk = 0;        // wParam
    unsigned scan = 0;      // lParam bits 16-23, or MapVirtualKey's for an injected key without one
    bool extended = false;  // lParam bit 24
    Mods mods;
    bool caps = false;    // the lock is on (GetKeyState(VK_CAPITAL) & 1)
    bool repeat = false;  // lParam bit 30: a repeat of a held key
    // What Windows' keyboard layout types for this press, as UTF-8; "" when it types nothing or
    // the text service can't tell. Read only to honour `types` (the standard, §6.4).
    std::string os_text;
};

// The key's code: "KeyA", "Convert", "ShiftLeft". "" for a key that is no key press at all
// (VK_PROCESSKEY). A key the tables below don't know, and a character injected as Unicode
// (VK_PACKET), is "Unidentified": the engine commits what is staged and passes it on.
std::string CodeOf(const RawKey& key);

// A modifier's own key: ShiftLeft, ControlRight, AltLeft, MetaRight and so on, and Num Lock and
// Scroll Lock. Its press belongs to the next key: it never goes to the engine, and never commits.
bool IsModifierCode(const std::string& code);
// 半角/全角, 無変換, 変換, カタカナ/ひらがな, 英数 and かな (the standard, §4). Not Caps Lock.
bool IsLanguageKey(const std::string& code);
// A language key or Caps Lock: never sent to the engine.
bool IsHarnessKey(const std::string& code);
// The language keys and the lock this harness can see, for the published state (§9.3).
const std::vector<std::string>& DetectedKeys();

// The Japanese layout's lock-like keys (半角/全角, カタカナ/ひらがな, the unshifted Caps Lock key):
// Windows reports their press, and their release only with the next press of the key. The text
// service takes such a key as pressed and released at once.
bool ReleasesLate(unsigned vk);

// `text` with its letters in capitals, for what the lock types (§6.4, §9.4). ASCII and Latin-1.
std::string Capitals(const std::string& text);

// The scan code for `vk` when an injected key came without one: MapVirtualKeyW on Windows, a stub
// in the tests.
unsigned ScanOrMapped(unsigned vk, unsigned scan, unsigned (*scan_of)(unsigned vk));

// Whether a key-down test (wParam, lParam, message time, tick) is the same key-down as an earlier
// one, tested again. A first press can't come twice without a key-up, so the same lParam within a
// short while is the same press; repeats share their lParam, so those must have the same message
// time too.
bool SameKeyDown(std::uintptr_t wparam, std::intptr_t lparam, std::int32_t time, std::uint64_t tick,
                 std::uintptr_t wparam2, std::intptr_t lparam2, std::int32_t time2, std::uint64_t tick2);

}  // namespace bada
