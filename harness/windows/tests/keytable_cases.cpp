// Replays the standard's key-table cases (docs/cases/key-table.json, `bada-key-cases/1`) on
// src/core/keytable: every case, and every sequence of presses and releases.
//
//   bada_keytable_cases <key-table.json>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "json.h"
#include "keytable.h"
#include "settings.h"

using namespace bada;
using test::Check;

namespace {

std::string Show(const Decision& d) {
    std::string s = std::string("then ") + ThenName(d.then) + ", binding " +
                    (d.binding < 0 ? "null" : std::to_string(d.binding)) + ", ran " + (d.ran.empty() ? "null" : d.ran);
    if (d.then == Decision::Then::kSelect) s += std::string(", ") + ModeName(d.mode) + " " + (d.profile.empty() ? "null" : d.profile);
    if (d.then == Decision::Then::kAction) s += ", name " + d.name;
    if (d.then == Decision::Then::kType) s += ", text \"" + d.text + "\"";
    if (d.caps_lock) s += std::string(", caps_lock ") + (*d.caps_lock ? "true" : "false");
    return s;
}

// Where a decision differs from what a case expects; "" when it doesn't.
std::string Differs(const json::Value& expect, const Decision& d, const TableState& before) {
    if (expect.GetString("then") != ThenName(d.then)) return "then";
    const json::Value* binding = expect.Get("binding");
    const int want_binding = binding && binding->IsNumber() ? static_cast<int>(binding->number) : -1;
    if (want_binding != d.binding) return "binding";
    const json::Value* ran = expect.Get("ran");
    if ((ran && ran->IsString() ? ran->string : std::string()) != d.ran) return "ran";
    if (const json::Value* mode = expect.Get("mode"); mode && mode->string != ModeName(d.mode)) return "mode";
    if (const json::Value* profile = expect.Get("profile")) {
        if ((profile->IsString() ? profile->string : std::string()) != d.profile) return "profile";
    }
    if (const json::Value* name = expect.Get("name"); name && name->string != d.name) return "name";
    if (const json::Value* text = expect.Get("text"); text && text->string != d.text) return "text";
    if (const json::Value* caps = expect.Get("caps_lock")) {
        // The lock afterwards: what the press made it, or what it was.
        if (caps->boolean != d.caps_lock.value_or(before.caps_lock)) return "caps_lock";
    }
    return "";
}

TableState StateOf(const json::Value& s) {
    TableState state;
    state.mode = s.GetString("mode") == "henkan" ? Mode::kHenkan : Mode::kMuhenkan;
    state.profile = s.GetString("profile");
    state.previous = s.GetString("previous");
    state.menu_rows = static_cast<size_t>(s.GetNumber("menu_rows"));
    state.staged = !s.GetString("staged").empty();
    state.caps_lock = s.GetBool("caps_lock");
    return state;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bada_keytable_cases <key-table.json>\n");
        return 2;
    }
    std::string text;
    auto doc = ReadFile(std::filesystem::path(argv[1]), &text) ? json::Parse(text) : std::nullopt;
    if (!doc || doc->GetString("format") != "bada-key-cases/1") {
        std::fprintf(stderr, "%s: not a bada-key-cases/1 file\n", argv[1]);
        return 2;
    }

    std::map<std::string, std::vector<Profile>> profiles;
    for (const auto& [name, list] : doc->Get("profiles")->object) {
        for (const json::Value& p : list.array) {
            Profile profile;
            profile.id = p.GetString("id");
            profile.name = profile.id;
            for (const json::Value& l : p.Get("layouts")->array) profile.layouts.push_back(l.string);
            profiles[name].push_back(std::move(profile));
        }
    }
    // The tables are read as a settings file's `keys` part is: by KeyTable::Parse.
    std::map<std::string, KeyTable> tables;
    for (const auto& [name, keys] : doc->Get("tables")->object) {
        std::vector<Problem> problems;
        auto table = KeyTable::Parse(&keys, Preset::kJisPc, &problems);
        Check(table.has_value(), "table " + name + " is read");
        for (const Problem& p : problems) std::printf("  %s: %s %s: %s\n", name.c_str(), p.path.c_str(), p.code.c_str(), p.message.c_str());
        if (table) tables[name] = std::move(*table);
    }

