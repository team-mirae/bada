// How the text service and the tide helper (bada-tide.exe) talk on Windows. No Windows headers: the
// names, how a message's two words are packed, and the status block's layout.
//
// The helper holds every keyboard's link, one helper per logon session (a named mutex). It has a
// hidden top-level window of the class kHelperClass. Each TSF thread with bada active has a
// message-only window of the class kThreadClass, titled with its thread id in decimal.
//
//   text service -> helper   kHostMessage   wParam: the thread id. lParam: PackHost
//   helper -> text service   kKeyMessage    a key the keyboard reported, that bada took. PackKey
//   helper -> text service   kAskMessage    send kHostMessage now (the helper started, or focus moved)
//   helper -> text service   kStatusMessage the status block changed: publish the state again
//
// The status block (kStatusMapping) is the helper's `harness.tide` and its problems, as JSON.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "keys.h"
#include "keytable.h"
#include "tide.h"

namespace bada::tide::ipc {

inline constexpr wchar_t kMutex[] = L"Local\\com.teammirae.bada.tide";
inline constexpr wchar_t kHelperClass[] = L"com.teammirae.bada.tide";
inline constexpr wchar_t kThreadClass[] = L"com.teammirae.bada.tide.thread";
inline constexpr wchar_t kHostMessage[] = L"com.teammirae.bada.tide.host";
inline constexpr wchar_t kKeyMessage[] = L"com.teammirae.bada.tide.key";
inline constexpr wchar_t kAskMessage[] = L"com.teammirae.bada.tide.ask";
inline constexpr wchar_t kStatusMessage[] = L"com.teammirae.bada.tide.status";
inline constexpr wchar_t kStatusMapping[] = L"Local\\com.teammirae.bada.tide.status.v1";
inline constexpr wchar_t kHelperExe[] = L"bada-tide.exe";

// The harness's state as one word: henkan, Caps Lock, the profile's index in the cycle (255 none)
// and the keys it takes (Keys() order).
inline std::uint32_t PackHost(const Host& host) {
    const std::uint32_t profile = host.profile < 0 || host.profile > 254 ? 255u : static_cast<std::uint32_t>(host.profile);
    return (host.mode == Mode::kHenkan ? 1u : 0u) | (host.caps ? 2u : 0u) | (profile << 8) | (KeyBits(host.take) << 16);
}

inline Host UnpackHost(std::uint32_t word) {
    Host host;
    host.mode = (word & 1) ? Mode::kHenkan : Mode::kMuhenkan;
    host.caps = (word & 2) != 0;
    const std::uint32_t profile = (word >> 8) & 0xFF;
    host.profile = profile == 255 ? -1 : static_cast<int>(profile);
    host.take = KeysOf((word >> 16) & 0x7F);
    return host;
}

// A key event: wParam the key's index in Keys() and bit 8 for a press; lParam the modifiers.
struct KeyEvent {
    std::string code;
    bool down = false;
    Mods mods;
};

inline void PackKey(const KeyEvent& key, std::uintptr_t* wparam, std::intptr_t* lparam) {
    std::uintptr_t index = 0;
    for (; index < Keys().size() && Keys()[index] != key.code; ++index) {
    }
    *wparam = index | (key.down ? 0x100u : 0u);
    *lparam = (key.mods.ctrl ? 1 : 0) | (key.mods.alt ? 2 : 0) | (key.mods.shift ? 4 : 0) | (key.mods.meta ? 8 : 0);
}

inline std::optional<KeyEvent> UnpackKey(std::uintptr_t wparam, std::intptr_t lparam) {
    const std::uintptr_t index = wparam & 0xFF;
    if (index >= Keys().size()) return std::nullopt;
    KeyEvent key;
    key.code = Keys()[index];
    key.down = (wparam & 0x100) != 0;
    key.mods.ctrl = (lparam & 1) != 0;
    key.mods.alt = (lparam & 2) != 0;
    key.mods.shift = (lparam & 4) != 0;
    key.mods.meta = (lparam & 8) != 0;
    return key;
}

// The status block:
//   offset  size  field
//        0     4  sequence   odd while the text is being written
//        4     4  length     bytes of the text
//        8     …  text       UTF-8 JSON {"links": [...], "problems": [...]}
inline constexpr std::size_t kStatusSize = 16384;

// One writer (the helper). Returns false when the text doesn't fit.
bool WriteStatus(void* block, std::string_view json);
// nullopt when there is nothing, or it kept changing.
std::optional<std::string> ReadStatus(const void* block);

}  // namespace bada::tide::ipc
