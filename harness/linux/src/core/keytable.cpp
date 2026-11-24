#include "keytable.h"

#include <algorithm>
#include <utility>

namespace bada {
namespace {

constexpr const char* kFile = "settings.json";

bool IsIdChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

// A profile id: [a-z0-9-]{1,32}.
bool IsProfileId(const std::string& s) {
    return !s.empty() && s.size() <= 32 && std::all_of(s.begin(), s.end(), IsIdChar);
}

// A key code: [A-Z][A-Za-z0-9]*.
bool IsCode(const std::string& s) {
    if (s.empty() || s[0] < 'A' || s[0] > 'Z') return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    });
}

// A layout id: a group of two or three letters, then one or more -parts.
bool IsLayoutId(const std::string& s) {
    const size_t dash = s.find('-');
    if (dash == std::string::npos || dash < 2 || dash > 3) return false;
    for (size_t i = 0; i < dash; ++i) {
        if (s[i] < 'a' || s[i] > 'z') return false;
    }
    bool part = false;
    for (size_t i = dash + 1; i < s.size(); ++i) {
        if (s[i] == '-') {
            if (!part) return false;
            part = false;
        } else if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9')) {
            part = true;
        } else {
            return false;
        }
    }
    return part;
}

// `<layout id>.<action id>`: the two halves, or false.
bool SplitLayoutAction(const std::string& s, std::string* layout, std::string* action) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return false;
    const std::string l = s.substr(0, dot), a = s.substr(dot + 1);
    if (!IsLayoutId(l) || a.empty() || a.front() == '-' || a.back() == '-') return false;
    if (!std::all_of(a.begin(), a.end(), IsIdChar) || a.find("--") != std::string::npos) return false;
    if (layout) *layout = l;
    if (action) *action = a;
    return true;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// An action the standard has (§9.2).
bool IsAction(const std::string& a) {
    static const char* const kNamed[] = {"mode.toggle",  "mode:muhenkan", "mode:henkan",  "profile.first",
                                         "profile.next", "profile.prev",  "profile.last", "menu.next",
                                         "menu.prev",    "fix",           "width.full",   "width.half",
                                         "pass",         "none"};
    for (const char* name : kNamed) {
        if (a == name) return true;
    }
    if (StartsWith(a, "profile:")) return IsProfileId(a.substr(8));
    return SplitLayoutAction(a, nullptr, nullptr);
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t from = 0;
    while (true) {
        const size_t at = s.find(sep, from);
        parts.push_back(s.substr(from, at == std::string::npos ? at : at - from));
        if (at == std::string::npos) break;
        from = at + 1;
    }
    return parts;
}

const Profile* Find(const std::vector<Profile>& profiles, const std::string& id) {
    for (const Profile& p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Binding Bind(int index, const char* on, std::vector<std::string> actions, const char* when = nullptr) {
    Binding b;
    const Press p = *ParsePress(on);
    b.key = p.key;
    b.key2 = p.key2;
    b.mods = p.mods;
    b.actions = std::move(actions);
    b.index = index;
    if (when) b.when = std::string(when) == "henkan" ? Binding::When::kHenkan : Binding::When::kMuhenkan;
    return b;
}

void Add(std::vector<Problem>* problems, const std::string& path, const char* code, std::string message,
         const char* level = "error") {
    if (problems) problems->push_back({kFile, path, code, std::move(message), level});
}

}  // namespace

const char* ThenName(Decision::Then then) {
    switch (then) {
        case Decision::Then::kSelect: return "select";
        case Decision::Then::kAction: return "action";
        case Decision::Then::kKey: return "key";
        case Decision::Then::kType: return "type";
        case Decision::Then::kPass: return "pass";
        case Decision::Then::kSwallow: return "swallow";
        case Decision::Then::kOnboarding: return "onboarding";
    }
    return "";
}

std::optional<Press> ParsePress(const std::string& text) {
    const std::vector<std::string> parts = Split(text, '+');
    Press p;
    // Two language keys held together, in either order, with no modifiers.
    if (parts.size() == 2 && IsLanguageKey(parts[0]) && IsLanguageKey(parts[1])) {
        if (parts[0] == parts[1]) return std::nullopt;
        p.key = parts[0];
        p.key2 = parts[1];
        return p;
    }
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        bool* mod = parts[i] == "ctrl"    ? &p.mods.ctrl
                    : parts[i] == "alt"   ? &p.mods.alt
                    : parts[i] == "shift" ? &p.mods.shift
                    : parts[i] == "meta"  ? &p.mods.meta
                                          : nullptr;
        if (!mod || *mod) return std::nullopt;
        *mod = true;
    }
    if (!IsCode(parts.back())) return std::nullopt;
    p.key = parts.back();
    return p;
}

