// The core, with no framework: the settings folder, the key flow, `types`, replies applied to a
// plain text field, and the published state. Everything here types into the linked engine.
//
//   bada_core_tests
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <dbus/dbus.h>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "harness.h"
#include "json.h"
#include "keys.h"
#include "keytable.h"
#include "publish.h"
#include "reply.h"
#include "settings.h"
#include "textmodel.h"

using namespace bada;
using test::Check;
using test::CheckEq;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
    std::string pattern = (fs::temp_directory_path() / (std::string("bada-test-") + name + "-XXXXXX")).string();
    const char* made = ::mkdtemp(pattern.data());
    return made ? fs::path(made) : fs::path();
}

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    // A temporary file, then a rename: as the settings page and the command line write (§10.1).
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
    fs::rename(tmp, path);
}

// The evdev code of a key, from its W3C code.
unsigned Evdev(const std::string& code) {
    for (unsigned e = 1; e < 256; ++e) {
        if (CodeOf(e, 0) == code) return e;
    }
    return 0;
}

// A press as a frontend would report it. `os`: what the system's layout types for it.
RawKey Key(const std::string& code, const std::string& os = "", Mods mods = {}, bool caps = false) {
    RawKey k;
    k.code = Evdev(code);
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
      {"on": "AltRight", "do": ["mode.toggle"]}
    ],
    "cycle": ["ko", "ja"], "caps_rule": "upper_snake"
  },
  "harness": {"com.teammirae.bada.linux": {"keyboard": "jis-pc"}}
})";

struct Sink final : StateSink {
    std::vector<std::string> published;
    int cleared = 0;
    void Publish(const std::string& json) override { published.push_back(json); }
    void Clear() override { ++cleared; }
};

// A session on its own settings folder.
struct Rig {
    fs::path dir;
    Sink sink;
    std::vector<std::string> log;
    std::unique_ptr<Session> session;

    explicit Rig(const char* name, const char* settings = kSettings, const fs::path& data = {}) : dir(TempDir(name)) {
        if (settings) WriteFile(dir / "settings.json", settings);
        Session::Options options;
        options.config_dir = dir;
        options.data_dir = data;
        options.frontend = "test";
        options.version = "0.1.0";
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
        if (!ctx.KeyDown(key, field, info)) field.Insert(os);  // the app types what the OS has
        ctx.KeyUp(key, field, info);
    }
}

