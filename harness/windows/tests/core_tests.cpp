// The core, with no TSF: Windows' keys, the settings folder, the key flow, `types`, Caps Lock,
// replies applied to a plain text field, the mode shared between processes, and the published
// state with its shared-memory block. Everything here types into the linked engine.
//
//   bada_core_tests
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "harness.h"
#include "json.h"
#include "keys.h"
#include "keytable.h"
#include "reply.h"
#include "settings.h"
#include "state_block.h"
#include "textmodel.h"

using namespace bada;
using test::Check;
using test::CheckEq;
namespace fs = std::filesystem;

namespace {

// A new folder in the system's temporary folder.
fs::path TempDir(const char* name) {
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const fs::path dir =
            fs::temp_directory_path() / ("bada-test-" + std::string(name) + "-" + std::to_string(random() % 1000000));
        std::error_code ec;
        if (fs::create_directories(dir, ec) && !ec) return dir;
    }
    return {};
}

// A temporary file, then a rename: as the settings page and the command line write (§10.1). Each
// write is a second later than the one before, so no two look the same to the harness.
void WriteFile(const fs::path& path, const std::string& text) {
    static int writes = 0;
    static const auto base = fs::file_time_type::clock::now();
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
    fs::last_write_time(tmp, base + std::chrono::seconds(++writes));
    fs::rename(tmp, path);
}

// A key as the text service would report it, from its W3C code: its scan code where the key goes
// by position, its virtual key otherwise.
RawKey Raw(const std::string& code) {
    for (unsigned scan = 1; scan < 0x100; ++scan) {
        RawKey k;
        k.scan = scan;
        if (CodeOf(k) == code) return k;
    }
    for (unsigned v = 1; v < 0x100; ++v) {
        RawKey k;
        k.vk = v;
        if (CodeOf(k) == code) return k;
    }
    Check(false, "the tests know no key named " + code);
    return {};
}

// A press. `os`: what Windows' layout types for it.
RawKey Key(const std::string& code, const std::string& os = "", Mods mods = {}, bool caps = false) {
    RawKey k = Raw(code);
    k.mods = mods;
    k.caps = caps;
    k.os_text = os;
    return k;
}
constexpr Mods kShift{false, false, true, false};
constexpr Mods kCtrl{true, false, false, false};
constexpr Mods kAlt{false, true, false, false};

const char* kSettings = R"({
  "format": "bada-settings/2",
  "engine": {
    "muhenkan": {"layout": "en-qwerty"},
    "henkan": {"profiles": [
      {"id": "ko", "name": "한국어", "layouts": ["ko-2-set"]},
      {"id": "ja", "name": "日本語", "layouts": ["ja-romaji"]},
      {"id": "work", "name": "work", "layouts": ["ko-2-set", "en-qwerty"]}
    ]}
  },
  "keys": {
    "bindings": [
      {"on": "NonConvert", "do": ["mode:muhenkan"]},
      {"on": "Convert", "do": ["profile.first"]},
      {"on": "Convert", "do": ["menu.next", "profile.first"], "when": "henkan"},
      {"on": "shift+Convert", "do": ["menu.prev"], "when": "henkan"},
      {"on": "ctrl+Convert", "do": ["fix"], "when": "henkan"},
      {"on": "Backquote", "do": ["width.full"]},
      {"on": "shift+Backquote", "do": ["width.half"]},
      {"on": "KanaMode", "do": ["profile:ja", "menu.next"]},
      {"on": "F13", "do": ["profile:work"]},
      {"on": "Lang1+Lang2", "do": ["profile:work"]},
      {"on": "Lang2", "do": ["pass"]},
      {"on": "F14", "do": ["pass"]},
      {"on": "ShiftRight", "do": ["mode.toggle"]}
    ],
    "cycle": ["ko", "ja"], "caps_rule": "upper_snake"
  },
  "harness": {"com.teammirae.bada.windows": {"keyboard": "jis-pc"}}
})";

struct Sink final : StateSink {
    std::vector<std::string> published;
    std::vector<StateSummary> summaries;
    int cleared = 0;
    void Publish(const std::string& json, const StateSummary& summary) override {
        published.push_back(json);
        summaries.push_back(summary);
    }
    void Clear() override { ++cleared; }
};

// A session on its own settings folder.
struct Rig {
    fs::path dir;
    Sink sink;
    std::vector<std::string> log;
    std::unique_ptr<Session> session;