KeyTable KeyTable::Default(Preset preset) {
    KeyTable t;
    switch (preset) {
        case Preset::kJisPc:
        case Preset::kJisMac: {
            const bool pc = preset == Preset::kJisPc;
            const char* off = pc ? "NonConvert" : "Lang2";
            const std::string on = pc ? "Convert" : "Lang1";
            t.bindings = {Bind(0, off, {"mode:muhenkan"}),
                          Bind(1, on.c_str(), {"profile.first"}),
                          Bind(2, on.c_str(), {"menu.next", "profile.first"}, "henkan"),
                          Bind(3, ("shift+" + on).c_str(), {"menu.prev"}, "henkan"),
                          Bind(4, ("ctrl+" + on).c_str(), {"fix"}, "henkan"),
                          Bind(5, "Backquote", {"width.full"}),
                          Bind(6, "shift+Backquote", {"width.half"})};
            break;
        }
        case Preset::kNoLanguageKeys:
            t.bindings = {Bind(0, "CapsLock", {"mode.toggle"})};
            break;
    }
    return t;
}

std::optional<KeyTable> KeyTable::Parse(const json::Value* keys, Preset preset, std::vector<Problem>* problems) {
    KeyTable t = Default(preset);
    if (!keys) return t;
    if (!keys->IsObject()) {
        Add(problems, "keys", "bad-value", "keys is an object");
        return std::nullopt;
    }
    bool ok = true;
    auto error = [&](const std::string& path, const char* code, std::string message) {
        Add(problems, path, code, std::move(message));
        ok = false;
    };
    for (const auto& [name, value] : keys->object) {
        (void)value;
        if (name != "bindings" && name != "cycle" && name != "henkan_enters" && name != "caps_lock" &&
            name != "caps_rule") {
            error("keys." + name, "unknown-field", "the key table has no field named " + name);
        }
    }
    if (const json::Value* list = keys->Get("bindings")) {
        t.bindings.clear();
        if (!list->IsArray()) {
            error("keys.bindings", "bad-value", "bindings is a list");
        } else {
            for (size_t i = 0; i < list->array.size(); ++i) {
                const json::Value& item = list->array[i];
                const std::string at = "keys.bindings[" + std::to_string(i) + "]";
                if (!item.IsObject()) {
                    error(at, "bad-value", "a binding is an object");
                    continue;
                }
                Binding b;
                b.index = static_cast<int>(i);
                for (const auto& [name, value] : item.object) {
                    (void)value;
                    if (name != "on" && name != "do" && name != "when") {
                        error(at + "." + name, "unknown-field", "a binding has no field named " + name);
                    }
                }
                const json::Value* on = item.Get("on");
                const std::optional<Press> press = on && on->IsString() ? ParsePress(on->string) : std::nullopt;
                if (!press) {
                    error(at + ".on", "bad-value",
                          "on is a key's code, with ctrl, alt, shift, meta before it, or two language keys");
                } else {
                    b.key = press->key;
                    b.key2 = press->key2;
                    b.mods = press->mods;
                }
                const json::Value* actions = item.Get("do");
                if (!actions || !actions->IsArray() || actions->array.empty()) {
                    error(at + ".do", "bad-value", "do is a list of one action or more");
                } else {
                    for (size_t j = 0; j < actions->array.size(); ++j) {
                        const json::Value& a = actions->array[j];
                        const std::string path = at + ".do[" + std::to_string(j) + "]";
                        if (!a.IsString()) {
                            error(path, "bad-value", "an action is a string");
                        } else if (!IsAction(a.string)) {
                            error(path, "unknown-action", "no action named " + a.string);
                        } else {
                            b.actions.push_back(a.string);
                        }
                    }
                }
                if (const json::Value* when = item.Get("when")) {
                    const std::string w = when->IsString() ? when->string : std::string();
                    if (w == "muhenkan") {
                        b.when = Binding::When::kMuhenkan;
                    } else if (w == "henkan") {
                        b.when = Binding::When::kHenkan;
                    } else if (StartsWith(w, "profile:") && IsProfileId(w.substr(8))) {
                        b.when = Binding::When::kProfile;
                        b.when_profile = w.substr(8);
                    } else {
                        error(at + ".when", "bad-value", "when is muhenkan, henkan or profile:<id>");
                    }
                }
                t.bindings.push_back(std::move(b));
            }
        }
    }
    if (const json::Value* cycle = keys->Get("cycle")) {
        if (!cycle->IsArray()) {
            error("keys.cycle", "bad-value", "cycle is a list of profile ids");
        } else {
            for (size_t i = 0; i < cycle->array.size(); ++i) {
                const json::Value& id = cycle->array[i];
                const std::string path = "keys.cycle[" + std::to_string(i) + "]";
                if (!id.IsString() || !IsProfileId(id.string)) {
                    error(path, "bad-value", "a profile id is 1 to 32 of a-z, 0-9 and -");
                } else if (std::find(t.cycle.begin(), t.cycle.end(), id.string) != t.cycle.end()) {
                    error(path, "bad-value", id.string + " is listed twice");
                } else {
                    t.cycle.push_back(id.string);
                }
            }
        }
    }
    if (const json::Value* enters = keys->Get("henkan_enters")) {
        if (!enters->IsString() || !IsProfileId(enters->string)) {
            error("keys.henkan_enters", "bad-value", "henkan_enters is last or a profile id");
        } else {
            t.henkan_enters = enters->string;
        }
    }
    if (const json::Value* caps = keys->Get("caps_lock")) {
        const std::string v = caps->IsString() ? caps->string : std::string();
        if (v == "respect") t.caps_lock = CapsSetting::kRespect;
        else if (v == "transient") t.caps_lock = CapsSetting::kTransient;
        else error("keys.caps_lock", "bad-value", "caps_lock is respect or transient");
    }
    if (const json::Value* rule = keys->Get("caps_rule")) {
        const std::string v = rule->IsString() ? rule->string : std::string();
        if (v == "allcaps") t.caps_rule = CapsRule::kAllCaps;
        else if (v == "upper_snake") t.caps_rule = CapsRule::kUpperSnake;
        else error("keys.caps_rule", "bad-value", "caps_rule is allcaps or upper_snake");
    }
    if (!ok) return std::nullopt;
    return t;
}