void TestJsonAndKeys() {
    const auto v = json::Parse(R"({"a": [1, 2.5, "xé😀"], "b": {"c": null, "d": true}})");
    Check(v.has_value(), "json parses");
    CheckEq(json::Write(*v), "{\"a\":[1,2.5,\"x\xC3\xA9\xF0\x9F\x98\x80\"],\"b\":{\"c\":null,\"d\":true}}", "json writes");
    Check(!json::Parse("{").has_value() && !json::Parse("[1,]").has_value(), "bad json is refused");

    CheckEq(CodeOf(30, 0), "KeyA", "evdev 30");
    CheckEq(CodeOf(92, 0), "Convert", "変換");
    CheckEq(CodeOf(94, 0), "NonConvert", "無変換");
    CheckEq(CodeOf(93, 0), "KanaMode", "カタカナ/ひらがな");
    CheckEq(CodeOf(41, 0), "Backquote", "半角/全角");
    CheckEq(CodeOf(122, 0), "Lang1", "かな");
    CheckEq(CodeOf(123, 0), "Lang2", "英数");
    CheckEq(CodeOf(89, 0), "IntlRo", "ろ");
    CheckEq(CodeOf(124, 0), "IntlYen", "¥");
    CheckEq(CodeOf(58, 0), "CapsLock", "Caps Lock");
    CheckEq(CodeOf(100, 0), "AltRight", "right Alt");
    CheckEq(CodeOf(0, 0xff23), "Convert", "a key with no code goes by its keysym");
    CheckEq(CodeOf(240, 0), "Unidentified", "a key the table doesn't know");
    Check(IsLanguageKey("Convert") && !IsLanguageKey("CapsLock") && IsHarnessKey("CapsLock"), "language keys and the lock");
    CheckEq(Capitals("a\xC3\xA9z1"), "A\xC3\x89Z1", "capitals");
    Check(Scalars("한a\xF0\x9F\x98\x80") == 3 && ByteOffset("한a", 1) == 3, "positions are scalar values");
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
    Check(!parse(R"({"bindings": [{"on": "Lang1+Lang1", "do": ["fix"]}]})"), "a pair is two different keys");
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
    // Caps Lock bound to `pass` is given back: it is the lock again (§9.4).
    const auto passes = parse(R"({"bindings": [{"on": "CapsLock", "do": ["pass"]}]})");
    if (passes) {
        const Decision d = Decide(*passes, {}, TableState{}, *ParsePress("CapsLock"));
        Check(d.then == Decision::Then::kPass && d.binding == 0 && d.caps_lock == true, "Caps Lock passed on is the lock");
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
    SettingsFolder folder(dir);

    // A new install: no file. No muhenkan layout, no profile, the keyboard's preset (§8.3).
    folder.Reload(*session);
    Check(folder.loaded().profiles.empty() && folder.loaded().muhenkan_layout.empty(), "a new install has no profile");
    Check(folder.loaded().table.bindings.size() == 7, "a new install has the JIS PC preset");
    Check(folder.problems().empty(), "a new install has no problems");
    Check(!folder.Reload(*session), "nothing changed: nothing is read");

    // A file appears: the watch fires, and it is read.
    WriteFile(dir / "settings.json", kSettings);
    Check(folder.watch_fd() >= 0 && folder.Changed(), "the watch sees settings.json written");
    Check(folder.Reload(*session), "the new file is read");
    Check(folder.loaded().profiles.size() == 3 && folder.loaded().profiles[0].id == "ko" &&
              folder.loaded().profiles[0].name == "한국어" && folder.loaded().profiles[2].layouts.size() == 2,
          "the profiles are the engine's");
    CheckEq(folder.loaded().muhenkan_layout, "en-qwerty", "the muhenkan layout");
    Check(folder.loaded().table.bindings.size() == 13 && folder.loaded().table.caps_rule == CapsRule::kUpperSnake,
          "the key table is the file's");
    Check(folder.problems().empty(), "a good file has no problems");

    // A file that can't be used changes nothing, and its problems are kept (§10.1).
    WriteFile(dir / "settings.json", "{ not json");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 3, "invalid JSON changes nothing");
    Check(folder.problems().size() == 1 && folder.problems()[0].code == "bad-json" && folder.problems()[0].file == "settings.json",
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
    Check(!folder.problems().empty() && folder.problems()[0].path == "keys.bindings[0].do[0]", "the key table's problem is kept");
    // A profile the table names that isn't there is taken, and noted.
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"henkan": {"profiles": [{"id": "ko", "name": "ko", "layouts": ["ko-2-set"]}]}},
        "keys": {"bindings": [{"on": "Lang1", "do": ["profile:gone"]}], "cycle": ["ko", "gone"]},
        "harness": {"com.teammirae.bada.linux": {"keyboard": "none", "publish_state": false}, "com.teammirae.bada.mac": {"x": 1}},
        "later": 1})");
    folder.Reload(*session);
    Check(folder.loaded().profiles.size() == 1 && folder.loaded().table.bindings.size() == 1, "a file with warnings is taken");
    Check(folder.loaded().harness.keyboard == Preset::kNoLanguageKeys && !folder.loaded().harness.publish_state,
          "the harness's own part is read, and only its own");
    int warnings = 0;
    for (const Problem& p : folder.problems()) warnings += p.level == "warning";
    Check(warnings == 3 && folder.problems().size() == 3, "an unknown profile (twice) and an unknown part are warnings");

    // A custom layout is fed before the settings, so the settings may name it (§7.6).
    std::string stock;
    ReadFile(fs::path(BADA_ROOT) / "engine/layouts/en-qwerty.json", &stock);
    auto layout = json::Parse(stock);
    Check(layout.has_value(), "a stock layout file is read");
    if (layout) {
        layout->Set("id", json::Str("en-mine"));
        WriteFile(dir / "layouts" / "mine.json", json::Write(*layout));
        WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-mine"}}})");
        Check(folder.Changed(), "the watch sees a layout written");
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
        Check(noted, "the engine's warning about a custom layout is kept");
        // A layout that can't be read: the one loaded stays.
        WriteFile(dir / "layouts" / "mine.json", "[1, 2");
        folder.Reload(*session);
        Check(listed("en-mine"), "a broken layout file changes nothing");
        // Removed: the settings that still name it are sent again, refused, and say so.
        fs::remove(dir / "layouts" / "mine.json");
        Check(folder.Changed(), "the watch sees a layout removed");
        folder.Reload(*session);
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
    folder.Reload(*session);
    const auto dictionary = json::Parse(session->Send(R"({"type": "dictionary"})"));
    Check(dictionary && dictionary->GetBool("ok"), "the dictionary event answers");
    WriteFile(dir / "dictionary.json", "nope");
    folder.Reload(*session);
    bool bad = false;
    for (const Problem& p : folder.problems()) bad |= p.file == "dictionary.json" && p.code == "bad-json";
    Check(bad, "a dictionary that isn't JSON is a problem");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestApply() {
    // A reply applied to the model, as §6.3 has it.
    test::TextModel field;
    field.Insert("ab");
    ApplyOnce(ParseReply(R"({"ok": true, "preedit": {"text": "한국", "caret": 1}, "staged": "한국"})"), field);
    Check(field.Text() == "ab한국" && field.Marked() == "한국" && field.from == 3, "a preedit is marked, with its caret");
    ApplyOnce(ParseReply(R"({"ok": false, "error": {"code": "bad-row"}})"), field);
    CheckEq(field.Marked(), "한국", "a refused event changes nothing");
    ApplyOnce(ParseReply(R"({"ok": true, "commit": "韓國", "preedit": {"text": "ㅇ"}})"), field);
    Check(field.Text() == "ab韓國ㅇ" && field.Marked() == "ㅇ", "a commit replaces the marked text, then the new preedit");
    ApplyOnce(ParseReply(R"({"ok": true, "menu": {"rows": [{"text": "a", "entry": 0, "note": "n"}], "selected": 0, "strip": true}, "preedit": {"text": "ㅇ"}})"),
              field);
    Check(field.menu.rows.size() == 1 && field.menu.strip && field.menu.selected == 0u && field.menu.rows[0].note == "n",
          "a menu with a note and a strip");
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
    Rig rig("new", nullptr);
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    // No muhenkan layout: the engine has no say, and the OS types the key.
    Check(!ctx.KeyDown(Key("KeyA", "a"), field, {}), "with no muhenkan layout a key goes to the app");
    // 変換 would enter henkan, and there is no profile: the onboarding, and the engine isn't asked.
    Check(ctx.KeyDown(Key("Convert"), field, {}), "変換 is taken");
    Check(rig.session->mode() == Mode::kMuhenkan, "with no profile, 変換 doesn't enter henkan");
    Check(!field.notice.empty() && field.notice.find("settings.json") != std::string::npos, "the frontend is given a notice");
    Check(!rig.log.empty() && rig.log.back().find("no henkan profile") != std::string::npos, "the onboarding is logged");
    CheckEq(rig.State().Get("harness")->GetString("notice"), field.notice, "the notice is in the state");
    // The next key takes the notice away.
    ctx.KeyDown(Key("KeyB", "b"), field, {});
    Check(field.notice.empty(), "the notice goes at the next key");
    // The README's two lines, written by hand: the key works from then on.
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-qwerty"},
        "henkan": {"profiles": [{"id": "main", "name": "日本語", "layouts": ["ja-romaji"]}]}}})");
    Check(rig.session->WatchFired(), "the folder's watch fires, and the file is read");
    Check(ctx.KeyDown(Key("Convert"), field, {}) && rig.session->mode() == Mode::kHenkan && rig.session->profile() == "main",
          "変換 enters the first profile");
    CheckEq(rig.session->Label(), "日", "the indicator is the profile's first character");
    Type(ctx, field, {{"KeyK", "k"}, {"KeyA", "a"}});
    CheckEq(field.Marked(), "か", "romaji: ka is か");
    Check(rig.State().Get("harness")->GetString("notice").empty(), "the notice is gone from the state");
}

