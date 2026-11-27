// The settings folder (docs/bada-compatible-v2.md §10.1): %APPDATA%\bada, with settings.json
// (`bada-settings/2`), layouts\*.json and dictionary.json. The harness reads them, feeds the engine
// (`layout`, `settings` and `dictionary` events, in that order), keeps the key table and its own
// part, and reads them again when a file changed.
//
// A file that can't be used changes nothing: what was in effect stays, and what was wrong is kept
// as problems, for the published state (§6.7). Nothing is ever written here, and nothing is read
// from the registry.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "json.h"
#include "keytable.h"

namespace bada {

// The keyboard, which picks the default bindings when the file has no keys.bindings (§9.5).
enum class Keyboard { kAuto, kJisPc, kJisMac, kNone };

// This harness's own settings: settings.json's harness["com.teammirae.bada.windows"].
struct HarnessSettings {
    // "auto" (the default): `jis-pc` when Windows reports a Japanese keyboard, `none` otherwise.
    // "jis-pc", "jis-mac" (an Apple JIS keyboard: 英数 and かな) or "none" say it outright.
    Keyboard keyboard = Keyboard::kAuto;
    // Publish the state for other programs (§6.7).
    bool publish_state = true;
    // Write the diagnostic log: activations, focus and errors, never typed text.
    bool log = false;

    // One app's settings. `menu: false` draws no candidate menu: the app shows the preedit itself.
    struct App {
        bool menu = true;
        bool operator==(const App& o) const { return menu == o.menu; }
    };
    // Per app, by the program's file name in lower case (`exefile.exe`). An entry in the file
    // replaces the default for its app; the defaults list EVE Online, which draws its own
    // composition box.
    std::map<std::string, App> apps = DefaultApps();
    static std::map<std::string, App> DefaultApps() { return {{"exefile.exe", App{false}}}; }

    // Whether the candidate menu is drawn in the program with this file name (any case).
    bool ShowsMenu(const std::string& exe) const;
};

// What is in effect.
struct Loaded {
    std::vector<Profile> profiles;  // engine.henkan.profiles, in order
    std::string muhenkan_layout;    // engine.muhenkan.layout; "" unset
    json::Value engine;             // the `engine` part, as the engine answered it
    KeyTable table;
    HarnessSettings harness;
};

class SettingsFolder {
public:
    // `detected`: the preset for `keyboard: "auto"`, as the text service found the keyboard.
    explicit SettingsFolder(std::filesystem::path dir, Preset detected = Preset::kNoLanguageKeys);
    SettingsFolder(const SettingsFolder&) = delete;
    SettingsFolder& operator=(const SettingsFolder&) = delete;

    // %BADA_CONFIG_DIR% (for tests), else %APPDATA%\bada.
    static std::filesystem::path DefaultDir();

    // Reads the files that changed since the last call and feeds them to the session. True when
    // anything was read: what is in effect, or the problems, may have changed.
    bool Reload(EngineSession& session);

    const std::filesystem::path& dir() const { return dir_; }
    const Loaded& loaded() const { return loaded_; }
    // What was wrong with the files last read: settings, then layouts, then the dictionary.
    std::vector<Problem> problems() const;

private:
    // A file's identity on disk, to tell when it changed.
    struct Stamp {
        bool exists = false;
        std::int64_t mtime = 0;
        std::uint64_t size = 0;
        bool operator==(const Stamp& o) const { return exists == o.exists && mtime == o.mtime && size == o.size; }
    };
    static Stamp StampOf(const std::filesystem::path& path);

    struct LayoutFile {
        Stamp stamp;
        std::string id;  // the layout it loaded as; "" when it never loaded
        std::vector<Problem> problems;
    };

    bool ReloadLayouts(EngineSession& session);
    bool ReloadSettings(EngineSession& session, bool layouts_changed);
    bool ReloadDictionary(EngineSession& session);

    std::filesystem::path dir_;
    Preset detected_;
    Loaded loaded_;
    bool read_ = false;  // the first Reload reads everything

    Stamp settings_stamp_;
    std::vector<Problem> settings_problems_;

    std::map<std::string, LayoutFile> layouts_;  // by file name

    Stamp dictionary_stamp_;
    bool dictionary_loaded_ = false;
    std::vector<Problem> dictionary_problems_;
};

// Reads a whole file, without a UTF-8 byte order mark. False when it can't be read.
bool ReadFile(const std::filesystem::path& path, std::string* out);

}  // namespace bada