int Binding::Rank(const TableState& state) const {
    const bool henkan = state.mode == Mode::kHenkan;
    switch (when) {
        case When::kAlways: return 1;
        case When::kMuhenkan: return henkan ? 0 : 2;
        case When::kHenkan: return henkan ? 2 : 0;
        case When::kProfile: return henkan && state.profile == when_profile ? 3 : 0;
    }
    return 0;
}

bool KeyTable::HasPair(const std::string& a, const std::string& b, const TableState& state) const {
    for (const Binding& x : bindings) {
        if (x.key2.empty() || !x.Rank(state)) continue;
        if ((x.key == a && x.key2 == b) || (x.key == b && x.key2 == a)) return true;
    }
    return false;
}

bool KeyTable::InBoundPair(const std::string& key, const TableState& state) const {
    for (const Binding& x : bindings) {
        if (!x.key2.empty() && (x.key == key || x.key2 == key) && x.Rank(state)) return true;
    }
    return false;
}

bool KeyTable::HasBindingOn(const std::string& key, const Mods& mods) const {
    for (const Binding& x : bindings) {
        if (x.key2.empty() && x.key == key && x.mods == mods) return true;
    }
    return false;
}

void TableState::Selected(Mode to, const std::string& id) {
    if (to == Mode::kHenkan && id != profile) {
        if (!profile.empty()) previous = profile;
        profile = id;
    }
    mode = to;
}

