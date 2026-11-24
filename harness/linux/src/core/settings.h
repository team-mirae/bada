// The settings folder (docs/bada-compatible-v2.md §10.1): ~/.config/bada, with settings.json
// (`bada-settings/2`), layouts/*.json and dictionary.json. The harness reads them, feeds the engine
// (`layout`, `settings` and `dictionary` events, in that order), keeps the key table and its own
// part, and watches the folder to do it again when a file changes.
//
// A file that can't be used changes nothing: what was in effect stays, and what was wrong is kept
// as problems, for the published state (§6.7).
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

// This harness's own settings: settings.json's harness["com.teammirae.bada.linux"].
struct HarnessSettings {
    // The keyboard, which picks the default bindings when the file has no keys.bindings (§9.5):
    // "jis-pc" (the default), "jis-mac" or "none". Linux can't tell which keyboard is plugged in.
    Preset keyboard = Preset::kJisPc;
    // Publish the state for other programs (§6.7).
    bool publish_state = true;
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
    explicit SettingsFolder(std::filesystem::path dir);
    ~SettingsFolder();
    SettingsFolder(const SettingsFolder&) = delete;
    SettingsFolder& operator=(const SettingsFolder&) = delete;

    // $BADA_CONFIG_DIR (for tests), else $XDG_CONFIG_HOME/bada, else ~/.config/bada.
    static std::filesystem::path DefaultDir();

    // Reads the files that changed since the last call and feeds them to the session. True when
    // anything was read: what is in effect, or the problems, may have changed.
    bool Reload(EngineSession& session);

    const std::filesystem::path& dir() const { return dir_; }
    const Loaded& loaded() const { return loaded_; }
    // What was wrong with the files last read: settings, then layouts, then the dictionary.
    std::vector<Problem> problems() const;

    // Turns readable when something in the folder changes (inotify); -1 when it can't be watched.
    // The frontend puts it in its event loop and calls Changed(), then Reload().
    int watch_fd() const { return watch_; }
    // Drains the watch. True when a file the harness reads was touched.
    bool Changed();

private:
    // A file's identity on disk, to tell when it changed.
    struct Stamp {
        bool exists = false;
        std::int64_t mtime_s = 0, mtime_ns = 0;
        std::uint64_t size = 0, inode = 0;
        bool operator==(const Stamp& o) const {
            return exists == o.exists && mtime_s == o.mtime_s && mtime_ns == o.mtime_ns && size == o.size &&
                   inode == o.inode;
        }
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
    void Watch();

    std::filesystem::path dir_;
    Loaded loaded_;
    bool read_ = false;  // the first Reload reads everything

    Stamp settings_stamp_;
    std::vector<Problem> settings_problems_;

    std::map<std::string, LayoutFile> layouts_;  // by file name

    Stamp dictionary_stamp_;
    bool dictionary_loaded_ = false;
    std::vector<Problem> dictionary_problems_;

    int watch_ = -1;
    int wd_dir_ = -1, wd_layouts_ = -1, wd_parent_ = -1;
};

// Reads a whole file. False when it can't be read.
bool ReadFile(const std::filesystem::path& path, std::string* out);

}  // namespace bada
