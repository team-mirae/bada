#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

namespace bada {
namespace fs = std::filesystem;

namespace {

constexpr const char* kSettingsFile = "settings.json";
constexpr const char* kDictionaryFile = "dictionary.json";
constexpr const char* kLayoutsDir = "layouts";
constexpr const char* kSettingsFormat = "bada-settings/2";

Problem Error(const std::string& file, const std::string& path, const char* code, std::string message) {
    return {file, path, code, std::move(message), "error"};
}

Problem Warning(const std::string& file, const std::string& path, const char* code, std::string message) {
    return {file, path, code, std::move(message), "warning"};
}

// The problems of a session event's reply (§6.6), with the file they are about. `prefix` goes
// before each path: the engine names paths inside its own part of the settings file.
void TakeProblems(const json::Value& reply, const std::string& file, const std::string& prefix,
                  std::vector<Problem>* out) {
    bool any = false;
    if (const json::Value* list = reply.Get("problems"); list && list->IsArray()) {
        for (const json::Value& p : list->array) {
            Problem problem;
            problem.file = file;
            const std::string path = p.GetString("path");
            problem.path = prefix.empty() ? path : path.empty() ? prefix : prefix + "." + path;
            problem.code = p.GetString("code");
            problem.message = p.GetString("message");
            problem.level = p.GetString("level", "error");
            out->push_back(std::move(problem));
            any = true;
        }
    }
    // A refusal with no list still says why.
    if (!any && !reply.GetBool("ok", false)) {
        const json::Value* e = reply.Get("error");
        out->push_back(Error(file, prefix, e ? e->GetString("code", "invalid").c_str() : "invalid",
                             e ? e->GetString("message") : std::string("the engine refused it")));
    }
}

json::Value Event(const char* type) {
    json::Value e = json::Object();
    e.Set("type", json::Str(type));
    return e;
}

// Sends a session event and reads the reply. A reply that isn't JSON reads as a refusal.
json::Value Send(EngineSession& session, const json::Value& event) {
    auto reply = json::Parse(session.Send(json::Write(event)));
    if (reply && reply->IsObject()) return std::move(*reply);
    json::Value refused = json::Object();
    refused.Set("ok", json::Bool(false));
    return refused;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const std::string x = suffix;
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

// A file's name as UTF-8, whatever the system's code page.
std::string Utf8Name(const fs::path& path) {
    const std::u8string name = path.filename().u8string();
    return std::string(name.begin(), name.end());
}

fs::path FromUtf8(const std::string& name) { return fs::path(std::u8string(name.begin(), name.end())); }

// A file read as JSON. nullopt when it can't be read or isn't a JSON object.
std::optional<json::Value> ReadObject(const fs::path& path) {
    std::string text;
    if (!ReadFile(path, &text)) return std::nullopt;
    auto value = json::Parse(text);
    if (!value || !value->IsObject()) return std::nullopt;
    return value;
}

constexpr const char* kNotJson = "not a JSON object (the file is UTF-8 text)";

// ASCII lower case: program file names are compared without case, as Windows does.
std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

}  // namespace

bool HarnessSettings::ShowsMenu(const std::string& exe) const {
    const auto found = apps.find(Lower(exe));
    return found == apps.end() || found->second.menu;
}

bool ReadFile(const fs::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) return false;
    *out = text.str();
    // Windows' editors put a byte order mark before UTF-8 text.
    if (out->size() >= 3 && out->compare(0, 3, "\xEF\xBB\xBF") == 0) out->erase(0, 3);
    return true;
}

fs::path SettingsFolder::DefaultDir() {
#ifdef _WIN32
    // The wide environment: a user's folder may have characters the code page lacks.
    if (const wchar_t* dir = _wgetenv(L"BADA_CONFIG_DIR"); dir && *dir) return fs::path(dir);
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) return fs::path(appdata) / L"bada";
    return fs::path();
#else
    // Only for the core's tests on another system.
    if (const char* dir = std::getenv("BADA_CONFIG_DIR"); dir && *dir) return dir;
    const char* home = std::getenv("HOME");
    return fs::path(home && *home ? home : "/") / ".config" / "bada";
#endif
}