    int cases = 0, sequences = 0;
    for (const json::Value& c : doc->Get("cases")->array) {
        ++cases;
        const std::string name = c.GetString("name");
        const KeyTable& table = tables[c.GetString("table")];
        const TableState state = StateOf(*c.Get("state"));
        const auto press = ParsePress(c.GetString("press"));
        Check(press.has_value(), "case \"" + name + "\": the press " + c.GetString("press") + " is read");
        if (!press) continue;
        const Decision d = Decide(table, profiles[c.GetString("profiles")], state, *press);
        const std::string differs = Differs(*c.Get("expect"), d, state);
        Check(differs.empty(), "case \"" + name + "\": " + differs + " differs. Got: " + Show(d) +
                                   ". Expected: " + json::Write(*c.Get("expect")));
    }

    if (const json::Value* list = doc->Get("sequences")) {
        for (const json::Value& s : list->array) {
            ++sequences;
            const std::string name = s.GetString("name");
            const KeyTable& table = tables[s.GetString("table")];
            const std::vector<Profile>& list_of = profiles[s.GetString("profiles")];
            TableState state = StateOf(*s.Get("state"));
            KeyTracker tracker;
            struct Happened {
                size_t at;
                Decision d;
                TableState before;
            };
            std::vector<Happened> happened;
            const auto& keys = s.Get("keys")->array;
            for (size_t i = 0; i < keys.size(); ++i) {
                const std::string& k = keys[i].string;
                const bool down = k.rfind("down:", 0) == 0;
                const std::string code = k.substr(k.find(':') + 1);
                const std::vector<KeyTracker::Step> steps =
                    down ? tracker.KeyDown(table, state, code, Mods{}).steps : tracker.KeyUp(code);
                for (const KeyTracker::Step& step : steps) {
                    const Decision d = Decide(table, list_of, state, step.press);
                    happened.push_back({i, d, state});
                    // What the harness then does to its own state.
                    if (d.then == Decision::Then::kSelect) state.Selected(d.mode, d.profile);
                    if (d.caps_lock) state.caps_lock = *d.caps_lock;
                }
            }
            const auto& expect = s.Get("expect")->array;
            std::string got;
            for (const Happened& h : happened) got += "\n    at " + std::to_string(h.at) + ": " + Show(h.d);
            bool same = expect.size() == happened.size();
            for (size_t i = 0; same && i < expect.size(); ++i) {
                same = static_cast<size_t>(expect[i].GetNumber("at")) == happened[i].at &&
                       Differs(expect[i], happened[i].d, happened[i].before).empty();
            }
            Check(same, "sequence \"" + name + "\" differs. Got:" + got + "\n  Expected: " + json::Write(*s.Get("expect")));
        }
    }

    // The presets of §9.5 are the tables the cases call jis-mac, jis-pc and no-language-keys.
    const std::pair<Preset, const char*> presets[] = {
        {Preset::kJisMac, "jis-mac"}, {Preset::kJisPc, "jis-pc"}, {Preset::kNoLanguageKeys, "no-language-keys"}};
    for (const auto& [preset, name] : presets) {
        const KeyTable mine = KeyTable::Default(preset);
        const KeyTable& theirs = tables[name];
        bool same = mine.bindings.size() == theirs.bindings.size() && mine.cycle == theirs.cycle &&
                    mine.henkan_enters == theirs.henkan_enters && mine.caps_lock == theirs.caps_lock &&
                    mine.caps_rule == theirs.caps_rule;
        for (size_t i = 0; same && i < mine.bindings.size(); ++i) {
            const Binding &a = mine.bindings[i], &b = theirs.bindings[i];
            same = a.key == b.key && a.key2 == b.key2 && a.mods == b.mods && a.actions == b.actions && a.when == b.when;
        }
        Check(same, std::string("the preset ") + name + " is the standard's");
    }

    std::printf("%d cases, %d sequences\n", cases, sequences);
    return test::Finish("key table");
}
