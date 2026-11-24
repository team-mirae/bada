// The key table (docs/bada-compatible-v2.md §9): which keys the harness takes for itself, and what
// each does. Language keys and Caps Lock never reach the engine; this decides what they do instead.
//
// Pure C++: no Fcitx5 or IBus types, and no engine. It is given the table, the profile list and the
// harness's state, and answers what must happen. tests/keytable_cases.cpp replays the standard's
// cases (docs/cases/key-table.json) on it.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "json.h"
#include "keys.h"

namespace bada {

enum class Mode { kMuhenkan, kHenkan };
inline const char* ModeName(Mode m) { return m == Mode::kHenkan ? "henkan" : "muhenkan"; }

// What was wrong with a file, as the engine reports it (§6.6), with the file it was in.
struct Problem {
    std::string file;   // "settings.json", "layouts/my.json", "dictionary.json"
    std::string path;   // where in the file: "keys.bindings[3].do[0]"
    std::string code;   // "bad-value", "unknown-action", ...
    std::string message;
    std::string level;  // "error" or "warning"
};

// A henkan profile, as much of it as the harness reads (§8.1).
struct Profile {
    std::string id;
    std::string name;
    std::vector<std::string> layouts;
};

struct TableState;

// One line of the table: a key, what it does, and when.
struct Binding {
    std::string key;   // the key's code; for a pair, one of the two
    std::string key2;  // a pair's other key; "" when `on` is one key
    Mods mods;         // none for a pair
    std::vector<std::string> actions;  // `do`: the first that applies runs
    enum class When { kAlways, kMuhenkan, kHenkan, kProfile } when = When::kAlways;
    std::string when_profile;
    int index = 0;  // its place in the file's `bindings`
    // How specific it is for the state: 0 when it doesn't hold, then 1 (no `when`), 2 (a mode's)
    // and 3 (a profile's).
    int Rank(const TableState& state) const;
};

// What the table reads of the harness (§9.1): its mode and profile, and the last reply.
struct TableState {
    Mode mode = Mode::kMuhenkan;
    std::string profile;   // the profile last selected; "" before the first
    std::string previous;  // the profile in use before that one; "" when none
    size_t menu_rows = 0;  // rows in the last reply's menu
    bool staged = false;   // the last reply's `staged` isn't empty
    bool caps_lock = false;  // the lock is on
    // A select happened: the mode, the profile, and the one before it (§9.2).
    void Selected(Mode to, const std::string& id);
};

enum class CapsSetting { kRespect, kTransient };
enum class CapsRule { kAllCaps, kUpperSnake };
// The default bindings, by keyboard (§9.5).
enum class Preset { kJisPc, kJisMac, kNoLanguageKeys };

struct KeyTable {
    std::vector<Binding> bindings;
    std::vector<std::string> cycle;
    std::string henkan_enters = "last";
    CapsSetting caps_lock = CapsSetting::kRespect;
    CapsRule caps_rule = CapsRule::kAllCaps;

    // Reads the `keys` part of a settings file (nullptr: there is none). With no `bindings`, the
    // preset's. An error-level problem means the table can't be used: nullopt, and nothing changes.
    static std::optional<KeyTable> Parse(const json::Value* keys, Preset preset, std::vector<Problem>* problems);
    static KeyTable Default(Preset preset);

    // Some binding that holds now (its `when`) is on this pair, in either order.
    bool HasPair(const std::string& a, const std::string& b, const TableState& state) const;
    // The key is one of some pair with a binding that holds now.
    bool InBoundPair(const std::string& key, const TableState& state) const;
    // Some binding is on this key alone, with exactly these modifiers.
    bool HasBindingOn(const std::string& key, const Mods& mods) const;
};

// One press to decide: a key with its modifiers, or a pair of language keys (§9.3).
struct Press {
    std::string key;
    std::string key2;  // a pair's other key
    Mods mods;
};
// "shift+Lang1", "KanaMode+Lang1", "KeyA": as `on` is written. nullopt when it isn't one.
std::optional<Press> ParsePress(const std::string& text);

struct Decision {
    enum class Then {
        kSelect,      // send `select` with `mode` and `profile`
        kAction,      // send `action` with `name`
        kKey,         // not the table's: feed the key to the engine
        kType,        // flush, then type `text` (UPPER_SNAKE)
        kPass,        // the app or the OS has the key
        kSwallow,     // nothing
        kOnboarding,  // no profile yet: the onboarding (§8.3)
    };
    Then then = Then::kKey;
    int binding = -1;  // the binding chosen; -1 when none matched
    std::string ran;   // the action of its `do` that ran; "" when none applied
    Mode mode = Mode::kMuhenkan;  // for kSelect
    std::string profile;          // for kSelect; "" is null
    std::string name;             // for kAction
    std::string text;             // for kType
    std::optional<bool> caps_lock;  // the lock afterwards, when the press changes it
};
const char* ThenName(Decision::Then then);

// §9.1 and §9.2: the most specific binding that holds, and the first of its actions that applies.
Decision Decide(const KeyTable& table, const std::vector<Profile>& profiles, const TableState& state,
                const Press& press);

// Presses and releases in order (§9.3): a key of a bound pair runs late, a pair runs when both keys
// are up, and a modifier pressed and released alone is a key of its own.
class KeyTracker {
public:
    struct Step {
        Press press;
        bool late = false;  // an earlier key's press, decided now; false: the key of this event
        bool tap = false;   // a modifier pressed and released alone: only a binding makes it a key
    };
    struct Down {
        std::vector<Step> steps;  // what to decide now, in order
        bool waits = false;       // this key waits for its pair or its release: its press is eaten
    };
    // `state`: the harness's mode and profile now. A key waits only for a pair binding that holds
    // when the key is pressed.
    Down KeyDown(const KeyTable& table, const TableState& state, const std::string& key, const Mods& mods);
    std::vector<Step> KeyUp(const std::string& key);
    // Focus left: nothing waits.
    void Clear();

private:
    std::string first_;   // a key of a bound pair, down, its own binding not run yet
    std::string second_;  // the pair's other key, once both went down
    bool first_up_ = false, second_up_ = false;
    std::string tap_;  // a modifier that went down with nothing else since
};

}  // namespace bada