    explicit Rig(const char* name, const char* settings = kSettings, Preset detected = Preset::kNoLanguageKeys,
                 const fs::path& data = {})
        : dir(TempDir(name)) {
        if (settings) WriteFile(dir / "settings.json", settings);
        Session::Options options;
        options.config_dir = dir;
        options.data_dir = data;
        options.version = "0.1.0";
        options.detected = detected;
        options.sink = &sink;
        options.log = [this](const std::string& line) { log.push_back(line); };
        std::string error;
        session = Session::Create(std::move(options), &error);
        Check(session != nullptr, std::string("the session starts: ") + error);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    json::Value State() const { return sink.published.empty() ? json::Value{} : *json::Parse(sink.published.back()); }
};

// Types a run of keys: each is a W3C code with what a US layout would type for it.
void Type(Context& ctx, test::TextModel& field, const std::vector<std::pair<const char*, const char*>>& keys,
          const ContextInfo& info = {}) {
    for (const auto& [code, os] : keys) {
        const RawKey key = Key(code, os);
        if (!ctx.KeyDown(key, field, info)) field.Insert(os);  // the app types what Windows has
        ctx.KeyUp(key, field, info);
    }
}

RawKey Scan(unsigned scan, unsigned v = 0, bool extended = false) {
    RawKey k;
    k.scan = scan;
    k.vk = v;
    k.extended = extended;
    return k;
}

void TestJsonAndKeys() {
    const auto v = json::Parse(R"({"a": [1, 2.5, "xé😀"], "b": {"c": null, "d": true}})");
    Check(v.has_value(), "json parses");
    CheckEq(json::Write(*v), "{\"a\":[1,2.5,\"x\xC3\xA9\xF0\x9F\x98\x80\"],\"b\":{\"c\":null,\"d\":true}}", "json writes");
    Check(!json::Parse("{").has_value() && !json::Parse("[1,]").has_value(), "bad json is refused");

    // Keys go by position: the scan code, whatever virtual key the layout gives.
    CheckEq(CodeOf(Scan(0x1E, 'A')), "KeyA", "scan 1E");
    CheckEq(CodeOf(Scan(0x1E, 'Q')), "KeyA", "the A position under a layout that has Q there");
    CheckEq(CodeOf(Scan(0x79, vk::kConvert)), "Convert", "変換");
    CheckEq(CodeOf(Scan(0x7B, vk::kNonConvert)), "NonConvert", "無変換");
    CheckEq(CodeOf(Scan(0x70, vk::kDbeHiragana)), "KanaMode", "カタカナ/ひらがな");
    CheckEq(CodeOf(Scan(0x70, 0xFF)), "KanaMode", "カタカナ/ひらがな under a layout that gives it no virtual key");
    CheckEq(CodeOf(Scan(0x29, vk::kOemAuto)), "Backquote", "半角/全角");
    CheckEq(CodeOf(Scan(0x29, vk::kOemEnlw)), "Backquote", "半角/全角, the other time");
    CheckEq(CodeOf(Scan(0x29, vk::kHanja)), "Backquote", "Alt + 半角/全角 is VK_KANJI");
    CheckEq(CodeOf(Scan(0x29, 0xC0)), "Backquote", "the ` key of a US layout");
    CheckEq(CodeOf(Scan(0x72, vk::kHangul)), "Lang1", "かな, 한/영");
    CheckEq(CodeOf(Scan(0xF2, vk::kHangul)), "Lang1", "한/영 on a PS/2 keyboard");
    CheckEq(CodeOf(Scan(0x71, vk::kHanja)), "Lang2", "英数, 한자");
    CheckEq(CodeOf(Scan(0x73, 0xE2)), "IntlRo", "ろ");
    CheckEq(CodeOf(Scan(0x7D, 0xDC)), "IntlYen", "¥");
    CheckEq(CodeOf(Scan(0x3A, vk::kCapital)), "CapsLock", "Caps Lock");
    CheckEq(CodeOf(Scan(0x3A, vk::kDbeAlphanumeric)), "CapsLock", "the Japanese layout's unshifted Caps Lock key");
    CheckEq(CodeOf(Scan(0x38, vk::kMenu, true)), "AltRight", "right Alt");
    CheckEq(CodeOf(Scan(0x38, vk::kMenu)), "AltLeft", "left Alt");
    CheckEq(CodeOf(Scan(0x36, vk::kShift)), "ShiftRight", "right Shift");
    CheckEq(CodeOf(Scan(0x2A, vk::kShift)), "ShiftLeft", "left Shift");
    CheckEq(CodeOf(Scan(0x1D, vk::kControl, true)), "ControlRight", "right Control");
    CheckEq(CodeOf(Scan(0x1C, vk::kReturn, true)), "Enter", "the keypad's Enter is Enter");
    CheckEq(CodeOf(Scan(0x48, vk::kUp, true)), "ArrowUp", "the arrows");
    CheckEq(CodeOf(Scan(0x48, vk::kNumpad0 + 8)), "Numpad8", "the keypad");
    CheckEq(CodeOf(Scan(0x40, vk::kF1 + 5)), "F6", "F keys");
    CheckEq(CodeOf(Scan(0x64, vk::kF1 + 12)), "F13", "F13");
    CheckEq(CodeOf(Scan(0x39, vk::kSpace)), "Space", "Space");
    CheckEq(CodeOf(Scan(0, vk::kPacket)), "Unidentified", "a character injected as Unicode");
    CheckEq(CodeOf(Scan(0x1E, vk::kProcessKey)), "", "a key another input method took is no key press");
    CheckEq(CodeOf(Scan(0x5E, 0xFF)), "Unidentified", "a key the tables don't know");
    // Injected keys without a scan code go by their virtual key.
    CheckEq(CodeOf(Scan(0, vk::kConvert)), "Convert", "VK_CONVERT alone");
    CheckEq(CodeOf(Scan(0, vk::kNonConvert)), "NonConvert", "VK_NONCONVERT alone");
    CheckEq(CodeOf(Scan(0, vk::kCapital)), "CapsLock", "VK_CAPITAL alone");
    CheckEq(CodeOf(Scan(0, vk::kHangul)), "Lang1", "VK_HANGUL alone");
    Check(ScanOrMapped('H', 0, [](unsigned) { return 0x23u; }) == 0x23 && ScanOrMapped('H', 0x10, nullptr) == 0x10 &&
              ScanOrMapped(vk::kPacket, 0, [](unsigned) { return 0x23u; }) == 0,
          "a key without a scan code gets the layout's");
    Check(IsLanguageKey("Convert") && !IsLanguageKey("CapsLock") && IsHarnessKey("CapsLock"), "language keys and the lock");
    Check(IsModifierCode("ShiftRight") && IsModifierCode("NumLock") && !IsModifierCode("CapsLock"), "modifiers");
    Check(ReleasesLate(vk::kOemAuto) && ReleasesLate(vk::kDbeAlphanumeric) && !ReleasesLate(vk::kConvert),
          "the Japanese layout's lock-like keys");
    CheckEq(Capitals("a\xC3\xA9z1"), "A\xC3\x89Z1", "capitals");
    Check(Scalars("한a\xF0\x9F\x98\x80") == 3 && ByteOffset("한a", 1) == 3, "positions are scalar values");
    // The same key-down, tested twice by TSF.
    const std::intptr_t first = 0x001E0001, repeat = 0x401E0001;
    Check(SameKeyDown('A', first, 100, 5000, 'A', first, 100, 5000), "the same message is the same press");
    Check(SameKeyDown('A', first, 100, 5000, 'A', first, 130, 5030), "a first press tested again a moment later");
    Check(!SameKeyDown('A', first, 100, 5000, 'A', first, 900, 5800), "…but not much later");
    Check(!SameKeyDown('A', repeat, 100, 5000, 'A', repeat, 130, 5030), "a repeat is another press");
    Check(!SameKeyDown('A', first, 100, 5000, 'B', first, 100, 5000), "another key");
}

void TestDescribe() {
    Describe d;
    std::string error;
    Check(ReadDescribe(&d, &error), "the linked engine is bada-compatible 2: " + error);
    CheckEq(d.engine_id, "com.teammirae.bada", "the engine's id");
    Check(d.abi_major == 2, "abi.major is 2");
    // An engine of another major version is refused (§1).
    Check(!ParseDescribe(R"({"abi": {"name": "bada-compatible", "major": 3, "minor": 0}})", &d, &error),
          "major 3 is refused");
    Check(!ParseDescribe(R"({"abi": {"name": "bada-compatible", "major": 1, "minor": 3}})", &d, &error),
          "major 1 is refused");
    Check(!ParseDescribe(R"({"abi": {"name": "something-else", "major": 2}})", &d, &error), "another standard is refused");
    Check(!ParseDescribe("nonsense", &d, &error), "what isn't JSON is refused");
    Check(ParseDescribe(R"({"abi": {"name": "bada-compatible", "major": 2, "minor": 7}})", &d, &error),
          "a later minor version is taken");
    const auto init = json::Parse(InitJson("0.1.0"));
    Check(init && init->Get("harness")->GetString("id") == "com.teammirae.bada.windows" &&
              init->Get("harness")->GetString("version") == "0.1.0" && init->Get("harness")->Get("features")->array.size() == 3,
          "the init object names this harness");
}

void TestKeyTableProblems() {
    std::vector<Problem> problems;
    auto parse = [&](const char* text) {
        problems.clear();
        const auto keys = json::Parse(text);
        return KeyTable::Parse(&*keys, Preset::kJisPc, &problems);
    };
    auto has = [&](const char* path, const char* code) {
        for (const Problem& p : problems) {
            if (p.path == path && p.code == code && p.level == "error") return true;
        }
        return false;
    };
    Check(!parse(R"({"bindings": [{"on": "lang1", "do": ["fix"]}]})") && has("keys.bindings[0].on", "bad-value"),
          "a key that isn't a code is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1", "do": ["jump"]}]})") && has("keys.bindings[0].do[0]", "unknown-action"),
          "an action the standard doesn't have is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1", "do": []}]})") && has("keys.bindings[0].do", "bad-value"),
          "an empty do is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1"}]})") && has("keys.bindings[0].do", "bad-value"), "a binding with no do is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1", "do": ["fix"], "then": 1}]})") && has("keys.bindings[0].then", "unknown-field"),
          "a field a binding doesn't have is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1+Lang1", "do": ["fix"]}]})"), "a pair is two different keys");
    Check(!parse(R"({"bindings": [{"on": "shift+shift+KeyA", "do": ["fix"]}]})") && has("keys.bindings[0].on", "bad-value"),
          "a modifier named twice is refused");
    Check(!parse(R"({"bindings": [{"on": "Lang1", "do": ["fix"], "when": "sometimes"}]})") &&
              has("keys.bindings[0].when", "bad-value"),
          "a when that isn't one is refused");
    Check(!parse(R"({"caps_lock": "toggle"})") && has("keys.caps_lock", "bad-value"), "v1's caps_lock toggle is gone");
    Check(!parse(R"({"shortcuts": []})") && has("keys.shortcuts", "unknown-field"), "a field the table doesn't have");
    const auto table = parse(R"({"bindings": [{"on": "alt+shift+F5", "do": ["ja-kana.katakana", "pass"]},
                                              {"on": "Convert+Lang1", "do": ["profile:a-1"], "when": "profile:b"}],
                                 "cycle": ["a", "b"], "henkan_enters": "b", "caps_lock": "transient", "caps_rule": "upper_snake"})");
    Check(table.has_value() && problems.empty(), "a table with every part is read");
    if (table) {
        Check(table->bindings[0].mods.alt && table->bindings[0].mods.shift && !table->bindings[0].mods.ctrl &&
                  table->bindings[0].key == "F5",
              "modifiers and the key");
        TableState in_b;
        in_b.mode = Mode::kHenkan;
        in_b.profile = "b";
        Check(table->HasPair("Lang1", "Convert", in_b) && table->InBoundPair("Convert", in_b) &&
                  !table->InBoundPair("KanaMode", in_b),
              "pairs");
        Check(!table->InBoundPair("Convert", TableState{}), "a pair bound for a profile makes no key wait elsewhere");
        Check(table->caps_lock == CapsSetting::kTransient && table->caps_rule == CapsRule::kUpperSnake, "caps settings");
    }
    // No `bindings`: the keyboard's preset (§9.5), with the other fields as written.
    const auto preset = parse(R"({"cycle": ["x"]})");
    Check(preset && preset->bindings.size() == 7 && preset->bindings[0].key == "NonConvert" && preset->cycle.size() == 1,
          "no bindings: the JIS PC preset");
    // An empty list is a list: no bindings at all.
    const auto none = parse(R"({"bindings": []})");
    Check(none && none->bindings.empty(), "an empty bindings list binds nothing");
}

