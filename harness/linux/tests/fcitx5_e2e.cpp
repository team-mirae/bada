// End to end through Fcitx5, with no desktop: a real fcitx::Instance with the bada addon, Fcitx5's
// own test frontend (an input context in this process) and its test UI. Keys go in as an app's
// would, with a keysym and an XKB keycode; commits, the preedit, the menu and the published state
// come out.
//
// The settings folder, the state file and the session bus are the test's own: nothing of the
// user's is read or written.
//
//   bada_fcitx5_e2e
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/testing.h>
#include <fcitx/action.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/surroundingtext.h>
#include <fcitx/userinterfacemanager.h>

#include "check.h"
#include "json.h"
#include "settings.h"
#include "testfrontend_public.h"

using test::Check;
using test::CheckEq;
namespace fs = std::filesystem;

namespace {

fs::path g_config, g_runtime;
fcitx::Instance* g_instance = nullptr;
fcitx::AddonInstance* g_frontend = nullptr;
// Every commit and every key handed back to the app, by input context.
std::map<fcitx::InputContext*, std::vector<std::string>> g_commits;
std::map<fcitx::InputContext*, int> g_forwarded;

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
    fs::rename(tmp, path);
}

bada::json::Value StateFile() {
    std::string text;
    if (!bada::ReadFile(g_runtime / "bada" / "state.json", &text)) return {};
    auto v = bada::json::Parse(text);
    return v ? *v : bada::json::Value{};
}

// token → keysym under a US layout, shifted keysym, evdev code.
struct KeyDef {
    unsigned sym, shifted, code;
};
const std::map<std::string, KeyDef>& Keys() {
    static const std::map<std::string, KeyDef> keys = [] {
        std::map<std::string, KeyDef> k;
        const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        const unsigned first[] = {16, 30, 44};
        for (int r = 0; r < 3; ++r) {
            for (unsigned i = 0; rows[r][i]; ++i) {
                const char c = rows[r][i];
                k[std::string(1, c)] = {static_cast<unsigned>(c), static_cast<unsigned>(c - 'a' + 'A'), first[r] + i};
            }
        }
        k["2"] = {'2', '@', 3};
        k["0"] = {'0', ')', 11};
        k["Equal"] = {'=', '+', 13};  // JIS ^ ~
        k["Space"] = {0x20, 0x20, 57};
        k["Enter"] = {0xff0d, 0xff0d, 28};
        k["Backspace"] = {0xff08, 0xff08, 14};
        k["Period"] = {'.', '>', 52};
        k["Muhenkan"] = {0xff22, 0xff22, 94};
        k["Henkan"] = {0xff23, 0xff23, 92};
        k["Katakana"] = {0xff27, 0xff27, 93};
        k["Grave"] = {'`', '~', 41};  // 半角/全角 by position; ` under a US layout
        k["F13"] = {0xffca, 0xffca, 183};
        return k;
    }();
    return keys;
}

// One app's text field: an input context, and the text the app would hold.
struct Client {
    fcitx::ICUUID uuid;
    fcitx::InputContext* ic;
    std::string text;
    size_t seen = 0;  // commits already put in `text`

    explicit Client(const std::string& program, fcitx::CapabilityFlags caps = fcitx::CapabilityFlag::NoFlag) {
        uuid = g_frontend->call<fcitx::ITestFrontend::createInputContext>(program);
        ic = g_instance->inputContextManager().findByUUID(uuid);
        ic->setCapabilityFlags(caps);
    }

    void Focus() {
        ic->focusIn();
        if (g_instance->inputMethod(ic) != "bada") g_instance->setCurrentInputMethod(ic, "bada", false);
    }

    void TakeCommits() {
        const auto& commits = g_commits[ic];
        for (; seen < commits.size(); ++seen) text += commits[seen];
    }

