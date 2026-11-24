// bada's identifiers on Windows (docs/bada-compatible-v2.md §12). Every one is bada's own: none is
// shared with bada-windows, bada ime's Windows port, so the two install side by side. They are
// permanent: they are in users' registries and language lists.
//
// kLangBarItemInputMode and kPropInputScope are Windows' own.
#pragma once

#include <guiddef.h>

namespace bada::tip {

// The text service's COM class. {5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}
inline constexpr GUID kClsid = {0x5b6d78a1, 0xd25c, 0x40fc, {0xb4, 0xff, 0x4a, 0xa0, 0x21, 0x20, 0xbd, 0x84}};
inline constexpr wchar_t kClsidText[] = L"{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}";

// Its one language profile. {89CC34D8-9E73-44BD-B1F9-301D19A5C30F}
inline constexpr GUID kProfile = {0x89cc34d8, 0x9e73, 0x44bd, {0xb1, 0xf9, 0x30, 0x1d, 0x19, 0xa5, 0xc3, 0x0f}};
inline constexpr wchar_t kProfileText[] = L"{89CC34D8-9E73-44BD-B1F9-301D19A5C30F}";

// The language the profile is listed under: Japanese. Under it Windows loads the Japanese keyboard
// layout, which gives a JIS keyboard's language keys their own virtual keys and types the JIS
// legends, as the standard has them (§4, §7.2).
inline constexpr unsigned short kLanguage = 0x0411;

// What Windows shows in its language list, and the harness's id.
inline constexpr wchar_t kDisplayName[] = L"bada";

// The session's mode and profile, shared by every process (a TSF global compartment).
// {D8E727A0-D33B-4BD7-B200-3FD62D13B33B}
inline constexpr GUID kSessionCompartment = {0xd8e727a0, 0xd33b, 0x4bd7, {0xb2, 0x00, 0x3f, 0xd6, 0x2d, 0x13, 0xb3, 0x3b}};

// Display attributes of the marked text, by the kind of its segment (§6.1).
// live: {0C41C814-D915-47F0-BDD2-940A84491C4C}
inline constexpr GUID kDisplayAttributeLive = {0x0c41c814, 0xd915, 0x47f0, {0xbd, 0xd2, 0x94, 0x0a, 0x84, 0x49, 0x1c, 0x4c}};
// fixed: {9197B9D4-5D36-4935-B141-B6C8C4E44B81}
inline constexpr GUID kDisplayAttributeFixed = {0x9197b9d4, 0x5d36, 0x4935, {0xb1, 0x41, 0xb6, 0xc8, 0xc4, 0xe4, 0x4b, 0x81}};
// picked: {FC87C3BB-2087-41F7-ABA7-A45B1E7217AD}
inline constexpr GUID kDisplayAttributePicked = {0xfc87c3bb, 0x2087, 0x41f7, {0xab, 0xa7, 0xa4, 0x5b, 0x1e, 0x72, 0x17, 0xad}};

// The input indicator's mode button (Windows 8 and later), Windows' value.
// {2C77A81E-41CC-4178-A3A7-5F8A987568E6}
inline constexpr GUID kLangBarItemInputMode = {0x2c77a81e, 0x41cc, 0x4178, {0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6}};

// The text property holding a field's input scopes (InputScope.h's GUID_PROP_INPUTSCOPE).
// {1713DD5A-68E7-4A5B-9AF6-592A595C778D}
inline constexpr GUID kPropInputScope = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

// The state for other programs (§6.7, §12): a block of shared memory, the lock its writers take
// turns with, and the window message that says it changed.
inline constexpr wchar_t kStateMapping[] = L"Local\\com.teammirae.bada.state.v2.{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}";
inline constexpr wchar_t kStateLock[] = L"Local\\com.teammirae.bada.state.v2.{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}.lock";
inline constexpr wchar_t kStateMessage[] = L"com.teammirae.bada.state";

// What marks a key the text service injected itself (a Caps Lock press that sets the lock back, a
// key handed on late): the key's extra information. The key sink lets such a key through unread.
inline constexpr unsigned long long kInjectedMark = 0x62616461;  // "bada"

}  // namespace bada::tip