Decision Decide(const KeyTable& table, const std::vector<Profile>& profiles, const TableState& state,
                const Press& press) {
    Decision d;
    const bool henkan = state.mode == Mode::kHenkan;

    // The most specific binding that holds: a profile's, then a mode's, then one with no `when`;
    // among equals, the later one.
    const Binding* chosen = nullptr;
    int rank = 0;
    for (const Binding& b : table.bindings) {
        if (press.key2.empty()) {
            if (!b.key2.empty() || b.key != press.key || !(b.mods == press.mods)) continue;
        } else {
            const bool same = (b.key == press.key && b.key2 == press.key2) ||
                              (b.key == press.key2 && b.key2 == press.key);
            if (b.key2.empty() || !same) continue;
        }
        const int r = b.Rank(state);
        if (r && r >= rank) {
            rank = r;
            chosen = &b;
        }
    }

    if (!chosen) {
        if (!press.key2.empty()) {
            d.then = Decision::Then::kSwallow;  // a pair nothing is bound to
            return d;
        }
        if (press.key == "CapsLock") {
            // Unbound, the key is the lock (§9.4), with any modifiers.
            if (table.caps_lock == CapsSetting::kTransient && henkan) {
                d.then = Decision::Then::kSwallow;
                d.caps_lock = false;
            } else {
                d.then = Decision::Then::kPass;
                d.caps_lock = !state.caps_lock;
            }
            return d;
        }
        if (IsLanguageKey(press.key)) {
            d.then = Decision::Then::kPass;  // never the engine's: the OS has it
            return d;
        }
        // UPPER_SNAKE (§9.4): with the lock on in muhenkan, Space types _ and ⌥Space a space.
        if (table.caps_rule == CapsRule::kUpperSnake && !henkan && state.caps_lock && press.key == "Space" &&
            !press.mods.ctrl && !press.mods.meta) {
            d.then = Decision::Then::kType;
            d.text = press.mods.alt ? " " : "_";
            return d;
        }
        d.then = Decision::Then::kKey;
        return d;
    }

    d.binding = chosen->index;
    auto select = [&](Mode mode, const std::string& profile, const std::string& ran) {
        d.then = Decision::Then::kSelect;
        d.ran = ran;
        d.mode = mode;
        d.profile = profile;
        // transient: the lock is turned off when muhenkan is left.
        if (table.caps_lock == CapsSetting::kTransient && !henkan && mode == Mode::kHenkan) d.caps_lock = false;
    };
    auto onboarding = [&](const std::string& ran) {
        d.then = Decision::Then::kOnboarding;
        d.ran = ran;
    };
    // The profile `mode:henkan` and the toggle enter.
    auto enters = [&]() -> std::string {
        if (table.henkan_enters != "last") {
            return Find(profiles, table.henkan_enters) ? table.henkan_enters : profiles.front().id;
        }
        return Find(profiles, state.profile) ? state.profile : profiles.front().id;
    };
    // Enters henkan in `id`, unless it is in use there.
    auto profile = [&](const std::string& id, const std::string& ran) {
        if (henkan && state.profile == id) return false;
        select(Mode::kHenkan, id, ran);
        return true;
    };

    for (const std::string& a : chosen->actions) {
        if (a == "mode.toggle") {
            if (henkan) select(Mode::kMuhenkan, state.profile, a);
            else if (profiles.empty()) onboarding(a);
            else select(Mode::kHenkan, enters(), a);
            return d;
        }
        if (a == "mode:muhenkan") {
            if (!henkan) continue;
            select(Mode::kMuhenkan, state.profile, a);
            return d;
        }
        if (a == "mode:henkan") {
            if (henkan) continue;
            if (profiles.empty()) onboarding(a);
            else select(Mode::kHenkan, enters(), a);
            return d;
        }
        if (a == "profile.first" || a == "profile.next" || a == "profile.prev" || a == "profile.last" ||
            StartsWith(a, "profile:")) {
            // With no profile at all, any action that would enter henkan opens the onboarding.
            if (profiles.empty()) {
                onboarding(a);
                return d;
            }
            if (a == "profile.first") {
                if (profile(profiles.front().id, a)) return d;
            } else if (a == "profile.last") {
                // The profile used before this one; in muhenkan, the one last used.
                const std::string& target = henkan ? state.previous : state.profile;
                if (!target.empty() && Find(profiles, target) && profile(target, a)) return d;
            } else if (a == "profile.next" || a == "profile.prev") {
                std::vector<std::string> cycle;
                for (const std::string& id : table.cycle) {
                    if (Find(profiles, id)) cycle.push_back(id);
                }
                if (cycle.empty()) continue;
                const auto at = std::find(cycle.begin(), cycle.end(), state.profile);
                const size_t n = cycle.size();
                const bool next = a == "profile.next";
                size_t to;
                if (at == cycle.end()) to = next ? 0 : n - 1;
                else to = (static_cast<size_t>(at - cycle.begin()) + (next ? 1 : n - 1)) % n;
                if (profile(cycle[to], a)) return d;
            } else {
                const std::string id = a.substr(8);
                if (Find(profiles, id) && profile(id, a)) return d;
            }
            continue;
        }
        if (a == "menu.next" || a == "menu.prev") {
            if (state.menu_rows <= 1) continue;
        } else if (a == "fix") {
            if (!state.staged) continue;
        } else if (a == "width.full" || a == "width.half") {
            // always
        } else if (a == "pass") {
            d.then = Decision::Then::kPass;
            d.ran = a;
            // Caps Lock given back is the lock again (§9.4).
            if (press.key == "CapsLock") {
                d.caps_lock = table.caps_lock == CapsSetting::kTransient && henkan ? false : !state.caps_lock;
            }
            return d;
        } else if (a == "none") {
            d.then = Decision::Then::kSwallow;
            d.ran = a;
            return d;
        } else {
            // A layout's action: in henkan, with its layout among the candidates of the profile.
            std::string layout;
            if (!SplitLayoutAction(a, &layout, nullptr) || !henkan) continue;
            const Profile* p = Find(profiles, state.profile);
            if (!p || std::find(p->layouts.begin(), p->layouts.end(), layout) == p->layouts.end()) continue;
        }
        d.then = Decision::Then::kAction;
        d.ran = a;
        d.name = a;
        return d;
    }
    // No action applies: the key is swallowed.
    d.then = Decision::Then::kSwallow;
    return d;
}