void TestSettingsFolder() {
    const fs::path dir = TempDir("folder");
    std::string error;
    const auto data = EngineData::Load(&error);
    const auto session = EngineSession::Create(data.get(), InitJson("0.1.0"), &error);
    Check(session != nullptr, "an engine session: " + error);
    if (!session) return;
    // The text service found a Japanese keyboard: `auto` is the JIS PC preset.
    SettingsFolder folder(dir, Preset::kJisPc);

    // A new install: no file. No muhenkan layout, no profile, the keyboard's preset (§8.3).
    folder.Reload(*session);
    Check(folder.loaded().profiles.empty() && folder.loaded().muhenkan_layout.empty(), "a new install has no profile");
    Check(folder.loaded().table.bindings.size() == 7, "a new install on a Japanese keyboard has the JIS PC preset");
    Check(folder.problems().empty(), "a new install has no problems");
    Check(!folder.Reload(*session), "nothing changed: nothing is read");
    Check(!fs::exists(dir / "settings.json") && fs::is_empty(dir), "the harness writes nothing in the folder");
    {
        SettingsFolder other(dir);
        other.Reload(*session);
        Check(other.loaded().table.bindings.size() == 1 && other.loaded().table.bindings[0].key == "CapsLock",
              "auto on any other keyboard: Caps Lock toggles");
    }

    // A file appears: it is read at the next look.
    WriteFile(dir / "settings.json", kSettings);
    Check(folder.Reload(*session), "the new file is read");
    Check(folder.loaded().profiles.size() == 3 && folder.loaded().profiles[0].id == "ko" &&
              folder.loaded().profiles[0].name == "한국어" && folder.loaded().profiles[2].layouts.size() == 2,
          "the profiles are the engine's");
    CheckEq(folder.loaded().muhenkan_layout, "en-qwerty", "the muhenkan layout");
    Check(folder.loaded().table.bindings.size() == 13 && folder.loaded().table.caps_rule == CapsRule::kUpperSnake,
          "the key table is the file's");
    Check(folder.loaded().harness.keyboard == Keyboard::kJisPc && folder.loaded().harness.publish_state &&
              !folder.loaded().harness.log,
          "the harness's own part");
    Check(folder.problems().empty(), "a good file has no problems");

    // A file that can't be used changes nothing, and its problems are kept (§10.1).
    WriteFile(dir / "settings.json", "{ not json");
    Check(folder.Reload(*session), "a changed file is looked at");
    Check(folder.loaded().profiles.size() == 3, "invalid JSON changes nothing");
    Check(folder.problems().size() == 1 && folder.problems()[0].code == "bad-json" && folder.problems()[0].file == "settings.json" &&
              folder.problems()[0].level == "error",
          "invalid JSON is a problem");
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/1", "engine": {}})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3 && folder.problems().size() == 1 && folder.problems()[0].code == "bad-format",
          "another format changes nothing");
    // The engine refuses its part: nothing of the file is taken, the key table included.
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"henkan": {"profiles": [{"id": "x", "name": "x", "layouts": ["ko-nothing"]}]}},
        "keys": {"bindings": []}})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3 && folder.loaded().table.bindings.size() == 13,
          "settings the engine refuses change nothing");
    bool found = false;
    for (const Problem& p : folder.problems()) {
        found |= p.code == "unknown-layout" && p.path == "engine.henkan.profiles[0].layouts[0]" && p.level == "error";
    }
    Check(found, "the engine's problem is kept, with its path in the file");
    // A key table that can't be read: the same.
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "engine": {},
        "keys": {"bindings": [{"on": "Lang1", "do": ["dance"]}]}})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3, "a key table with an error changes nothing, the engine's part included");
    Check(!folder.problems().empty() && folder.problems()[0].path == "keys.bindings[0].do[0]" &&
              folder.problems()[0].code == "unknown-action" && folder.problems()[0].level == "error",
          "the key table's problem is kept");
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "keys": {"bindings": [], "extra": 1}})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3 && !folder.problems().empty() && folder.problems()[0].path == "keys.extra" &&
              folder.problems()[0].code == "unknown-field" && folder.problems()[0].level == "error",
          "a field the key table doesn't have is an error");
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2",
        "harness": {"com.teammirae.bada.windows": {"keyboard": "jis"}}})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3 && !folder.problems().empty() &&
              folder.problems()[0].path == "harness.com.teammirae.bada.windows.keyboard",
          "a keyboard the harness doesn't know is an error");
    // A profile the table names that isn't there is a warning: the file is taken.
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"henkan": {"profiles": [{"id": "ko", "name": "ko", "layouts": ["ko-2-set"]}]}},
        "keys": {"bindings": [{"on": "Lang1", "do": ["profile:gone"]}, {"on": "Lang2", "do": ["fix"], "when": "profile:lost"}],
                 "cycle": ["ko", "gone"], "henkan_enters": "away"},
        "harness": {"com.teammirae.bada.windows": {"keyboard": "none", "publish_state": false, "log": true, "new": 1},
                    "com.teammirae.bada.mac": {"x": 1}},
        "later": 1})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 1 && folder.loaded().table.bindings.size() == 2, "a file with warnings is taken");
    Check(folder.loaded().harness.keyboard == Keyboard::kNone && !folder.loaded().harness.publish_state &&
              folder.loaded().harness.log,
          "the harness's own part is read, and only its own");
    int warnings = 0, unknown_profiles = 0;
    for (const Problem& p : folder.problems()) {
        warnings += p.level == "warning";
        unknown_profiles += p.code == "unknown-profile";
    }
    Check(warnings == 6 && folder.problems().size() == 6 && unknown_profiles == 4,
          "a profile that isn't there (in do, when, cycle and henkan_enters) and unknown parts are warnings");
    {
        // …and the action that names it just doesn't apply.
        TableState state;
        const Decision d = Decide(folder.loaded().table, folder.loaded().profiles, state, *ParsePress("Lang1"));
        Check(d.then == Decision::Then::kSwallow && d.binding == 0 && d.ran.empty(), "an action on a profile that isn't there doesn't apply");
    }

    // A byte order mark, as Windows' editors write one, is no problem.
    WriteFile(dir / "settings.json", std::string("\xEF\xBB\xBF") + R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-dvorak"}}})");
    folder.Reload(*session);
    Check(folder.problems().empty() && folder.loaded().muhenkan_layout == "en-dvorak", "a file with a byte order mark is read");

    // A custom layout is fed before the settings, so the settings may name it (§7.6).
    std::string stock;
    ReadFile(fs::path(BADA_ROOT) / "engine" / "layouts" / "en-qwerty.json", &stock);
    auto layout = json::Parse(stock);
    Check(layout.has_value(), "a stock layout file is read");
    if (layout) {
        layout->Set("id", json::Str("en-mine"));
        WriteFile(dir / "layouts" / "mine.json", json::Write(*layout));
        WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-mine"}}})");
        folder.Reload(*session);
        CheckEq(folder.loaded().muhenkan_layout, "en-mine", "the settings name a custom layout");
        auto listed = [&](const char* id) {
            const auto reply = json::Parse(session->Send(R"({"type": "layout"})"));
            for (const json::Value& l : reply->Get("layouts")->array) {
                if (l.GetString("id") == id) return true;
            }
            return false;
        };
        Check(listed("en-mine"), "the engine has the custom layout");
        bool noted = false;
        for (const Problem& p : folder.problems()) noted |= p.file == "layouts/mine.json" && p.code == "no-model" && p.level == "warning";
        Check(noted, "the engine's warning about a custom layout is kept, with its file");
        // A layout that can't be read: the one loaded stays.
        WriteFile(dir / "layouts" / "mine.json", "[1, 2");
        folder.Reload(*session);
        Check(listed("en-mine"), "a broken layout file changes nothing");
        // Removed: the settings that still name it are sent again, refused, and say so.
        fs::remove(dir / "layouts" / "mine.json");
        Check(folder.Reload(*session), "a layout file removed is seen");
        Check(!listed("en-mine"), "a layout file removed is a layout removed");
        bool missing = false;
        for (const Problem& p : folder.problems()) missing |= p.code == "unknown-layout" && p.path == "engine.muhenkan.layout";
        Check(missing, "settings that name a layout that is gone are a problem");
        WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "engine": {}})");
        folder.Reload(*session);
        Check(folder.problems().empty(), "…until the settings stop naming it");
    }

    // The user dictionary.
    WriteFile(dir / "dictionary.json", R"({"format": "bada-dictionary", "version": 1})");
    Check(folder.Reload(*session), "the dictionary is read");
    const auto dictionary = json::Parse(session->Send(R"({"type": "dictionary"})"));
    Check(dictionary && dictionary->GetBool("ok"), "the dictionary event answers");
    WriteFile(dir / "dictionary.json", "nope");
    folder.Reload(*session);
    bool bad = false;
    for (const Problem& p : folder.problems()) bad |= p.file == "dictionary.json" && p.code == "bad-json";
    Check(bad, "a dictionary that isn't JSON is a problem");
    fs::remove(dir / "dictionary.json");
    folder.Reload(*session);
    Check(folder.problems().empty(), "a dictionary removed takes its problem with it");

    // A first file that can't be used: nothing is in effect but the keyboard's preset.
    const fs::path broken = TempDir("broken");
    WriteFile(broken / "settings.json", "{");
    SettingsFolder second(broken, Preset::kJisPc);
    second.Reload(*session);
    Check(second.loaded().profiles.empty() && second.loaded().table.bindings.size() == 7 && second.problems().size() == 1,
          "a first file that can't be used leaves a new install, and its problem");

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(broken, ec);
}