    // A press or a release. True: bada ate it. A press it didn't eat is the app's: it types what
    // its US layout has for the key.
    bool Key(const std::string& token, bool release = false, fcitx::KeyStates states = fcitx::KeyStates(), bool app = true) {
        const KeyDef& def = Keys().at(token);
        const bool shift = states.test(fcitx::KeyState::Shift);
        const fcitx::Key key(static_cast<fcitx::KeySym>(shift ? def.shifted : def.sym), states, static_cast<int>(def.code + 8));
        const bool eaten = g_frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid, key, release);
        TakeCommits();
        if (!eaten && !release && app && !states.test(fcitx::KeyState::Ctrl) && !states.test(fcitx::KeyState::Alt)) {
            const unsigned sym = shift ? def.shifted : def.sym;
            if (token == "Enter") text += "\n";
            else if (token == "Backspace") text = text.substr(0, text.empty() ? 0 : text.size() - 1);
            else if (sym >= 0x20 && sym < 0x7f) text += static_cast<char>(sym);
        }
        return eaten;
    }

    // A press and its release, with the very keysym: what the app's layout made of the key.
    bool Raw(unsigned sym, fcitx::KeyStates states, unsigned code) {
        const fcitx::Key key(static_cast<fcitx::KeySym>(sym), states, static_cast<int>(code + 8));
        const bool eaten = g_frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid, key, false);
        g_frontend->call<fcitx::ITestFrontend::sendKeyEvent>(uuid, key, true);
        TakeCommits();
        return eaten;
    }

    // Presses and releases each key of a run: "g k s Space".
    void Type(const std::string& tokens) {
        size_t from = 0;
        while (from < tokens.size()) {
            size_t to = tokens.find(' ', from);
            if (to == std::string::npos) to = tokens.size();
            if (to > from) {
                const std::string token = tokens.substr(from, to - from);
                Key(token);
                Key(token, true);
            }
            from = to + 1;
        }
    }

    // The marked text, where it is drawn: by the app (client preedit) or in the panel.
    std::string Preedit() const {
        const auto& panel = ic->inputPanel();
        return panel.clientPreedit().toString() + panel.preedit().toString();
    }
    std::vector<std::string> Menu() const {
        std::vector<std::string> rows;
        const auto list = ic->inputPanel().candidateList();
        const auto* bulk = list ? list->toBulk() : nullptr;
        for (int i = 0; bulk && i < bulk->totalSize(); ++i) rows.push_back(bulk->candidateFromAll(i).text().toString());
        return rows;
    }
    size_t Commits() const { return g_commits[ic].size(); }
};

const char* kFirstSettings = R"({
  "format": "bada-settings/2",
  "engine": {
    "muhenkan": {"layout": "en-qwerty"},
    "henkan": {"profiles": [{"id": "main", "name": "한국어", "layouts": ["ko-2-set"]}]}
  }
})";

const char* kSecondSettings = R"({
  "format": "bada-settings/2",
  "engine": {
    "muhenkan": {"layout": "en-qwerty"},
    "henkan": {"profiles": [
      {"id": "main", "name": "한국어", "layouts": ["ko-2-set"]},
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
      {"on": "Backquote", "do": ["width.full"]},
      {"on": "KanaMode", "do": ["profile:ja", "menu.next"]},
      {"on": "F13", "do": ["profile.next"]},
      {"on": "Convert+KanaMode", "do": ["profile:work"]}
    ],
    "cycle": ["main", "ja", "work"]
  }
})";

// Phase 1: a new install. No settings file: no muhenkan layout, no profile, the JIS PC preset.
void NewInstall(Client& a) {
    a.Focus();
    CheckEq(g_instance->inputMethod(a.ic), "bada", "bada is the input method in use");
    bada::json::Value state = StateFile();
    Check(state.GetString("format") == "bada-compatible-state/2" && state.GetString("mode") == "muhenkan" &&
              state.Get("harness") && state.Get("harness")->GetString("id") == "com.teammirae.bada.linux" &&
              state.Get("harness")->GetString("frontend") == "fcitx5" && state.Get("harness")->GetString("app") == "e2e",
          "the state is published at focus in: " + bada::json::Write(state));
    Check(state.Get("harness") && state.Get("harness")->Get("keys")->Get("available")->array.size() == 7 &&
              state.Get("harness")->Get("keys")->GetBool("chords"),
          "harness.keys says what is detected");
    // With no muhenkan layout the engine has no say: the app types its own layout's characters.
    a.Type("h i");
    CheckEq(a.text, "hi", "a new install: the OS types");
    Check(a.Commits() == 0, "…and bada commits nothing");
    // 変換 would enter henkan, but there is no profile: the onboarding, and no henkan.
    Check(a.Key("Henkan"), "変換 is eaten");
    a.Key("Henkan", true);
    CheckEq(g_instance->inputMethodEngine("bada")->subMode(*g_instance->inputMethodManager().entry("bada"), *a.ic), "muhenkan",
            "with no profile, 変換 doesn't enter henkan");
    const std::string notice = a.ic->inputPanel().auxUp().toString();
    Check(notice.find("no henkan profile") != std::string::npos, "the notice is shown: \"" + notice + "\"");
    state = StateFile();
    Check(state.Get("harness") && !state.Get("harness")->GetString("notice").empty(), "the notice is in the state");
    a.Type("a");
    Check(a.ic->inputPanel().auxUp().toString().empty(), "the notice goes at the next key");
    a.text.clear();
    // The first settings file is written, as the README says. The watch picks it up.
    WriteFile(g_config / "settings.json", kFirstSettings);
}

