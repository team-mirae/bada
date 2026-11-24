#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

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

}  // namespace

bool ReadFile(const fs::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) return false;
    *out = text.str();
    return true;
}

fs::path SettingsFolder::DefaultDir() {
    if (const char* dir = std::getenv("BADA_CONFIG_DIR"); dir && *dir == '/') return dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg == '/') return fs::path(xdg) / "bada";
    const char* home = std::getenv("HOME");
    return fs::path(home && *home ? home : "/") / ".config" / "bada";
}

SettingsFolder::SettingsFolder(fs::path dir) : dir_(std::move(dir)) {
    watch_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    Watch();
}

SettingsFolder::~SettingsFolder() {
    if (watch_ >= 0) ::close(watch_);
}

SettingsFolder::Stamp SettingsFolder::StampOf(const fs::path& path) {
    Stamp s;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return s;
    s.exists = true;
    s.mtime_s = st.st_mtim.tv_sec;
    s.mtime_ns = st.st_mtim.tv_nsec;
    s.size = static_cast<std::uint64_t>(st.st_size);
    s.inode = static_cast<std::uint64_t>(st.st_ino);
    return s;
}

void SettingsFolder::Watch() {
    if (watch_ < 0) return;
    // Adding a watch that is already there answers the same descriptor, so this is repeated after
    // every reload: a folder that didn't exist is picked up once it does.
    const uint32_t files = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF |
                           IN_MOVE_SELF;
    wd_dir_ = ::inotify_add_watch(watch_, dir_.c_str(), files);
    wd_layouts_ = ::inotify_add_watch(watch_, (dir_ / kLayoutsDir).c_str(), files);
    // The parent, only to see the folder itself appear or go.
    wd_parent_ = ::inotify_add_watch(watch_, dir_.parent_path().c_str(),
                                     IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
}

bool SettingsFolder::Changed() {
    if (watch_ < 0) return false;
    bool ours = false;
    alignas(inotify_event) char buffer[4096];
    while (true) {
        const ssize_t n = ::read(watch_, buffer, sizeof buffer);
        if (n <= 0) break;
        for (ssize_t at = 0; at < n;) {
            const auto* e = reinterpret_cast<const inotify_event*>(buffer + at);
            const std::string name = e->len ? e->name : "";
            if (e->mask & IN_Q_OVERFLOW) {
                ours = true;
            } else if (e->wd == wd_parent_) {
                ours |= name == dir_.filename().string();
            } else if (e->wd == wd_layouts_) {
                ours |= name.empty() || EndsWith(name, ".json");
            } else {
                ours |= name.empty() || name == kSettingsFile || name == kDictionaryFile || name == kLayoutsDir;
            }
            at += static_cast<ssize_t>(sizeof(inotify_event) + e->len);
        }
    }
    return ours;
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
    Watch();
    return layouts || settings || dictionary;
}

bool SettingsFolder::ReloadLayouts(EngineSession& session) {
    bool changed = false;
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir_ / kLayoutsDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (EndsWith(name, ".json") && name[0] != '.') names.push_back(name);
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
        const fs::path path = dir_ / kLayoutsDir / name;
        const Stamp stamp = StampOf(path);
        auto found = layouts_.find(name);
        if (found != layouts_.end() && found->second.stamp == stamp) continue;
        changed = true;
        LayoutFile& file = layouts_[name];
        file.stamp = stamp;
        file.problems.clear();
        const std::string shown = std::string(kLayoutsDir) + "/" + name;
        std::string text;
        auto value = ReadFile(path, &text) ? json::Parse(text) : std::nullopt;
        if (!value || !value->IsObject()) {
            // What it loaded as before, if anything, stays loaded.
            file.problems.push_back(Error(shown, "", "bad-json", "not a JSON object"));
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
    const fs::path path = dir_ / kSettingsFile;
    const Stamp stamp = StampOf(path);
    // Read again when the file changed, and when the layouts it may name did: the engine checks
    // the settings against the layouts there are only when it is sent them.
    if (read_ && stamp == settings_stamp_ && !layouts_changed) return false;
    settings_stamp_ = stamp;
    std::vector<Problem> problems;
    // Nothing of a file that can't be used is taken: what was in effect stays, with the problems.
    auto refuse = [&]() {
        settings_problems_ = std::move(problems);
        return true;
    };

    json::Value file = json::Object();
    if (stamp.exists) {
        std::string text;
        auto parsed = ReadFile(path, &text) ? json::Parse(text) : std::nullopt;
        if (!parsed || !parsed->IsObject()) {
            problems.push_back(Error(kSettingsFile, "", "bad-json", "not a JSON object"));
            return refuse();
        }
        file = std::move(*parsed);
        if (file.GetString("format") != kSettingsFormat) {
            problems.push_back(Error(kSettingsFile, "format", "bad-format",
                                     std::string("the format is ") + kSettingsFormat));
            return refuse();
        }
    }
    // A file that isn't there is a new install: no muhenkan layout, no profile, the default keys.
    bool ok = true;
    for (const auto& [name, value] : file.object) {
        (void)value;
        if (name != "format" && name != "engine" && name != "keys" && name != "harness") {
            problems.push_back({kSettingsFile, name, "unknown-field", "the settings have no part named " + name, "warning"});
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
                        // `auto`: Linux can't tell which keyboard it has, so a PC one.
                        if (k == "jis-pc" || k == "auto") harness.keyboard = Preset::kJisPc;
                        else if (k == "jis-mac") harness.keyboard = Preset::kJisMac;
                        else if (k == "none") harness.keyboard = Preset::kNoLanguageKeys;
                        else {
                            problems.push_back(Error(kSettingsFile, at + ".keyboard", "bad-value",
                                                     "keyboard is jis-pc, jis-mac or none"));
                            ok = false;
                        }
                    } else if (name == "publish_state") {
                        if (value.IsBool()) harness.publish_state = value.boolean;
                        else {
                            problems.push_back(Error(kSettingsFile, at + ".publish_state", "bad-value",
                                                     "publish_state is true or false"));
                            ok = false;
                        }
                    } else {
                        problems.push_back({kSettingsFile, at + "." + name, "unknown-field",
                                            "this harness has no setting named " + name, "warning"});
                    }
                }
            }
        }
    }

    // The key table.
    std::optional<KeyTable> table = KeyTable::Parse(file.Get("keys"), harness.keyboard, &problems);
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

    // What the table names that isn't there is taken, and noted: it doesn't apply (§9.2).
    auto known = [&](const std::string& id) {
        return std::any_of(loaded.profiles.begin(), loaded.profiles.end(), [&](const Profile& p) { return p.id == id; });
    };
    auto unknown = [&](const std::string& path, const std::string& id) {
        problems.push_back({kSettingsFile, path, "unknown-profile", "no profile named " + id, "warning"});
    };
    for (const Binding& b : loaded.table.bindings) {
        const std::string at = "keys.bindings[" + std::to_string(b.index) + "]";
        if (b.when == Binding::When::kProfile && !known(b.when_profile)) unknown(at + ".when", b.when_profile);
        for (size_t j = 0; j < b.actions.size(); ++j) {
            const std::string& a = b.actions[j];
            if (a.rfind("profile:", 0) == 0 && !known(a.substr(8))) {
                unknown(at + ".do[" + std::to_string(j) + "]", a.substr(8));
            }
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
    const fs::path path = dir_ / kDictionaryFile;
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
    std::string text;
    auto value = ReadFile(path, &text) ? json::Parse(text) : std::nullopt;
    if (!value || !value->IsObject()) {
        dictionary_problems_.push_back(Error(kDictionaryFile, "", "bad-json", "not a JSON object"));
        return true;
    }
    e.Set("value", std::move(*value));
    const json::Value reply = Send(session, e);
    TakeProblems(reply, kDictionaryFile, "", &dictionary_problems_);
    if (reply.GetBool("ok", false)) dictionary_loaded_ = true;
    return true;
}

}  // namespace bada