SettingsFolder::SettingsFolder(fs::path dir, Preset detected) : dir_(std::move(dir)), detected_(detected) {}

SettingsFolder::Stamp SettingsFolder::StampOf(const fs::path& path) {
    Stamp s;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec) || ec) return s;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return s;
    const auto size = fs::file_size(path, ec);
    if (ec) return s;
    s.exists = true;
    s.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
    s.size = static_cast<std::uint64_t>(size);
    return s;
}

std::vector<Problem> SettingsFolder::problems() const {
    std::vector<Problem> all = settings_problems_;
    for (const auto& [name, file] : layouts_) all.insert(all.end(), file.problems.begin(), file.problems.end());
    all.insert(all.end(), dictionary_problems_.begin(), dictionary_problems_.end());
    return all;
}

bool SettingsFolder::Reload(EngineSession& session) {
    // Custom layouts first: the settings may name them. Then the settings, then the dictionary.
    const bool layouts = ReloadLayouts(session);
    const bool settings = ReloadSettings(session, layouts);
    const bool dictionary = ReloadDictionary(session);
    read_ = true;
    return layouts || settings || dictionary;
}

bool SettingsFolder::ReloadLayouts(EngineSession& session) {
    bool changed = false;
    std::vector<std::string> names;
    std::error_code ec;
    if (!dir_.empty()) {
        for (fs::directory_iterator it(dir_ / kLayoutsDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = Utf8Name(it->path());
            if (EndsWith(name, ".json") && name[0] != '.') names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());

    auto remove = [&](const std::string& id, const std::string& but) {
        if (id.empty()) return;
        // Another file may give the same layout: then it stays.
        for (const auto& [name, file] : layouts_) {
            if (name != but && file.id == id) return;
        }
        json::Value e = Event("layout");
        e.Set("id", json::Str(id)).Set("value", json::Null());
        Send(session, e);
    };

    for (const std::string& name : names) {
        const fs::path path = dir_ / kLayoutsDir / FromUtf8(name);
        const Stamp stamp = StampOf(path);
        auto found = layouts_.find(name);
        if (found != layouts_.end() && found->second.stamp == stamp) continue;
        changed = true;
        LayoutFile& file = layouts_[name];
        file.stamp = stamp;
        file.problems.clear();
        const std::string shown = std::string(kLayoutsDir) + "/" + name;
        auto value = ReadObject(path);
        if (!value) {
            // What it loaded as before, if anything, stays loaded.
            file.problems.push_back(Error(shown, "", "bad-json", kNotJson));
            continue;
        }
        json::Value e = Event("layout");
        e.Set("value", *value);
        const json::Value reply = Send(session, e);
        TakeProblems(reply, shown, "", &file.problems);
        if (!reply.GetBool("ok", false)) continue;
        const std::string id = value->GetString("id");
        if (file.id != id) remove(file.id, name);
        file.id = id;
    }
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        if (std::binary_search(names.begin(), names.end(), it->first)) {
            ++it;
            continue;
        }
        changed = true;
        remove(it->second.id, it->first);
        it = layouts_.erase(it);
    }
    return changed;
}

bool SettingsFolder::ReloadSettings(EngineSession& session, bool layouts_changed) {
    const fs::path path = dir_.empty() ? fs::path() : dir_ / kSettingsFile;
    const Stamp stamp = StampOf(path);
    // Read again when the file changed, and when the layouts it may name did: the engine checks
    // the settings against the layouts there are only when it is sent them.
    if (read_ && stamp == settings_stamp_ && !layouts_changed) return false;
    settings_stamp_ = stamp;
    std::vector<Problem> problems;
    // Nothing of a file that can't be used is taken: what was in effect stays, with the problems.
    // The very first read has nothing in effect yet: then the keyboard's preset is.
    auto refuse = [&]() {
        if (!read_) loaded_.table = KeyTable::Default(detected_);
        settings_problems_ = std::move(problems);
        return true;
    };

    json::Value file = json::Object();
    if (stamp.exists) {
        auto parsed = ReadObject(path);
        if (!parsed) {
            problems.push_back(Error(kSettingsFile, "", "bad-json", kNotJson));
            return refuse();
        }
        file = std::move(*parsed);
        if (file.GetString("format") != kSettingsFormat) {
            problems.push_back(
                Error(kSettingsFile, "format", "bad-format", std::string("the format is ") + kSettingsFormat));
            return refuse();
        }
    }
    // A file that isn't there is a new install: no muhenkan layout, no profile, the default keys.
    bool ok = true;
    for (const auto& [name, value] : file.object) {
        (void)value;
        if (name != "format" && name != "engine" && name != "keys" && name != "harness") {
            problems.push_back(Warning(kSettingsFile, name, "unknown-field", "the settings have no part named " + name));
        }
    }

    // The harness's own part.
    HarnessSettings harness;
    if (const json::Value* all = file.Get("harness")) {
        if (!all->IsObject()) {
            problems.push_back(Error(kSettingsFile, "harness", "bad-value", "harness is an object, by harness id"));
            ok = false;
        } else if (const json::Value* mine = all->Get(kHarnessId)) {
            const std::string at = std::string("harness.") + kHarnessId;
            if (!mine->IsObject()) {
                problems.push_back(Error(kSettingsFile, at, "bad-value", "a harness's settings are an object"));
                ok = false;
            } else {
                for (const auto& [name, value] : mine->object) {
                    if (name == "keyboard") {
                        const std::string k = value.IsString() ? value.string : std::string();
                        if (k == "auto") harness.keyboard = Keyboard::kAuto;
                        else if (k == "jis-pc") harness.keyboard = Keyboard::kJisPc;
                        else if (k == "jis-mac") harness.keyboard = Keyboard::kJisMac;
                        else if (k == "none") harness.keyboard = Keyboard::kNone;
                        else {
                            problems.push_back(Error(kSettingsFile, at + ".keyboard", "bad-value",
                                                     "keyboard is auto, jis-pc, jis-mac or none"));
                            ok = false;
                        }
                    } else if (name == "publish_state" || name == "log") {
                        if (value.IsBool()) {
                            (name == "log" ? harness.log : harness.publish_state) = value.boolean;
                        } else {
                            problems.push_back(Error(kSettingsFile, at + "." + name, "bad-value", name + " is true or false"));
                            ok = false;
                        }
                    } else if (name == "apps") {
                        if (!value.IsObject()) {
                            problems.push_back(Error(kSettingsFile, at + ".apps", "bad-value",
                                                     "apps is an object, by the program's file name"));
                            ok = false;
                            continue;
                        }
                        for (const auto& [app, entry] : value.object) {
                            const std::string app_at = at + ".apps." + app;
                            if (!entry.IsObject()) {
                                problems.push_back(Error(kSettingsFile, app_at, "bad-value", "an app's settings are an object"));
                                ok = false;
                                continue;
                            }
                            HarnessSettings::App one;
                            for (const auto& [field, v] : entry.object) {
                                if (field == "menu") {
                                    if (v.IsBool()) {
                                        one.menu = v.boolean;
                                    } else {
                                        problems.push_back(Error(kSettingsFile, app_at + ".menu", "bad-value", "menu is true or false"));
                                        ok = false;
                                    }
                                } else {
                                    problems.push_back(Warning(kSettingsFile, app_at + "." + field, "unknown-field",
                                                               "an app has no setting named " + field));
                                }
                            }
                            harness.apps[Lower(app)] = one;
                        }
                    } else {
                        problems.push_back(Warning(kSettingsFile, at + "." + name, "unknown-field",
                                                   "this harness has no setting named " + name));
                    }
                }
            }
        }
    }

    // The key table. With no bindings in the file, the keyboard's preset (§9.5).
    const Preset preset = harness.keyboard == Keyboard::kJisPc    ? Preset::kJisPc
                          : harness.keyboard == Keyboard::kJisMac ? Preset::kJisMac
                          : harness.keyboard == Keyboard::kNone   ? Preset::kNoLanguageKeys
                                                                  : detected_;
    std::optional<KeyTable> table = KeyTable::Parse(file.Get("keys"), preset, &problems);
    if (!table) ok = false;

    const json::Value* engine = file.Get("engine");
    if (engine && !engine->IsObject()) {
        problems.push_back(Error(kSettingsFile, "engine", "bad-value", "engine is an object"));
        ok = false;
    }
    if (!ok) return refuse();

    // The engine's part goes to the engine, which checks it. Refused, nothing of the file is taken.
    json::Value e = Event("settings");
    e.Set("set", engine ? *engine : json::Object());
    const json::Value reply = Send(session, e);
    TakeProblems(reply, kSettingsFile, "engine", &problems);
    if (!reply.GetBool("ok", false)) return refuse();

    Loaded loaded;
    loaded.table = std::move(*table);
    loaded.harness = harness;
    if (const json::Value* in_effect = reply.Get("settings")) loaded.engine = *in_effect;
    if (const json::Value* muhenkan = loaded.engine.Get("muhenkan")) loaded.muhenkan_layout = muhenkan->GetString("layout");
    if (const json::Value* henkan = loaded.engine.Get("henkan")) {
        if (const json::Value* list = henkan->Get("profiles"); list && list->IsArray()) {
            for (const json::Value& p : list->array) {
                Profile profile;
                profile.id = p.GetString("id");
                profile.name = p.GetString("name", profile.id);
                if (const json::Value* layouts = p.Get("layouts"); layouts && layouts->IsArray()) {
                    for (const json::Value& l : layouts->array) {
                        if (l.IsString()) profile.layouts.push_back(l.string);
                    }
                }
                if (!profile.id.empty()) loaded.profiles.push_back(std::move(profile));
            }
        }
    }

    // A profile the table names that isn't there is a warning: the file is taken, and that action
    // doesn't apply (§9.2).
    auto known = [&](const std::string& id) {
        return std::any_of(loaded.profiles.begin(), loaded.profiles.end(), [&](const Profile& p) { return p.id == id; });
    };
    auto unknown = [&](const std::string& at, const std::string& id) {
        problems.push_back(Warning(kSettingsFile, at, "unknown-profile", "no profile named " + id));
    };
    for (const Binding& b : loaded.table.bindings) {
        const std::string at = "keys.bindings[" + std::to_string(b.index) + "]";
        if (b.when == Binding::When::kProfile && !known(b.when_profile)) unknown(at + ".when", b.when_profile);
        for (size_t j = 0; j < b.actions.size(); ++j) {
            const std::string& a = b.actions[j];
            if (a.rfind("profile:", 0) == 0 && !known(a.substr(8))) unknown(at + ".do[" + std::to_string(j) + "]", a.substr(8));
        }
    }
    for (size_t i = 0; i < loaded.table.cycle.size(); ++i) {
        if (!known(loaded.table.cycle[i])) unknown("keys.cycle[" + std::to_string(i) + "]", loaded.table.cycle[i]);
    }
    if (loaded.table.henkan_enters != "last" && !known(loaded.table.henkan_enters)) {
        unknown("keys.henkan_enters", loaded.table.henkan_enters);
    }

    loaded_ = std::move(loaded);
    settings_problems_ = std::move(problems);
    return true;
}

bool SettingsFolder::ReloadDictionary(EngineSession& session) {
    const fs::path path = dir_.empty() ? fs::path() : dir_ / kDictionaryFile;
    const Stamp stamp = StampOf(path);
    if (read_ && stamp == dictionary_stamp_) return false;
    dictionary_stamp_ = stamp;
    const bool had_problems = !dictionary_problems_.empty();
    dictionary_problems_.clear();
    json::Value e = Event("dictionary");
    if (!stamp.exists) {
        if (!dictionary_loaded_) return had_problems;
        // The file is gone: so are its words.
        e.Set("value", json::Null());
        Send(session, e);
        dictionary_loaded_ = false;
        return true;
    }
    auto value = ReadObject(path);
    if (!value) {
        dictionary_problems_.push_back(Error(kDictionaryFile, "", "bad-json", kNotJson));
        return true;
    }
    e.Set("value", std::move(*value));
    const json::Value reply = Send(session, e);
    TakeProblems(reply, kDictionaryFile, "", &dictionary_problems_);
    if (reply.GetBool("ok", false)) dictionary_loaded_ = true;
    return true;
}

}  // namespace bada