void TestApply() {
    // A reply applied to the model, as §6.3 has it.
    test::TextModel field;
    field.Insert("ab");
    ApplyOnce(ParseReply(R"({"ok": true, "preedit": {"text": "한국", "caret": 1}, "staged": "한국"})"), field);
    Check(field.Text() == "ab한국" && field.Marked() == "한국" && field.from == 3, "a preedit is marked, with its caret");
    Check(field.begun == 1 && field.ended == 1, "a reply is one step for the field");
    ApplyOnce(ParseReply(R"({"ok": false, "error": {"code": "bad-row"}})"), field);
    CheckEq(field.Marked(), "한국", "a refused event changes nothing");
    ApplyOnce(ParseReply(R"({"ok": true, "commit": "韓國", "preedit": {"text": "ㅇ"}})"), field);
    Check(field.Text() == "ab韓國ㅇ" && field.Marked() == "ㅇ", "a commit replaces the marked text, then the new preedit");
    ApplyOnce(ParseReply(R"({"ok": true, "menu": {"rows": [{"text": "a", "entry": 0, "note": "n"}], "selected": 0, "strip": true}, "preedit": {"text": "ㅇ"}})"),
              field);
    Check(field.menu.rows.size() == 1 && field.menu.strip && field.menu.selected == 0u && field.menu.rows[0].note == "n",
          "a menu with a note and a strip");
    // Segments: fixed, picked and live, counted in scalar values.
    const Reply cut = ParseReply(R"({"ok": true, "preedit": {"text": "あなた😀inter", "caret": 9,
        "segments": [{"len": 3, "kind": "fixed"}, {"len": 1, "kind": "picked"}, {"len": 5, "kind": "live"}]}})");
    Check(cut.preedit.segments.size() == 3 && cut.preedit.segments[0].kind == Segment::Kind::kFixed &&
              cut.preedit.segments[1].kind == Segment::Kind::kPicked && cut.preedit.segments[2].kind == Segment::Kind::kLive &&
              cut.preedit.segments[2].len == 5 && cut.preedit.caret == 9,
          "segments are read");
    // A panic: the engine reset itself. The marked text goes, and the menu closes.
    ApplyOnce(ParseReply(R"({"ok": false, "error": {"code": "panic", "message": "boom"}})"), field);
    Check(field.Text() == "ab韓國" && !field.marked && field.menu.rows.empty(), "a panic clears the marked text and the menu");
    // replace: "selection" replaces the selection even when text is marked.
    field.from = 0;
    field.to = 2;
    ApplyOnce(ParseReply(R"({"ok": true, "commit": "ＡＢ", "replace": "selection"})"), field);
    CheckEq(field.Text(), "ＡＢ韓國", "replace: selection");
    // An ask is answered once, with the selection, and never deeper.
    field.Select("xy");
    int sent = 0;
    const Reply shown = Apply(ParseReply(R"({"ok": true, "ask": {"around": {"before": 1, "after": 9}, "selection": "full"}})"), field,
                              [&](const std::string& event) {
                                  ++sent;
                                  const auto e = json::Parse(event);
                                  Check(e->GetString("type") == "text" && e->GetString("selection") == "xy" &&
                                            e->GetString("before") == "Ｂ" && e->GetString("after") == "韓國",
                                        "the text event carries what was asked: " + event);
                                  // The answer asks again: it must not be answered.
                                  return ParseReply(R"({"ok": true, "commit": "!", "replace": "selection", "ask": {"selection": "more"}})");
                              });
    Check(sent == 1 && field.Text() == "ＡＢ!韓國" && shown.commit == "!", "an ask is answered once");
    const Reply nothing = ParseReply("not a reply");
    Check(!nothing.ok && nothing.error_code == "bad-event", "what isn't a reply reads as a refusal");
}

void TestNewInstall() {
    Rig rig("new", nullptr, Preset::kJisPc);
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    // No muhenkan layout: the engine has no say, and Windows types the key.
    Check(!ctx.KeyDown(Key("KeyA", "a"), field, {}), "with no muhenkan layout a key goes to the app");
    // 変換 would enter henkan, and there is no profile: the onboarding, and the engine isn't asked.
    Check(ctx.KeyDown(Key("Convert"), field, {}), "変換 is taken");
    Check(rig.session->mode() == Mode::kMuhenkan, "with no profile, 変換 doesn't enter henkan");
    Check(!field.notice.empty() && field.notice.find("settings.json") != std::string::npos, "the text service is given a notice");
    Check(!rig.log.empty() && rig.log.back().find("no henkan profile") != std::string::npos, "the onboarding is logged");
    CheckEq(rig.State().Get("harness")->GetString("notice"), field.notice, "the notice is in the state");
    // The next key takes the notice away.
    ctx.KeyDown(Key("KeyB", "b"), field, {});
    Check(field.notice.empty(), "the notice goes at the next key");
    // The README's settings, written by hand: the key works from then on.
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-qwerty"},
        "henkan": {"profiles": [{"id": "main", "name": "日本語", "layouts": ["ja-romaji"]}]}}})");
    Check(rig.session->Poll(1000), "the folder is looked at with a key, and the file is read");
    Check(!rig.session->Poll(1100), "…but not at every key");
    Check(ctx.KeyDown(Key("Convert"), field, {}) && rig.session->mode() == Mode::kHenkan && rig.session->profile() == "main",
          "変換 enters the first profile");
    CheckEq(rig.session->Label(), "日", "the indicator is the profile's first character");
    CheckEq(rig.session->ModeText(), "日本語", "the profile's name");
    Type(ctx, field, {{"KeyK", "k"}, {"KeyA", "a"}});
    CheckEq(field.Marked(), "か", "romaji: ka is か");
    Check(rig.State().Get("harness")->GetString("notice").empty(), "the notice is gone from the state");
}