// Phase 2: the settings file is there. QWERTY, Korean 2-set, a mode switch.
void FirstSettings(Client& a) {
    bada::json::Value state = StateFile();
    Check(state.Get("harness") && state.Get("harness")->GetString("notice").empty() &&
              state.Get("harness")->Get("problems")->array.empty(),
          "the settings were read when the file was written: " + bada::json::Write(state));

    // QWERTY in muhenkan. The OS types what its layout has the same; bada types the rest.
    a.Type("h e l l o");
    CheckEq(a.text, "hello", "QWERTY in muhenkan");
    Check(a.Commits() == 0, "the US layout types the same letters, so the keys are let through");
    Check(a.Key("Equal"), "the JIS ^ key is typed by bada under a US layout");
    a.Key("Equal", true);
    Check(a.Key("2", false, fcitx::KeyState::Shift), "⇧2 is typed by bada: \" on JIS, @ on US");
    a.Key("2", true, fcitx::KeyState::Shift);
    CheckEq(a.text, "hello^\"", "JIS characters under a US layout");
    Check(a.Key("0", false, fcitx::KeyState::Shift) && a.text == "hello^\"", "⇧0 types nothing");
    a.Key("0", true, fcitx::KeyState::Shift);
    Check(!a.Key("c", false, fcitx::KeyState::Ctrl) && a.Commits() == 2, "⌃C goes to the app, and nothing is committed");
    a.Key("c", true, fcitx::KeyState::Ctrl);
    // Caps Lock: the letters in capitals. An app whose layout gives A is left to type it; where
    // the layout gives a (⇧ undoes the lock in XKB), bada types the capital.
    Check(!a.Raw('A', fcitx::KeyState::CapsLock, 30), "the OS types A with the lock on, and bada lets it");
    Check(a.Raw('a', fcitx::KeyStates{fcitx::KeyState::CapsLock, fcitx::KeyState::Shift}, 30) && a.text == "hello^\"A",
          "⇧A with the lock on is still a capital: bada types it");
    a.text.clear();

    // 変換: a mode switch through the key table (the JIS PC preset: profile.first).
    Check(a.Key("Henkan"), "変換 is eaten");
    a.Key("Henkan", true);
    state = StateFile();
    Check(state.GetString("mode") == "henkan" && state.GetString("profile") == "main", "変換 enters henkan: " + bada::json::Write(state));
    auto* engine = g_instance->inputMethodEngine("bada");
    const auto* entry = g_instance->inputMethodManager().entry("bada");
    CheckEq(engine->subMode(*entry, *a.ic), "한국어", "the sub mode is the profile's name");
    CheckEq(engine->subModeLabel(*entry, *a.ic), "한", "the indicator is its first character");

    // Korean 2-set: gks is 한; Space commits it and goes to the app.
    const size_t committed = a.Commits();
    a.Type("g k s");
    CheckEq(a.Preedit(), "한", "gks is 한");
    Check(a.Commits() == committed, "nothing is committed while composing");
    state = StateFile();
    Check(state.GetString("staged") == "한" && state.Get("preedit")->GetString("text") == "한", "the state has what is staged");
    a.Type("Space");
    Check(a.text == "한 " && a.Preedit().empty(), "Space commits 한, and the app types the space: \"" + a.text + "\"");
    a.Type("d k s s u d Enter");
    CheckEq(a.text, "한 안녕\n", "안녕, then Enter commits it and goes to the app");

    // Backspace takes back the last key.
    a.Type("g k s Backspace");
    CheckEq(a.Preedit(), "하", "Backspace takes back the last staged key");
    // 無変換: back to muhenkan. The select commits what is staged.
    Check(a.Key("Muhenkan"), "無変換 is eaten");
    a.Key("Muhenkan", true);
    Check(a.text == "한 안녕\n하" && a.Preedit().empty(), "無変換 commits what is staged");
    CheckEq(StateFile().GetString("mode"), "muhenkan", "無変換 leaves henkan");
    a.Type("o k");
    CheckEq(a.text, "한 안녕\n하ok", "muhenkan again");
    a.text.clear();

    // More profiles, and a key table of the user's own.
    WriteFile(g_config / "settings.json", kSecondSettings);
}

