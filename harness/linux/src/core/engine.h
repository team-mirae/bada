// The engine, through its C ABI (docs/bada-compatible-v2.md §3): bada_describe, the data, one
// session per process and one engine per text field. Everything else is an event: JSON in, a reply
// out. The library is linked, not loaded: this harness is built for bada's own engine.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "json.h"
#include "keys.h"
#include "keytable.h"

struct bada_data;
struct bada_session;
struct bada_engine;

namespace bada {

// The standard this harness speaks.
inline constexpr int kAbiMajor = 2;
// The harness's id (§12).
inline constexpr const char* kHarnessId = "com.teammirae.bada.linux";

// What bada_describe says, as much of it as the harness reads.
struct Describe {
    std::string abi_name;
    int abi_major = 0;
    int abi_minor = 0;
    std::string engine_id;
    std::string engine_version;
    std::vector<std::string> events;
    std::vector<std::string> features;
    json::Value raw;
};
// Reads what a P_describe answered. False and `error` when it isn't a bada-compatible engine of
// our major version: the harness then refuses it (§1).
bool ParseDescribe(const std::string& text, Describe* out, std::string* error);
// The same, of the linked engine's bada_describe.
bool ReadDescribe(Describe* out, std::string* error);

// The engine's data (§11): named buffers, built once and shared by every session.
class EngineData {
public:
    // **No data files are passed yet**: the engine types on every layout without them, and has no
    // kanji conversion or word ranking until they are. When the pack is defined, this is where
    // its blobs are mapped and passed as named buffers (bada_buf: name, ptr, len, BADA_BORROWED).
    // nullptr and `error` when the engine refuses what it was given.
    static std::unique_ptr<EngineData> Load(std::string* error);
    ~EngineData();
    EngineData(const EngineData&) = delete;
    EngineData& operator=(const EngineData&) = delete;
    const bada_data* get() const { return data_; }

private:
    EngineData() = default;
    const bada_data* data_ = nullptr;
};

// The session: the settings, the custom layouts and the user dictionary of one process (§3.1).
class EngineSession {
public:
    // `init_json` is §3.3's object. nullptr and `error` when the engine is of another major
    // version, or refuses the init.
    static std::unique_ptr<EngineSession> Create(const EngineData* data, const std::string& init_json,
                                                 std::string* error);
    ~EngineSession();
    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    // A session event (`settings`, `layout`, `dictionary`): the reply, copied.
    std::string Send(const std::string& event_json);
    bada_session* get() const { return session_; }

private:
    EngineSession() = default;
    bada_session* session_ = nullptr;
};

// One text field.
class Engine {
public:
    explicit Engine(EngineSession& session);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool ok() const { return engine_ != nullptr; }
    // One event in, one reply out: the reply's JSON, copied.
    std::string Send(const std::string& event_json);

private:
    EngineSession& session_;
    bada_engine* engine_ = nullptr;
};

// The init object for this harness (§3.3). `frontend` is "fcitx5" or "ibus".
std::string InitJson(const std::string& version);

// Events (§5), as JSON.
std::string KeyEventJson(const std::string& code, const Mods& mods, bool repeat);
std::string ActionEventJson(const std::string& name);
// `profile` "" is null.
std::string SelectEventJson(Mode mode, const std::string& profile);
std::string PickEventJson(size_t row);
// `around`: also `before` and `after`.
std::string TextEventJson(const std::string& selection, bool around, const std::string& before,
                          const std::string& after);
std::string FlushEventJson(const char* cause);

}  // namespace bada