void TestTyping() {
    Rig rig("typing");
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    const ContextInfo info{"app.exe", false};

    // Muhenkan (§6.4): Windows types the key when its layout gives the same text.
    Check(!ctx.KeyDown(Key("KeyH", "h"), field, info), "Windows types h itself");
    Check(field.commits.empty(), "…and the harness commits nothing");
    // Another layout in Windows (Dvorak has d on the H key): the harness types what the layout says.
    Check(ctx.KeyDown(Key("KeyH", "d"), field, info) && field.Text() == "h", "the harness types h under another Windows layout");
    // The JIS table, whatever Windows has: ^ on the Equal key, " on ⇧2, \ on ろ, ¥.
    Check(ctx.KeyDown(Key("Equal", "="), field, info) && ctx.KeyDown(Key("Digit2", "@", kShift), field, info) &&
              ctx.KeyDown(Key("IntlRo", ""), field, info) && ctx.KeyDown(Key("IntlYen", "\\"), field, info),
          "JIS symbols are typed by the harness under a US layout");
    CheckEq(field.Text(), "h^\"\\\xC2\xA5", "the JIS characters");
    // Under the Japanese layout Windows types the same, so the keys are the app's.
    Check(!ctx.KeyDown(Key("Equal", "^"), field, info) && !ctx.KeyDown(Key("Digit2", "\"", kShift), field, info),
          "under the Japanese layout Windows types the JIS symbols itself");
    // ⇧0 is the one empty slot: it types nothing, and the app doesn't get it.
    Check(ctx.KeyDown(Key("Digit0", ")", kShift), field, info) && field.Text() == "h^\"\\\xC2\xA5", "⇧0 types nothing");
    // A shortcut is passed on as the physical key.
    Check(!ctx.KeyDown(Key("KeyC", "c", kCtrl), field, info), "⌃C goes to the app");
    // Space, Enter and the like: Windows types them.
    Check(!ctx.KeyDown(Key("Space", " "), field, info) && !ctx.KeyDown(Key("Enter", "\r"), field, info), "Space and Enter go to the app");
    // A modifier on its own is never the engine's, and never eaten.
    Check(!ctx.KeyDown(Key("ShiftLeft"), field, info) && !ctx.KeyUp(Key("ShiftLeft"), field, info), "a modifier's press reaches the app");
    // A key another input method took, and a character injected as Unicode.
    RawKey odd;
    odd.vk = vk::kProcessKey;
    Check(!ctx.KeyDown(odd, field, info), "VK_PROCESSKEY is nobody's");
    odd.vk = vk::kPacket;
    Check(!ctx.KeyDown(odd, field, info), "an injected character passes");

    // 変換: henkan, the first profile (Korean 2-set). gks is 한; Space commits and goes to the app.
    field = test::TextModel();
    Check(ctx.KeyDown(Key("Convert"), field, info), "変換 is the table's");
    Check(rig.session->mode() == Mode::kHenkan && rig.session->profile() == "ko", "変換 enters the first profile");
    Type(ctx, field, {{"KeyG", "g"}, {"KeyK", "k"}, {"KeyS", "s"}}, info);
    Check(field.Marked() == "한" && field.Text() == "한", "gks is 한, marked");
    const json::Value state = rig.State();
    Check(state.GetString("format") == "bada-compatible-state/2" && state.GetString("mode") == "henkan" &&
              state.GetString("profile") == "ko" && state.GetString("staged") == "한" &&
              state.Get("preedit")->GetString("text") == "한",
          "the state is published while composing: " + json::Write(state));
    const json::Value* h = state.Get("harness");
    Check(h && h->GetString("id") == "com.teammirae.bada.windows" && h->GetString("version") == "0.1.0" &&
              h->GetString("app") == "app.exe" && h->GetBool("active") && !h->GetBool("secure"),
          "the harness's part of the state");
    Check(h && h->Get("keys")->Get("available")->array.size() == 7 && h->Get("keys")->GetBool("chords") &&
              h->Get("keys")->Get("missing")->array.empty() && h->Get("problems")->array.empty(),
          "harness.keys and harness.problems");
    Check(h && h->Get("caps_lock")->GetString("setting") == "respect" && h->Get("caps_lock")->GetString("rule") == "upper_snake" &&
              !h->Get("caps_lock")->GetBool("on"),
          "harness.caps_lock");
    Check(rig.sink.summaries.back().mode == Mode::kHenkan && rig.sink.summaries.back().active &&
              rig.sink.summaries.back().composing && !rig.sink.summaries.back().secure,
          "the summary of the state");
    Type(ctx, field, {{"Space", " "}}, info);
    Check(field.Text() == "한 " && !field.marked, "Space commits 한 and then types a space");
    Check(!rig.sink.summaries.back().composing, "nothing is staged after the commit");
    // A language key never reaches the engine: bound, its action runs; with nothing to do, swallowed.
    Check(ctx.KeyDown(Key("Convert"), field, info) && field.Text() == "한 ", "変換 with no menu does nothing in the first profile");

    // 無変換: a mode switch through the key table. What is staged is committed by the select.
    Type(ctx, field, {{"KeyG", "g"}, {"KeyK", "k"}}, info);
    Check(ctx.KeyDown(Key("NonConvert"), field, info), "無変換 is the table's");
    Check(rig.session->mode() == Mode::kMuhenkan && field.Text() == "한 하" && !field.marked, "無変換 commits and leaves henkan");
    CheckEq(rig.session->Label(), "A", "the indicator in muhenkan");

    // カタカナ: a profile switch (profile:ja). Romaji.
    Check(ctx.KeyDown(Key("KanaMode"), field, info) && rig.session->profile() == "ja", "カタカナ enters its profile");
    Type(ctx, field, {{"KeyK", "k"}, {"KeyA", "a"}}, info);
    CheckEq(field.Marked(), "か", "ka is か");
    // A chord (F13) switches profile again, committing what was staged.
    Check(ctx.KeyDown(Key("F13"), field, info) && rig.session->profile() == "work" && field.Text() == "한 하か",
          "a profile switch commits what is staged");
    // Two layouts in the profile: a menu of both readings. 変換 is the next entry, ⇧変換 the previous.
    Type(ctx, field, {{"KeyG", "g"}, {"KeyK", "k"}}, info);
    Check(field.menu.rows.size() == 2 && field.menu.selected == 0u && field.Marked() == "하", "a menu of both readings");
    ctx.KeyDown(Key("Convert"), field, info);
    Check(field.menu.selected == 1u && field.Marked() == "gk", "変換 moves to the next row");
    ctx.KeyDown(Key("Convert", "", kShift), field, info);
    Check(field.menu.selected == 0u && field.Marked() == "하", "⇧変換 moves back");
    // A click on a row: pick, then commit.
    ctx.Pick(1, field, info);
    Check(field.Text() == "한 하かgk" && !field.marked && field.menu.rows.empty(), "a click on a row commits it");

    // 半角/全角 with nothing staged asks for the selection, once (§6.3), and replaces it.
    field.Select("abc");
    Check(ctx.KeyDown(Key("Backquote", "`"), field, info), "半角/全角 is the table's");
    CheckEq(field.Text(), "한 하かgkａｂｃ", "the selection is made full-width");

    // The app ended the composition by itself: the engine drops what it had, and commits nothing.
    Type(ctx, field, {{"KeyG", "g"}}, info);
    Check(ctx.composing(), "something is staged");
    const size_t commits = field.commits.size();
    ctx.Forget();
    Check(!ctx.composing() && field.commits.size() == commits, "forgetting commits nothing");
    field.SetPreedit(Preedit{});
    Type(ctx, field, {{"KeyK", "k"}}, info);
    CheckEq(field.Marked(), "ㅏ", "the engine starts fresh");
    ctx.Flush(field, "click");
    Check(field.Text() == "한 하かgkａｂｃㅏ" && !field.marked, "a click commits what is staged");

    // Focus leaves: what is staged is committed.
    Type(ctx, field, {{"KeyG", "g"}}, info);
    ctx.FocusOut(field);
    Check(field.Text() == "한 하かgkａｂｃㅏㅎ" && !field.marked, "focus out commits what is staged");
    Check(!rig.State().Get("harness")->GetBool("active") && rig.State().GetString("staged").empty(),
          "with no field in focus the state shows nothing");
    Check(!rig.sink.summaries.back().active, "…and its summary says so");
    // The indicator and the menu.
    ctx.Toggle(field, info);
    Check(rig.session->mode() == Mode::kMuhenkan, "the indicator's click leaves henkan");
    ctx.Toggle(field, info);
    Check(rig.session->mode() == Mode::kHenkan && rig.session->profile() == "work", "…and enters the profile last used");
    ctx.Select(Mode::kHenkan, "ja", field, info);
    Check(rig.session->profile() == "ja", "the menu picks a profile");
    ctx.Select(Mode::kHenkan, "nobody", field, info);
    Check(rig.session->profile() == "ja", "a profile that isn't there is not picked");
    ctx.Select(Mode::kMuhenkan, "", field, info);
    Check(rig.session->mode() == Mode::kMuhenkan && rig.session->profile() == "ja", "muhenkan keeps the profile for later");
}

// The engine's data (§11): the files of a folder, each the buffer of its name.
void TestData() {
    std::string error;
    const fs::path data = fs::path(BADA_ROOT) / "engine" / "data";
    const auto none = EngineData::Load(&error);
    Check(none && none->names().empty(), "no folder: no buffers, and the engine still starts");
    const auto missing = EngineData::Load(&error, TempDir("no-data") / "nothing-here");
    Check(missing && missing->names().empty(), "a folder that isn't there is no data");
    const auto all = EngineData::Load(&error, data);
    Check(all != nullptr, "the data loads: " + error);
    if (!all) return;
    std::string names;
    for (const std::string& name : all->names()) names += name + " ";
    const auto has = [&](const char* name) {
        return std::find(all->names().begin(), all->names().end(), name) != all->names().end();
    };
    Check(all->names().size() == 13 && has("kanji") && has("ko-words") && has("key-models") && !has("en-ko"),
          "every buffer the engine knows that has a file: " + names);

    // A session on the data converts kana to kanji; one without it types the kana.
    for (const bool with : {false, true}) {
        Rig rig(with ? "with-data" : "without-data", kSettings, Preset::kNoLanguageKeys, with ? data : fs::path());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        const ContextInfo info{"app", false};
        Check(ctx.KeyDown(Key("KanaMode"), field, info) && rig.session->profile() == "ja", "the romaji profile");
        Type(ctx, field, {{"KeyK", "k"}, {"KeyA", "a"}, {"KeyN", "n"}, {"KeyJ", "j"}, {"KeyI", "i"}}, info);
        bool kanji = false;
        for (const auto& row : field.menu.rows) kanji |= row.text == "漢字";
        const bool logged = std::any_of(rig.log.begin(), rig.log.end(), [&](const std::string& line) {
            return line.rfind("data: ", 0) == 0 && (line.find(" kanji") != std::string::npos) == with;
        });
        if (with) {
            Check(kanji && field.menu.rows.size() > 2, "with the data, kanji is 漢字 among the rows");
            Check(field.Marked() != "かんじ", "…and a conversion shows first: " + field.Marked());
        } else {
            Check(!kanji && field.menu.rows.empty() && field.Marked() == "かんじ", "without it, the kana and no menu");
        }
        Check(logged, "the log says which buffers were passed");
        ctx.FocusOut(field);
    }
}