// Phase 3: three profiles and the user's own table.
void SecondSettings(Client& a) {
    auto* engine = g_instance->inputMethodEngine("bada");
    const auto* entry = g_instance->inputMethodManager().entry("bada");

    // カタカナ: a profile switch through the key table (profile:ja). Romaji: ka is か.
    Check(a.Key("Katakana", false, fcitx::KeyStates(), false), "カタカナ waits: it is half of a bound pair");
    a.Key("Katakana", true);
    bada::json::Value state = StateFile();
    Check(state.GetString("mode") == "henkan" && state.GetString("profile") == "ja", "カタカナ, released alone, enters its profile");
    CheckEq(engine->subMode(*entry, *a.ic), "日本語", "the sub mode follows the profile");
    a.Type("k a");
    CheckEq(a.Preedit(), "か", "romaji: ka is か");
    a.Type("n j i");
    CheckEq(a.Preedit(), "かんじ", "kanji is かんじ (no kanji data yet: it stays kana)");
    a.Type("Enter");
    CheckEq(a.text, "かんじ\n", "Enter commits, then the app has it");

    // F13: profile.next, a chord. ja → work.
    a.Type("F13");
    CheckEq(StateFile().GetString("profile"), "work", "F13 goes to the next profile of the cycle");
    // Two layouts: a menu of both readings. 変換 is the next row, ⇧変換 the previous.
    a.Type("g k");
    Check(a.Menu() == std::vector<std::string>({"하", "gk"}), "a menu with a row for each layout");
    CheckEq(a.Preedit(), "하", "the top reading shows");
    a.Type("Henkan");
    CheckEq(a.Preedit(), "gk", "変換 moves to the next row");
    Check(a.ic->inputPanel().candidateList()->cursorIndex() == 1, "the menu's cursor follows");
    a.Key("Henkan", false, fcitx::KeyState::Shift);
    a.Key("Henkan", true, fcitx::KeyState::Shift);
    CheckEq(a.Preedit(), "하", "⇧変換 moves back");
    // A click on a row: bada moves to it and commits.
    a.ic->inputPanel().candidateList()->candidate(1).select(a.ic);
    a.TakeCommits();
    Check(a.text == "かんじ\ngk" && a.Preedit().empty() && a.Menu().empty(), "a click on a row commits it: \"" + a.text + "\"");

    // 変換 and カタカナ held together: the pair's binding, when both are released.
    a.Type("F13");
    CheckEq(StateFile().GetString("profile"), "main", "the cycle wraps");
    a.Key("Henkan");
    a.Key("Katakana");
    a.Key("Henkan", true);
    CheckEq(StateFile().GetString("profile"), "main", "one key of the pair is still down");
    a.Key("Katakana", true);
    CheckEq(StateFile().GetString("profile"), "work", "the pair ran when both keys were up");

    // The input menu: muhenkan, and each profile by name.
    auto* ja = g_instance->userInterfaceManager().lookupAction("bada-select-2");
    Check(ja != nullptr && ja->shortText(a.ic) == "日本語", "the input menu lists the profiles by name");
    if (ja) ja->activate(a.ic);
    CheckEq(StateFile().GetString("profile"), "ja", "the input menu selects a profile");

    // 半角/全角 with nothing staged: the engine asks for the selection, once, and replaces it.
    Client s("selection", fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit, fcitx::CapabilityFlag::SurroundingText});
    s.Focus();
    s.ic->surroundingText().setText("abc def", 3, 0);
    s.ic->updateSurroundingText();
    Check(s.Key("Grave"), "半角/全角 is eaten");
    s.Key("Grave", true);
    Check(s.Commits() == 1 && g_commits[s.ic][0] == "ａｂｃ", "the selection is committed full-width");
    s.ic->focusOut();

    // Focus out commits what is staged, once. With the marked text in the panel, bada commits.
    a.Focus();
    a.text.clear();
    a.Type("k a");
    size_t before = a.Commits();
    a.ic->focusOut();
    a.TakeCommits();
    Check(a.Commits() == before + 1 && a.text == "か", "focus out commits か once (panel preedit)");
    Check(!StateFile().Get("harness")->GetBool("active"), "with no field in focus, the state says so");
    // With the marked text in the app, Fcitx5 commits it as focus leaves, and bada lets go of it.
    Client c("client-preedit", fcitx::CapabilityFlag::Preedit);
    c.Focus();
    c.Type("k a");
    Check(c.ic->inputPanel().clientPreedit().toString() == "か", "an app that draws the preedit is given it");
    c.ic->focusOut();
    c.TakeCommits();
    Check(c.Commits() == 1 && c.text == "か", "focus out commits か once (client preedit): " + std::to_string(c.Commits()));
    c.Focus();
    c.Type("k a");
    c.ic->reset();
    Check(c.Commits() == 1 && c.Preedit().empty(), "the app's reset: the app keeps or drops its own marked text, bada adds nothing");
    c.Type("k i");
    CheckEq(c.Preedit(), "き", "after a reset nothing is left staged");
    c.ic->focusOut();

    // A second field follows the session's mode and profile; 無変換 there switches for every field.
    Client b("other");
    b.Focus();
    b.Type("s a");
    CheckEq(b.Preedit(), "さ", "another field follows the mode and the profile");
    b.Type("Muhenkan x");
    CheckEq(b.text, "さx", "無変換 there commits and switches");
    b.ic->focusOut();
    a.Focus();
    a.text.clear();
    a.Type("y");
    Check(a.text == "y" && a.Preedit().empty(), "the first field followed to muhenkan");

    // A password field: nothing composes, and nothing typed is published.
    a.Type("Henkan");
    a.ic->focusOut();
    Client p("pw", fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit, fcitx::CapabilityFlag::Password});
    p.Focus();
    CheckEq(g_instance->inputMethod(p.ic), "bada", "bada is in use in the password field");
    p.Type("g k s");
    Check(p.text == "gks" && p.Preedit().empty() && p.Commits() == 0, "a password field gets plain keys");
    state = StateFile();
    Check(state.GetString("mode") == "henkan" && state.Get("harness")->GetBool("secure") && state.GetString("staged").empty() &&
              bada::json::Write(state).find("gks") == std::string::npos,
          "the state hides a password field: " + bada::json::Write(state));
    // The JIS characters are still typed there, and language keys still switch mode.
    Check(p.Key("Equal") && p.text == "gks^", "a password field still gets JIS characters");
    p.Key("Equal", true);
    p.Type("Muhenkan");
    CheckEq(StateFile().GetString("mode"), "muhenkan", "a password field still switches mode");
    p.Type("Henkan");
    p.ic->focusOut();

    // A file that can't be used changes nothing, and its problems are published.
    a.Focus();
    WriteFile(g_config / "settings.json", "{ this is not json");
}