void TestTyping() {
    Rig rig("typing");
    if (!rig.session) return;
    Context ctx(*rig.session);
    test::TextModel field;
    const ContextInfo info{"app", false};

    // Muhenkan (§6.4): the OS types the key when its layout gives the same text.
    Check(!ctx.KeyDown(Key("KeyH", "h"), field, info), "the OS types h itself");
    Check(field.commits.empty(), "…and the harness commits nothing");
    // Another layout in the OS (Dvorak has d on the H key): the harness types what the layout says.
    Check(ctx.KeyDown(Key("KeyH", "d"), field, info) && field.Text() == "h", "the harness types h under another OS layout");
    // The JIS table, whatever the OS has: ^ on the Equal key, " on ⇧2, \ on ろ, ¥.
    Check(ctx.KeyDown(Key("Equal", "="), field, info) && ctx.KeyDown(Key("Digit2", "@", kShift), field, info) &&
              ctx.KeyDown(Key("IntlRo", ""), field, info) && ctx.KeyDown(Key("IntlYen", "\\"), field, info),
          "JIS symbols are typed by the harness under a US layout");
    CheckEq(field.Text(), "h^\"\\\xC2\xA5", "the JIS characters");
    // ⇧0 is the one empty slot: it types nothing, and the app doesn't get it.
    Check(ctx.KeyDown(Key("Digit0", ")", kShift), field, info) && field.Text() == "h^\"\\\xC2\xA5", "⇧0 types nothing");
    // A shortcut is passed on as the physical key.
    Check(!ctx.KeyDown(Key("KeyC", "c", kCtrl), field, info), "⌃C goes to the app");
    // Space, Enter and the like: the engine has no say.
    Check(!ctx.KeyDown(Key("Space", " "), field, info) && !ctx.KeyDown(Key("Enter", "\r"), field, info), "Space and Enter go to the app");
    // A modifier on its own is never the engine's, and never eaten.
    Check(!ctx.KeyDown(Key("ShiftLeft"), field, info), "a modifier's press reaches the app");

    // Caps Lock: the letters of `types` in capitals.
    field = test::TextModel();
    Check(!ctx.KeyDown(Key("KeyA", "A", {}, true), field, info), "with the lock on the OS types A itself");
    Check(ctx.KeyDown(Key("KeyA", "a", kShift, true), field, info) && field.Text() == "A", "⇧ with the lock on is still a capital");
    Check(ctx.KeyDown(Key("Digit1", "!", {}, true), field, info) && field.Text() == "A1", "the lock leaves digits alone");
    // UPPER_SNAKE (the table's caps_rule): Space types _, ⌥Space a space, without the engine.
    Check(ctx.KeyDown(Key("Space", " ", {}, true), field, info) && ctx.KeyDown(Key("Space", " ", kAlt, true), field, info) &&
              field.Text() == "A1_ ",
          "UPPER_SNAKE: Space is _, and ⌥Space a space");
    Check(!ctx.KeyDown(Key("Space", " "), field, info), "with the lock off Space is the app's");

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
    Check(h && h->GetString("id") == "com.teammirae.bada.linux" && h->GetString("version") == "0.1.0" &&
              h->GetString("app") == "app" && h->GetBool("active") && !h->GetBool("secure"),
          "the harness's part of the state");
    Check(h && h->Get("keys")->Get("available")->array.size() == 7 && h->Get("keys")->GetBool("chords") &&
              h->Get("keys")->Get("missing")->array.empty() && h->Get("problems")->array.empty(),
          "harness.keys and harness.problems");
    Check(h && h->Get("caps_lock")->GetString("setting") == "respect" && h->Get("caps_lock")->GetString("rule") == "upper_snake",
          "harness.caps_lock");
    Type(ctx, field, {{"Space", " "}}, info);
    Check(field.Text() == "한 " && !field.marked, "Space commits 한 and then types a space");
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

    // Focus leaves: what is staged is committed.
    Type(ctx, field, {{"KeyG", "g"}}, info);
    ctx.FocusOut(field);
    Check(field.Text() == "한 하かgkａｂｃㅎ" && !field.marked, "focus out commits what is staged");
    Check(!rig.State().Get("harness")->GetBool("active") && rig.State().GetString("staged").empty(),
          "with no field in focus the state shows nothing");
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
        Rig rig(with ? "with-data" : "without-data", kSettings,  with ? data : fs::path());
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
    const ContextInfo secure{"pw", true};
    Type(password, fp, {{"KeyG", "g"}, {"KeyK", "k"}, {"KeyS", "s"}}, secure);
    Check(fp.Text() == "gks" && !fp.marked && fp.commits.empty(), "a password field gets plain keys");
    const json::Value state = rig.State();
    Check(state.Get("harness")->GetBool("secure") && state.GetString("staged").empty() &&
              state.Get("preedit")->GetString("text").empty() && state.Get("menu")->Get("rows")->array.empty(),
          "the state hides a password field");
    for (const std::string& s : rig.sink.published) {
        if (s.find("\"secure\":true") != std::string::npos) Check(s.find("gks") == std::string::npos, "no state carried the password");
    }
    // Language keys still switch mode there.
    Check(password.KeyDown(Key("NonConvert"), fp, secure) && rig.session->mode() == Mode::kMuhenkan,
          "a password field still switches mode");
}