// A key the user binds to the entry before, or else the toggle (§9.2): ⇧Tab here. No preset has it.
void TestToggleKey() {
    const char* settings = R"({
      "format": "bada-settings/2",
      "engine": {
        "muhenkan": {"layout": "en-qwerty"},
        "henkan": {"profiles": [
          {"id": "ko", "name": "한국어", "layouts": ["ko-2-set"]},
          {"id": "work", "name": "work", "layouts": ["ko-2-set", "en-qwerty"]}
        ]}
      },
      "keys": {"bindings": [
        {"on": "shift+Tab", "do": ["menu.prev", "mode.toggle"]},
        {"on": "F13", "do": ["profile:work"]}
      ]}
    })";
    Rig rig("toggle-key", settings);
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    const ContextInfo info{"app", false};
    const RawKey shift_tab = Key("Tab", "\t", kShift);

    Check(ctx.KeyDown(shift_tab, field, info) && rig.session->mode() == Mode::kHenkan && rig.session->profile() == "ko",
          "⇧Tab with no menu enters henkan, and the app doesn't get the key");
    ctx.KeyUp(shift_tab, field, info);
    // One candidate is no menu: the toggle, and what was staged is committed.
    Type(ctx, field, {{"KeyG", "g"}, {"KeyK", "k"}, {"KeyS", "s"}}, info);
    Check(field.Marked() == "한" && field.menu.rows.empty(), "one candidate, no menu");
    Check(ctx.KeyDown(shift_tab, field, info) && rig.session->mode() == Mode::kMuhenkan && field.Text() == "한" && !field.marked,
          "⇧Tab with one candidate leaves henkan and commits it");
    ctx.KeyUp(shift_tab, field, info);
    // A menu open: the entry before, and the mode stays.
    Check(ctx.KeyDown(Key("F13"), field, info) && rig.session->profile() == "work", "the profile with two layouts");
    Type(ctx, field, {{"KeyG", "g"}, {"KeyK", "k"}, {"Tab", "\t"}}, info);
    Check(field.menu.rows.size() == 2 && field.menu.selected == 1u && field.Marked() == "gk", "Tab is still the engine's: the next entry");
    Check(ctx.KeyDown(shift_tab, field, info) && rig.session->mode() == Mode::kHenkan && field.menu.selected == 0u &&
              field.Marked() == "하",
          "⇧Tab with a menu open is the entry before");
    ctx.KeyUp(shift_tab, field, info);
    Type(ctx, field, {{"Enter", "\r"}}, info);
    Check(ctx.KeyDown(shift_tab, field, info) && rig.session->mode() == Mode::kMuhenkan && rig.session->profile() == "work",
          "…and the toggle again once the menu is gone");
    ctx.KeyUp(shift_tab, field, info);

    // With the keyboard's preset ⇧Tab has no binding: in muhenkan the app has it, and the mode stays.
    Rig plain("toggle-key-preset", R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-qwerty"},
      "henkan": {"profiles": [{"id": "ko", "name": "한국어", "layouts": ["ko-2-set"]}]}}})");
    if (!plain.session) return;
    Context other(*plain.session);
    test::TextModel text;
    Check(!other.KeyDown(shift_tab, text, info) && plain.session->mode() == Mode::kMuhenkan, "unbound, ⇧Tab goes to the app");
    other.KeyUp(shift_tab, text, info);
}

void TestTwoFieldsAndSecure() {
    Rig rig("fields");
    if (!rig.session) return;
    Context a(*rig.session), b(*rig.session), password(*rig.session);
    test::TextModel fa, fb, fp;
    a.KeyDown(Key("Convert"), fa, {});
    Type(a, fa, {{"KeyG", "g"}});
    // Another field follows the session's mode at its next key: one `select`, after bada_new.
    Type(b, fb, {{"KeyG", "g"}, {"KeyK", "k"}});
    CheckEq(fb.Marked(), "하", "a second field follows the mode");
    // 無変換 there switches for everyone. The first field commits its own when it next sees a key.
    b.KeyDown(Key("NonConvert"), fb, {});
    Check(fb.Text() == "하" && !fb.marked, "the second field commits at the switch");
    Check(fa.Marked() == "ㅎ", "the first field hasn't heard yet");
    Type(a, fa, {{"KeyA", "a"}});
    Check(fa.Text() == "ㅎa" && !fa.marked, "the first field follows: it commits, and types in muhenkan");

    // A password field: nothing composes, whatever the mode, and nothing typed is published.
    a.KeyDown(Key("Convert"), fa, {});
    const ContextInfo secure{"pw.exe", true};
    Type(password, fp, {{"KeyG", "g"}, {"KeyK", "k"}, {"KeyS", "s"}}, secure);
    Check(fp.Text() == "gks" && !fp.marked && fp.commits.empty(), "a password field gets plain keys");
    const json::Value state = rig.State();
    Check(state.Get("harness")->GetBool("secure") && state.GetString("staged").empty() &&
              state.Get("preedit")->GetString("text").empty() && state.Get("menu")->Get("rows")->array.empty(),
          "the state hides a password field");
    Check(rig.sink.summaries.back().secure && !rig.sink.summaries.back().composing, "the summary says it is a password field");
    for (const std::string& s : rig.sink.published) {
        if (s.find("\"secure\":true") != std::string::npos) Check(s.find("gks") == std::string::npos, "no state carried the password");
    }
    // Language keys still switch mode there.
    Check(password.KeyDown(Key("NonConvert"), fp, secure) && rig.session->mode() == Mode::kMuhenkan,
          "a password field still switches mode");
}

void TestPairs() {
    Rig rig("pairs");
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    // かな and 英数 held together and released: the pair's binding, when the second is released.
    Check(ctx.KeyDown(Key("Lang1"), field, {}), "a key of a bound pair is held back");
    Check(ctx.KeyDown(Key("Lang2"), field, {}), "…and so is the other");
    Check(rig.session->mode() == Mode::kMuhenkan, "nothing runs while they are down");
    Check(ctx.KeyUp(Key("Lang1"), field, {}) && rig.session->mode() == Mode::kMuhenkan, "one released: not yet");
    Check(ctx.KeyUp(Key("Lang2"), field, {}), "the release is eaten, as the press was");
    Check(rig.session->mode() == Mode::kHenkan && rig.session->profile() == "work", "both released: the pair's binding ran");
    // A key of a bound pair alone runs its own binding late. Lang2's is `pass`: the app gets the
    // key the harness had held back.
    ctx.KeyDown(Key("Lang2"), field, {});
    Check(field.forwarded.empty(), "held back while it may be a pair");
    ctx.KeyUp(Key("Lang2"), field, {});
    Check(field.forwarded.size() == 1 && CodeOf(field.forwarded[0]) == "Lang2", "released alone: passed on, late");
    // A key that waited and has no binding of its own goes to Windows then (かな, here).
    Check(ctx.KeyDown(Key("Lang1"), field, {}), "かな waits for its pair");
    ctx.KeyUp(Key("Lang1"), field, {});
    Check(field.forwarded.size() == 2 && CodeOf(field.forwarded[1]) == "Lang1", "released alone and unbound: it goes to Windows");
    field.forwarded.pop_back();
    // Another key while one waits: the waiting key's binding first, then the key.
    ctx.KeyDown(Key("Lang2"), field, {});
    Check(ctx.KeyDown(Key("KeyG", "g"), field, {}) && field.forwarded.size() == 2 && field.Marked() == "ㅎ",
          "another key runs the waiting one first");
    ctx.KeyUp(Key("Lang2"), field, {});
    ctx.KeyUp(Key("KeyG"), field, {});
    // A modifier pressed and released alone is a key where a binding says so (ShiftRight: the toggle).
    Check(!ctx.KeyDown(Key("ShiftRight"), field, {}), "a modifier's press is never eaten");
    ctx.KeyUp(Key("ShiftRight"), field, {});
    Check(rig.session->mode() == Mode::kMuhenkan && field.Text() == "ㅎ", "right Shift tapped: the toggle, which commits");
    // …but not when another key came between.
    ctx.KeyDown(Key("ShiftRight"), field, {});
    ctx.KeyDown(Key("KeyA", "A", kShift), field, {});
    ctx.KeyUp(Key("ShiftRight"), field, {});
    Check(rig.session->mode() == Mode::kMuhenkan, "right Shift held for a capital is no tap");
    // A binding runs once for each press: the repeats of a held key that has one are swallowed.
    RawKey repeat = Key("Convert");
    ctx.KeyDown(repeat, field, {});
    repeat.repeat = true;
    Check(ctx.KeyDown(repeat, field, {}) && rig.session->profile() == "ko", "a held language key acts once");
    ctx.KeyUp(repeat, field, {});
    repeat = Key("F13");
    ctx.KeyDown(repeat, field, {});
    repeat.repeat = true;
    Check(ctx.KeyDown(repeat, field, {}) && rig.session->profile() == "work", "a held chord acts once, and its repeats are swallowed");
    ctx.KeyUp(repeat, field, {});
    // …but for a key that was passed on, whose repeats pass too.
    repeat = Key("F14");
    Check(!ctx.KeyDown(repeat, field, {}), "a key bound to pass goes to the app");
    repeat.repeat = true;
    Check(!ctx.KeyDown(repeat, field, {}), "…and so do its repeats");
    ctx.KeyUp(repeat, field, {});
    // A key with no binding repeats as it always did.
    field = test::TextModel();
    repeat = Key("KeyG", "g");
    ctx.KeyDown(repeat, field, {});
    repeat.repeat = true;
    ctx.KeyDown(repeat, field, {});
    Check(bada::Scalars(field.Marked()) == 2, "a held typing key repeats: \"" + field.Marked() + "\"");
    ctx.KeyUp(repeat, field, {});

    // A profile removed while selected: no fallback to another, so muhenkan (§8.1).
    Check(rig.session->mode() == Mode::kHenkan, "still in henkan");
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "other", "name": "o", "layouts": ["ja-kana"]}]}}})");
    rig.session->Reload();
    Check(rig.session->mode() == Mode::kMuhenkan && rig.session->profile().empty(), "the selected profile was removed: muhenkan");
    field = test::TextModel();
    Check(!ctx.KeyDown(Key("KeyB", "b"), field, {}), "the field follows with a select, and types in muhenkan");
    // publish_state false: nothing is published.
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2", "harness": {"com.teammirae.bada.windows": {"publish_state": false}}})");
    rig.session->Reload();
    const size_t published = rig.sink.published.size();
    ctx.KeyDown(Key("KeyB", "b"), field, {});
    Check(rig.sink.cleared == 1 && rig.sink.published.size() == published, "publish_state false: the state is cleared, and stays so");
}