KeyTracker::Down KeyTracker::KeyDown(const KeyTable& table, const TableState& state, const std::string& key,
                                     const Mods& mods) {
    Down out;
    tap_.clear();
    if (!first_.empty() && (key == first_ || key == second_)) {
        out.waits = true;  // a repeat of a key that waits
        return out;
    }
    if (!second_.empty()) {
        // Another key before both were released: not a pair, and nothing runs for either.
        first_.clear();
        second_.clear();
    } else if (!first_.empty()) {
        if (IsLanguageKey(key) && !mods.Any() && table.HasPair(first_, key, state)) {
            second_ = key;
            first_up_ = second_up_ = false;
            out.waits = true;
            return out;
        }
        // A key that makes no bound pair with the one waiting: that one runs first.
        out.steps.push_back({Press{first_, {}, {}}, true, false});
        first_.clear();
    }
    if (IsModifierCode(key)) {
        if (!mods.Any()) tap_ = key;
        return out;
    }
    // A key of a bound pair waits, but only for a pair binding that holds now (§9.3).
    if (IsLanguageKey(key) && !mods.Any() && table.InBoundPair(key, state)) {
        first_ = key;
        out.waits = true;
        return out;
    }
    out.steps.push_back({Press{key, {}, mods}, false, false});
    return out;
}

std::vector<KeyTracker::Step> KeyTracker::KeyUp(const std::string& key) {
    std::vector<Step> steps;
    if (!second_.empty()) {
        if (key == first_) first_up_ = true;
        else if (key == second_) second_up_ = true;
        if (first_up_ && second_up_) {
            steps.push_back({Press{first_, second_, {}}, true, false});
            first_.clear();
            second_.clear();
        }
        return steps;
    }
    if (!first_.empty() && key == first_) {
        steps.push_back({Press{first_, {}, {}}, true, false});
        first_.clear();
        return steps;
    }
    if (!tap_.empty() && key == tap_) {
        steps.push_back({Press{key, {}, {}}, true, true});
        tap_.clear();
    }
    return steps;
}

void KeyTracker::Clear() {
    first_.clear();
    second_.clear();
    first_up_ = second_up_ = false;
    tap_.clear();
}

}  // namespace bada