void TestPairsAndLock() {
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
    Check(field.forwarded.size() == 1 && field.forwarded[0].code == Evdev("Lang2"), "released alone: passed on, late");
    // A key that waited and has no binding of its own goes to the OS then (かな, here).
    Check(ctx.KeyDown(Key("Lang1"), field, {}), "かな waits for its pair");
    ctx.KeyUp(Key("Lang1"), field, {});
    Check(field.forwarded.size() == 2 && field.forwarded[1].code == Evdev("Lang1"), "released alone and unbound: it goes to the OS");
    field.forwarded.pop_back();
    // Another key while one waits: the waiting key's binding first, then the key.
    ctx.KeyDown(Key("Lang2"), field, {});
    Check(ctx.KeyDown(Key("KeyG", "g"), field, {}) && field.forwarded.size() == 2 && field.Marked() == "ㅎ",
          "another key runs the waiting one first");
    ctx.KeyUp(Key("Lang2"), field, {});
    ctx.KeyUp(Key("KeyG"), field, {});
    // A modifier pressed and released alone is a key where a binding says so (AltRight: the toggle).
    Check(!ctx.KeyDown(Key("AltRight"), field, {}), "a modifier's press is never eaten");
    ctx.KeyUp(Key("AltRight"), field, {});
    Check(rig.session->mode() == Mode::kMuhenkan && field.Text() == "ㅎ", "right Alt tapped: the toggle, which commits");
    // …but not when another key came between.
    ctx.KeyDown(Key("AltRight"), field, {});
    ctx.KeyDown(Key("KeyA", "a", kAlt), field, {});
    ctx.KeyUp(Key("AltRight"), field, {});
    Check(rig.session->mode() == Mode::kMuhenkan, "right Alt held for a chord is no tap");
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
    Check(field.Marked() == "ㅎㅎ" || field.Marked() == "ㄱㄱ" || bada::Scalars(field.Marked()) == 2,
          "a held typing key repeats: \"" + field.Marked() + "\"");
    ctx.KeyUp(repeat, field, {});
    ctx.KeyDown(Key("NonConvert"), field, {});
    ctx.KeyDown(Key("Convert"), field, {});

    // Caps Lock bound to the toggle is no longer the lock (§9.4): the harness keeps its own
    // account, since Linux still flips the lock under it.
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "ko", "name": "ko", "layouts": ["ko-2-set"]}]}},
        "keys": {"caps_lock": "transient"}, "harness": {"com.teammirae.bada.linux": {"keyboard": "none"}}})");
    rig.session->Reload();
    field = test::TextModel();
    Check(rig.session->table().bindings.size() == 1, "keyboard none: the preset binds Caps Lock to the toggle");
    Check(rig.session->mode() == Mode::kHenkan && rig.session->profile() == "ko", "a profile that is still there stays selected");
    Check(ctx.KeyDown(Key("CapsLock"), field, {}) && rig.session->mode() == Mode::kMuhenkan, "Caps Lock toggles out");
    // Linux flipped the lock, so every key now says it is on. The harness knows better.
    Check(ctx.KeyDown(Key("KeyA", "A", {}, true), field, {}) && field.Text() == "a", "a bound Caps Lock doesn't type capitals");
    // ⇧Caps Lock is another chord: unbound, it is the lock.
    Check(!ctx.KeyDown(Key("CapsLock", "", kShift, true), field, {}), "⇧Caps Lock goes to the OS");
    Check(ctx.KeyDown(Key("KeyA", "a"), field, {}) && field.Text() == "aA", "…and is the lock");
    Check(rig.State().Get("harness")->Get("caps_lock")->GetBool("on"), "the lock is in the state");

    // A profile removed while selected: no fallback to another, so muhenkan (§8.1).
    ctx.KeyDown(Key("CapsLock"), field, {});
    Check(rig.session->mode() == Mode::kHenkan, "back in henkan");
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2",
        "engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "other", "name": "o", "layouts": ["ja-kana"]}]}}})");
    rig.session->Reload();
    Check(rig.session->mode() == Mode::kMuhenkan && rig.session->profile().empty(), "the selected profile was removed: muhenkan");
    field = test::TextModel();
    Check(!ctx.KeyDown(Key("KeyB", "b"), field, {}), "the field follows with a select, and types in muhenkan");
    // publish_state false: nothing is published.
    WriteFile(rig.dir / "settings.json", R"({"format": "bada-settings/2", "harness": {"com.teammirae.bada.linux": {"publish_state": false}}})");
    rig.session->Reload();
    const size_t published = rig.sink.published.size();
    ctx.KeyDown(Key("KeyB", "b"), field, {});
    Check(rig.sink.cleared == 1 && rig.sink.published.size() == published, "publish_state false: the state is cleared, and stays so");
}

