// Linux key events → the standard's key codes (docs/bada-compatible-v2.md §4).
//
// A key is named by its position: its evdev code (linux/input-event-codes.h), which is what a W3C
// KeyboardEvent.code names. Fcitx5 reports the XKB keycode (evdev + 8) and IBus the evdev code; the
// frontends convert. The keysym is used only for a key that comes without a code (a synthetic key).
#pragma once

#include <string>
#include <vector>

namespace bada {

struct Mods {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;  // Super
    bool Any() const { return ctrl || alt || shift || meta; }
    bool operator==(const Mods& o) const {
        return ctrl == o.ctrl && alt == o.alt && shift == o.shift && meta == o.meta;
    }
};

// A key press or release, as a frontend sees it.
struct RawKey {
    unsigned code = 0;    // evdev code; 0: none (a synthetic key)
    unsigned sym = 0;     // the keysym; 0: unknown
    Mods mods;
    bool caps = false;    // the lock is on
    bool repeat = false;  // a repeat of a held key (no release since the last press)
    // What the system's keyboard layout types for this press, as UTF-8; "" when it types nothing
    // or the frontend can't tell. Read only to honour `types` (the standard, §6.4).
    std::string os_text;
};

// The key's code: "KeyA", "Convert", "ShiftLeft". A key the table below doesn't know is
// "Unidentified": the engine commits what is staged and passes it on.
std::string CodeOf(unsigned evdev, unsigned sym);

// A modifier's own key: ShiftLeft, ControlRight, AltLeft, MetaRight and so on, and Num Lock and
// Scroll Lock. Its press belongs to the next key: it never goes to the engine, and never commits.
bool IsModifierCode(const std::string& code);
// 半角/全角, 無変換, 変換, カタカナ/ひらがな, 英数 and かな (the standard, §4). Not Caps Lock.
bool IsLanguageKey(const std::string& code);
// A language key or Caps Lock: never sent to the engine.
bool IsHarnessKey(const std::string& code);
// The language keys and the lock this harness can see, for the published state (§9.3).
const std::vector<std::string>& DetectedKeys();

// `text` with its letters in capitals, for what the lock types (§6.4, §9.4). ASCII and Latin-1.
std::string Capitals(const std::string& text);

}  // namespace bada