// §9.4. Windows flips the lock itself when the Caps Lock key goes down, whatever the text service
// answers: where the harness decides otherwise, it says what the lock must be, and the text
// service sets it.
void TestCapsLock() {
    const char* base = R"("engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "ko", "name": "ko", "layouts": ["ko-2-set"]}]}})";
    auto settings = [&](const std::string& keys, const char* keyboard) {
        return std::string(R"({"format": "bada-settings/2", )") + base + R"(, "keys": )" + keys +
               R"(, "harness": {"com.teammirae.bada.windows": {"keyboard": ")" + keyboard + R"("}}})";
    };
    {
        // respect, unbound: the key is Windows', and so is the lock.
        Rig rig("caps-respect", settings("{}", "jis-pc").c_str());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        Check(!ctx.KeyDown(Key("CapsLock"), field, {}), "unbound, Caps Lock goes to Windows");
        Check(!rig.session->TakeLockWanted().has_value(), "…and the harness leaves the lock to it");
        ctx.KeyUp(Key("CapsLock", "", {}, true), field, {});
        Check(rig.session->lock() && rig.State().Get("harness")->Get("caps_lock")->GetBool("on"), "the lock is read back, and published");
        // The letters of `types` in capitals.
        Check(!ctx.KeyDown(Key("KeyA", "A", {}, true), field, {}), "with the lock on Windows types A itself");
        // Windows types a small letter for ⇧ with the lock on; the standard has a capital.
        Check(ctx.KeyDown(Key("KeyA", "a", kShift, true), field, {}) && field.Text() == "A", "⇧ with the lock on is still a capital");
        Check(!ctx.KeyDown(Key("Digit1", "1", {}, true), field, {}), "the lock leaves digits alone");
        // respect: henkan doesn't turn it off.
        ctx.KeyDown(Key("Convert", "", {}, true), field, {});
        Check(rig.session->mode() == Mode::kHenkan && !rig.session->TakeLockWanted().has_value() && rig.session->lock(),
              "respect: entering henkan leaves the lock on");
        // The engine gets the same events either way: composition ignores the lock.
        Type(ctx, field, {{"KeyG", "G"}});
        ctx.KeyDown(Key("KeyK", "K", {}, true), field, {});
        Check(field.Marked() == "하" || field.Marked() == "ㅎㅏ" || !field.Marked().empty(), "henkan composes with the lock on");
        // Under the Japanese layout the unshifted key is 英数: Windows flips nothing, and the
        // harness reads that back too.
        ctx.KeyDown(Key("NonConvert", "", {}, true), field, {});
        Check(!ctx.KeyDown(Key("CapsLock", "", {}, true), field, {}), "the key is passed on");
        ctx.KeyUp(Key("CapsLock", "", {}, true), field, {});
        Check(rig.session->lock(), "what Windows did with the key is what the harness has");
    }
    {
        // UPPER_SNAKE: with the lock on in muhenkan, Space types _ and Alt+Space a space.
        Rig rig("caps-snake", settings(R"({"caps_rule": "upper_snake"})", "jis-pc").c_str());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        Check(!ctx.KeyDown(Key("KeyA", "A", {}, true), field, {}), "A");
        field.Insert("A");
        Check(ctx.KeyDown(Key("Space", " ", {}, true), field, {}) && ctx.KeyDown(Key("Space", " ", kAlt, true), field, {}) &&
                  ctx.KeyDown(Key("Space", " ", kShift, true), field, {}) && field.Text() == "A_ _",
              "UPPER_SNAKE: Space is _, ⇧Space too, and Alt+Space a space");
        Check(!ctx.KeyDown(Key("Space", " "), field, {}), "with the lock off Space is the app's");
        ctx.KeyDown(Key("Convert", "", {}, true), field, {});
        Type(ctx, field, {{"KeyG", "g"}});
        Check(!ctx.KeyDown(Key("Space", " ", {}, true), field, {}) && field.Text() == "A_ _ㅎ", "in henkan Space is Space");
    }
    {
        // Bound, Caps Lock stops being the lock: its binding runs, and the lock doesn't change.
        Rig rig("caps-bound", settings("{}", "none").c_str());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        Check(rig.session->table().bindings.size() == 1, "keyboard none: the preset binds Caps Lock to the toggle");
        Check(ctx.KeyDown(Key("CapsLock"), field, {}) && rig.session->mode() == Mode::kHenkan, "Caps Lock toggles in");
        Check(rig.session->TakeLockWanted() == std::optional<bool>(false), "the lock was off, and must stay off");
        Check(!rig.session->TakeLockWanted().has_value(), "…which is said once");
        // Windows flipped it meanwhile: the release reports it on. The harness knows better.
        ctx.KeyUp(Key("CapsLock", "", {}, true), field, {});
        Check(!rig.session->lock(), "a bound Caps Lock isn't read back");
        RawKey held = Key("CapsLock");
        held.repeat = true;
        Check(ctx.KeyDown(Key("CapsLock"), field, {}) && rig.session->mode() == Mode::kMuhenkan, "Caps Lock toggles out");
        Check(ctx.KeyDown(held, field, {}) && rig.session->mode() == Mode::kMuhenkan, "held, it toggles once");
        ctx.KeyUp(held, field, {});
        rig.session->TakeLockWanted();
        // With the lock on before the press, it stays on.
        ctx.KeyDown(Key("KeyA", "A", {}, true), field, {});
        ctx.KeyDown(Key("CapsLock", "", {}, true), field, {});
        Check(rig.session->TakeLockWanted() == std::optional<bool>(true) && rig.session->lock(), "the lock was on, and must stay on");
        // ⇧Caps Lock is another chord: unbound, it is the lock, Windows' own.
        Check(!ctx.KeyDown(Key("CapsLock", "", kShift, true), field, {}), "⇧Caps Lock goes to Windows");
        Check(!rig.session->TakeLockWanted().has_value(), "…which flips the lock itself");
        ctx.KeyUp(Key("CapsLock", "", kShift, false), field, {});
        Check(!rig.session->lock(), "the lock is read back off");
    }
    {
        // Bound to `pass`, it is the lock again.
        Rig rig("caps-pass", settings(R"({"bindings": [{"on": "CapsLock", "do": ["pass"]}]})", "jis-pc").c_str());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        Check(!ctx.KeyDown(Key("CapsLock"), field, {}) && !rig.session->TakeLockWanted().has_value(), "Caps Lock passed on is Windows'");
        RawKey held = Key("CapsLock", "", {}, true);
        held.repeat = true;
        Check(!ctx.KeyDown(held, field, {}), "…and so are its repeats");
    }
    {
        // transient: Caps Lock in muhenkan only.
        Rig rig("caps-transient", settings(R"({"caps_lock": "transient"})", "jis-pc").c_str());
        if (!rig.session) return;
        Context ctx(*rig.session);
        test::TextModel field;
        Check(!ctx.KeyDown(Key("CapsLock"), field, {}) && !rig.session->TakeLockWanted().has_value(), "in muhenkan it is the lock");
        ctx.KeyUp(Key("CapsLock", "", {}, true), field, {});
        Check(rig.session->lock(), "the lock is on");
        // It is turned off when muhenkan is left.
        Check(ctx.KeyDown(Key("Convert", "", {}, true), field, {}) && rig.session->mode() == Mode::kHenkan, "変換");
        Check(rig.session->TakeLockWanted() == std::optional<bool>(false) && !rig.session->lock(), "leaving muhenkan turns the lock off");
        Check(!rig.State().Get("harness")->Get("caps_lock")->GetBool("on") &&
                  rig.State().Get("harness")->Get("caps_lock")->GetString("setting") == "transient",
              "the state says so");
        // Pressed in henkan it does nothing.
        Check(ctx.KeyDown(Key("CapsLock"), field, {}), "in henkan Caps Lock is swallowed");
        Check(rig.session->TakeLockWanted() == std::optional<bool>(false), "…and the lock stays off");
        ctx.KeyUp(Key("CapsLock", "", {}, true), field, {});
        Check(!rig.session->lock(), "what Windows did meanwhile isn't taken");
        // If another program turns it on in henkan, the harness turns it off.
        Type(ctx, field, {{"KeyG", "g"}});
        ctx.KeyDown(Key("KeyK", "K", {}, true), field, {});
        Check(rig.session->TakeLockWanted() == std::optional<bool>(false) && !rig.session->lock(), "the lock turned on from outside is turned off");
        CheckEq(field.Marked(), "하", "…and typing went on");
        // Back in muhenkan the lock is the lock.
        ctx.KeyDown(Key("NonConvert"), field, {});
        Check(!ctx.KeyDown(Key("KeyA", "A", {}, true), field, {}) && !rig.session->TakeLockWanted().has_value(), "in muhenkan the lock is left alone");
    }
}