// Phase 4: the broken file changed nothing.
void BrokenSettings(Client& a) {
    const bada::json::Value state = StateFile();
    const bada::json::Value* problems = state.Get("harness") ? state.Get("harness")->Get("problems") : nullptr;
    Check(problems && problems->array.size() == 1 && problems->array[0].GetString("code") == "bad-json" &&
              problems->array[0].GetString("file") == "settings.json",
          "the broken file's problem is published: " + bada::json::Write(state));
    a.text.clear();
    a.Type("Muhenkan Katakana k a Enter");
    CheckEq(a.text, "か\n", "the settings from before are still in effect");
    // Another input method takes over: what is staged is committed first.
    a.Type("k a");
    const size_t before = a.Commits();
    g_instance->setCurrentInputMethod(a.ic, "keyboard-us", false);
    a.TakeCommits();
    Check(a.Commits() == before + 1 && a.text == "か\nか", "switching input method commits what is staged");
}

// Runs the phases one after another, with the event loop turning in between so that the folder's
// watch can fire.
std::vector<std::unique_ptr<fcitx::EventSourceTime>> g_timers;

void Schedule(fcitx::EventDispatcher* dispatcher, std::vector<std::function<void()>> phases, size_t at = 0) {
    if (at >= phases.size()) {
        dispatcher->schedule([dispatcher] {
            dispatcher->detach();
            g_instance->exit();
        });
        return;
    }
    g_timers.push_back(g_instance->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 300000, 0,
        [dispatcher, phases, at](fcitx::EventSourceTime*, uint64_t) {
            phases[at]();
            Schedule(dispatcher, phases, at + 1);
            return true;
        }));
}

}  // namespace