// The publisher: a file, and a D-Bus signal when there is a session bus.
void TestPublisher() {
    const fs::path run = TempDir("run");
    ::setenv("XDG_RUNTIME_DIR", run.c_str(), 1);
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* listener = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    dbus_error_free(&error);
    if (listener) {
        dbus_connection_set_exit_on_disconnect(listener, FALSE);
        dbus_bus_add_match(listener, "type='signal',interface='com.teammirae.Bada.State',path='/com/teammirae/Bada'", nullptr);
        dbus_connection_flush(listener);
    }
    {
        Publisher publisher;
        CheckEq(Publisher::StatePath().string(), (run / "bada" / "state.json").string(), "the state file's place");
        publisher.Publish(R"({"format":"bada-compatible-state/2","mode":"henkan"})");
        std::string text;
        Check(ReadFile(Publisher::StatePath(), &text) && text == "{\"format\":\"bada-compatible-state/2\",\"mode\":\"henkan\"}\n",
              "the state is written to its file");
        Check((fs::status(Publisher::StatePath()).permissions() & fs::perms::others_read) == fs::perms::none,
              "only the user reads the state file");
        if (listener) {
            std::string got;
            for (int i = 0; i < 50 && got.empty(); ++i) {
                dbus_connection_read_write(listener, 20);
                while (DBusMessage* m = dbus_connection_pop_message(listener)) {
                    const char* json = nullptr;
                    if (dbus_message_is_signal(m, "com.teammirae.Bada.State", "Changed") &&
                        dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID)) {
                        got = json;
                    }
                    dbus_message_unref(m);
                }
            }
            CheckEq(got, R"({"format":"bada-compatible-state/2","mode":"henkan"})",
                    "com.teammirae.Bada.State.Changed carries the state");
        } else {
            std::printf("(no session bus: the D-Bus signal isn't checked. Run under dbus-run-session.)\n");
        }
        publisher.Clear();
        Check(!fs::exists(Publisher::StatePath()), "cleared: the file is gone");
    }
    if (listener) {
        dbus_connection_close(listener);
        dbus_connection_unref(listener);
    }
    std::error_code ec;
    fs::remove_all(run, ec);
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
    TestPairsAndLock();
    TestPublisher();
    return test::Finish("core");
}