// The mode and the profile are the session's: every process follows them, by the profile's id.
void TestShared() {
    const Shared shared{Mode::kHenkan, "ja", "ko"};
    const std::string text = WriteShared(shared);
    CheckEq(text, R"({"mode":"henkan","previous":"ko","profile":"ja"})", "what is kept across processes names the profile by its id");
    Check(ReadShared(text) == std::optional<Shared>(shared), "…and reads back");
    Check(ReadShared(WriteShared(Shared{})) == std::optional<Shared>(Shared{}), "muhenkan with no profile yet");
    Check(!ReadShared("").has_value() && !ReadShared("3").has_value() && !ReadShared(R"({"mode": 1})").has_value(),
          "what isn't ours is not read");

    Rig one("shared-1"), two("shared-2");
    if (!one.session || !two.session) return;
    Context a(*one.session), b(*two.session);
    test::TextModel fa, fb;
    Type(b, fb, {{"KeyG", "g"}});
    Check(fb.Text() == "g" && two.session->switches() == 0, "the second process types in muhenkan");
    // The first process switches: it says so, and the second follows.
    a.KeyDown(Key("KanaMode"), fa, {});
    Check(one.session->switches() == 1 && one.session->shared().mode == Mode::kHenkan && one.session->shared().profile == "ja",
          "a switch here is for the others to follow");
    Check(two.session->Adopt(*ReadShared(WriteShared(one.session->shared()))), "the second process takes it");
    Check(two.session->mode() == Mode::kHenkan && two.session->profile() == "ja" && two.session->switches() == 0,
          "…and following is no switch of its own");
    Check(!two.session->Adopt(one.session->shared()), "the same again changes nothing");
    Type(b, fb, {{"KeyK", "k"}, {"KeyA", "a"}});
    CheckEq(fb.Marked(), "か", "its field follows at its next key: one select");
    // Back to muhenkan from the second: its field commits; the first follows and commits its own.
    Type(a, fa, {{"KeyK", "k"}, {"KeyI", "i"}});
    b.KeyDown(Key("NonConvert"), fb, {});
    Check(fb.Text() == "gか" && two.session->switches() == 1, "the second process switches");
    Check(one.session->Adopt(two.session->shared()) && one.session->mode() == Mode::kMuhenkan, "the first follows");
    Check(one.session->profile() == "ja", "the profile is kept for later");
    Type(a, fa, {{"KeyA", "a"}});
    CheckEq(fa.Text(), "きa", "its field commits what it had, and types in muhenkan");
    // A profile this process doesn't have is not taken, and nothing else is in its place.
    Check(!one.session->Adopt(Shared{Mode::kHenkan, "elsewhere", ""}) && one.session->mode() == Mode::kMuhenkan,
          "a profile that isn't in the settings is not followed");
    // profile.last goes back to the one before, which travels too.
    a.KeyDown(Key("F13"), fa, {});
    Check(one.session->shared().profile == "work" && one.session->shared().previous == "ja", "the profile before is kept");
    two.session->Adopt(one.session->shared());
    Check(two.session->shared().previous == "ja", "…and followed");
}

// The block of shared memory the state is published in.
void TestStateBlock() {
    std::vector<std::uint64_t> memory(state::kBlockSize / 8, 0);
    void* block = memory.data();
    Check(!state::Read(block, state::kBlockSize).has_value(), "an empty block has no state");
    state::Init(block, state::kBlockSize);
    const auto* header = static_cast<const state::Header*>(block);
    Check(header->magic == state::kMagic && header->version == 2 && header->capacity == state::kBlockSize - 32,
          "the header");
    Check(std::string(reinterpret_cast<const char*>(block), 4) == "BDS2", "the magic reads BDS2");
    Check(!state::Read(block, state::kBlockSize).has_value(), "nothing is published yet");

    StateSummary summary;
    summary.mode = Mode::kHenkan;
    summary.active = true;
    summary.composing = true;
    const std::uint32_t flags = state::Flags(summary);
    Check(flags == (state::kFlagHenkan | state::kFlagActive | state::kFlagComposing), "the flags");
    const std::string json = R"({"format":"bada-compatible-state/2","staged":"한"})";
    Check(state::Write(block, json, flags, 11, 22) == state::Wrote::kWritten, "a state is written");
    const auto read = state::Read(block, state::kBlockSize);
    Check(read && read->json == json && read->flags == flags && read->process_id == 11 && read->thread_id == 22,
          "…and read back");
    Check(std::string(static_cast<const char*>(block) + 32) == json, "the text ends with a NUL");
    Check(header->sequence % 2 == 0, "the sequence is even at rest");
    Check(state::Write(block, json, flags, 11, 22) == state::Wrote::kSame, "the same state again writes nothing");
    Check(state::Write(block, json, flags, 11, 23) == state::Wrote::kWritten, "the same text from another thread is news");
    Check(state::Write(block, json, flags | state::kFlagCapsLock, 11, 23) == state::Wrote::kWritten, "other flags are news");
    Check(state::Write(block, std::string(state::kBlockSize, 'x'), 0, 1, 1) == state::Wrote::kTooLong &&
              state::Read(block, state::kBlockSize)->json == json,
          "a state too long for the block isn't written");
    const std::string big = R"({"menu":{"rows":[{"text":"a"},{"text":"b"}],"selected":1,"strip":false},"staged":"x"})";
    CheckEq(state::WithoutRows(big), R"({"menu":{"rows":[],"selected":null,"strip":false},"staged":"x"})", "a long state loses its rows");
    // A writer that died in the middle left the sequence odd: the next write starts clean.
    const_cast<state::Header*>(header)->sequence |= 1;
    Check(!state::Read(block, state::kBlockSize).has_value(), "a block being written is not read");
    Check(state::Write(block, "{}", 0, 1, 1) == state::Wrote::kWritten && state::Read(block, state::kBlockSize)->json == "{}",
          "the next writer starts clean");
    Check(state::Write(block, "", 0, 1, 1) == state::Wrote::kWritten && !state::Read(block, state::kBlockSize).has_value(),
          "an empty text publishes nothing");
}

}  // namespace

int main() {
    TestJsonAndKeys();
    TestDescribe();
    TestKeyTableProblems();
    TestSettingsFolder();
    TestApply();
    TestNewInstall();
    TestTyping();
    TestData();
    TestToggleKey();
    TestTwoFieldsAndSecure();
    TestPairs();
    TestCapsLock();
    TestShared();
    TestStateBlock();
    return test::Finish("core");
}