int main() {
    // The test's own settings folder, runtime folder and (if dbus-run-session wraps it) bus.
    char config[] = "/tmp/bada-e2e-config-XXXXXX", runtime[] = "/tmp/bada-e2e-run-XXXXXX";
    g_config = ::mkdtemp(config);
    g_runtime = ::mkdtemp(runtime);
    ::setenv("BADA_CONFIG_DIR", g_config.c_str(), 1);
    ::setenv("XDG_RUNTIME_DIR", g_runtime.c_str(), 1);
    // No data, whatever is installed on this machine: what the test types doesn't depend on it.
    ::setenv("BADA_DATA_DIR", (g_config / "no-data").c_str(), 1);

    fcitx::setupTestingEnvironment(BADA_TEST_BINARY_DIR, {BADA_TEST_ADDON_DIR}, {BADA_TEST_DATA_DIR});
    char arg0[] = "bada_fcitx5_e2e", arg1[] = "--disable=all", arg2[] = "--enable=testim,testfrontend,testui,bada";
    char* argv[] = {arg0, arg1, arg2};
    fcitx::Log::setLogRule("default=3,bada=5");
    fcitx::Instance instance(3, argv);
    g_instance = &instance;
    instance.addonManager().registerDefaultLoader(nullptr);

    const auto commits = instance.watchEvent(fcitx::EventType::InputContextCommitString, fcitx::EventWatcherPhase::PreInputMethod,
                                             [](fcitx::Event& event) {
                                                 auto& e = static_cast<fcitx::CommitStringEvent&>(event);
                                                 g_commits[e.inputContext()].push_back(e.text());
                                             });
    const auto forwards = instance.watchEvent(fcitx::EventType::InputContextForwardKey, fcitx::EventWatcherPhase::PreInputMethod,
                                              [](fcitx::Event& event) {
                                                  ++g_forwarded[static_cast<fcitx::ForwardKeyEvent&>(event).inputContext()];
                                              });

    fcitx::EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    std::unique_ptr<Client> a;
    dispatcher.schedule([&] {
        // Fcitx5 keeps input methods out of password fields unless told otherwise. It is told
        // here, so that bada's own handling of one is tested.
        fcitx::RawConfig global;
        global.setValueByPath("Behavior/AllowInputMethodForPassword", "True");
        instance.globalConfig().load(global, true);
        Check(instance.globalConfig().allowInputMethodForPassword(), "Fcitx5 is told to allow input methods in password fields");
        g_frontend = instance.addonManager().addon("testfrontend");
        Check(g_frontend != nullptr, "the test frontend is loaded");
        Check(instance.inputMethodManager().entry("bada") != nullptr, "the input method bada is listed");
        // The addon is loaded on demand; ask for it, as the first use would.
        Check(instance.addonManager().addon("bada", true) != nullptr, "the addon bada loads");
        if (!g_frontend || !instance.inputMethodManager().entry("bada") || !instance.addonManager().addon("bada")) {
            dispatcher.detach();
            instance.exit();
            return;
        }
        auto group = instance.inputMethodManager().currentGroup();
        group.inputMethodList().clear();
        group.inputMethodList().push_back(fcitx::InputMethodGroupItem("keyboard-us"));
        group.inputMethodList().push_back(fcitx::InputMethodGroupItem("bada"));
        group.setDefaultInputMethod("bada");
        instance.inputMethodManager().setGroup(group);
        a = std::make_unique<Client>("e2e");
        Schedule(&dispatcher, {[&] { NewInstall(*a); }, [&] { FirstSettings(*a); }, [&] { SecondSettings(*a); },
                               [&] { BrokenSettings(*a); }});
    });
    instance.exec();
    g_timers.clear();

    std::error_code ec;
    fs::remove_all(g_config, ec);
    fs::remove_all(g_runtime, ec);
    return test::Finish("fcitx5 e2e");
}
