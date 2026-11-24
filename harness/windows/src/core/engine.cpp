#include "engine.h"

#include "bada.h"

namespace bada {

bool ReadDescribe(Describe* out, std::string* error) {
    const char* text = bada_describe();
    return ParseDescribe(text ? text : "", out, error);
}

bool ParseDescribe(const std::string& text, Describe* out, std::string* error) {
    auto parsed = json::Parse(text);
    if (!parsed || !parsed->IsObject()) {
        if (error) *error = "describe didn't answer a JSON object";
        return false;
    }
    Describe d;
    if (const json::Value* abi = parsed->Get("abi")) {
        d.abi_name = abi->GetString("name");
        d.abi_major = static_cast<int>(abi->GetNumber("major", 0));
        d.abi_minor = static_cast<int>(abi->GetNumber("minor", 0));
    }
    if (const json::Value* engine = parsed->Get("engine")) {
        d.engine_id = engine->GetString("id");
        d.engine_version = engine->GetString("version");
    }
    for (const char* list : {"events", "features"}) {
        const json::Value* v = parsed->Get(list);
        if (!v || !v->IsArray()) continue;
        for (const json::Value& item : v->array) {
            if (item.IsString()) (std::string(list) == "events" ? d.events : d.features).push_back(item.string);
        }
    }
    d.raw = std::move(*parsed);
    const bool fits = d.abi_name == "bada-compatible" && d.abi_major == kAbiMajor;
    if (!fits && error) {
        *error = "the engine speaks " + (d.abi_name.empty() ? std::string("no standard") : d.abi_name) + " " +
                 std::to_string(d.abi_major) + "." + std::to_string(d.abi_minor) + ", and this harness bada-compatible " +
                 std::to_string(kAbiMajor);
    }
    if (out) *out = std::move(d);
    return fits;
}

std::unique_ptr<EngineData> EngineData::Load(std::string* error) {
    std::unique_ptr<EngineData> data(new EngineData());
    // No buffers yet (see the header). Later:
    //   std::vector<bada_buf> bufs = the mapped pack's blobs, each {name, ptr, len, BADA_BORROWED};
    //   data->data_ = bada_data_new(bufs.data(), bufs.size());
    // with the mapping kept open in this object, since borrowed buffers must outlive the data.
    data->data_ = bada_data_new(nullptr, 0);
    if (!data->data_) {
        if (error) *error = "bada_data_new answered NULL";
        return nullptr;
    }
    return data;
}

EngineData::~EngineData() { bada_data_free(data_); }

std::unique_ptr<EngineSession> EngineSession::Create(const EngineData* data, const std::string& init_json,
                                                     std::string* error) {
    if (!ReadDescribe(nullptr, error)) return nullptr;
    std::unique_ptr<EngineSession> s(new EngineSession());
    s->session_ = bada_session_new(data ? data->get() : nullptr, init_json.c_str());
    if (!s->session_) {
        if (error) *error = "bada_session_new answered NULL";
        return nullptr;
    }
    return s;
}

EngineSession::~EngineSession() { bada_session_free(session_); }

std::string EngineSession::Send(const std::string& event_json) {
    const char* reply = bada_send(session_, nullptr, event_json.c_str());
    return reply ? std::string(reply) : std::string();
}

Engine::Engine(EngineSession& session) : session_(session), engine_(bada_new(session.get())) {}

Engine::~Engine() { bada_free(engine_); }

std::string Engine::Send(const std::string& event_json) {
    if (!engine_) return {};
    // The reply is the engine's until the next call: copy it.
    const char* reply = bada_send(session_.get(), engine_, event_json.c_str());
    return reply ? std::string(reply) : std::string();
}

std::string InitJson(const std::string& version) {
    json::Value harness = json::Object();
    harness.Set("id", json::Str(kHarnessId));
    harness.Set("version", json::Str(version));
    // segments: the preedit's parts are drawn apart (display attributes). pick: a click on a row
    // of the menu. around: the text around the caret, where the app gives it. Not rtl: the menu
    // window doesn't draw right to left.
    json::Value features = json::Array();
    for (const char* f : {"segments", "pick", "around"}) features.Push(json::Str(f));
    harness.Set("features", std::move(features));
    json::Value init = json::Object();
    init.Set("harness", std::move(harness));
    return json::Write(init);
}

std::string KeyEventJson(const std::string& code, const Mods& mods, bool repeat) {
    json::Value e = json::Object();
    e.Set("type", json::Str("key")).Set("key", json::Str(code));
    if (mods.shift) e.Set("shift", json::Bool(true));
    if (mods.ctrl) e.Set("ctrl", json::Bool(true));
    if (mods.alt) e.Set("alt", json::Bool(true));
    if (mods.meta) e.Set("meta", json::Bool(true));
    if (repeat) e.Set("repeat", json::Bool(true));
    return json::Write(e);
}

std::string ActionEventJson(const std::string& name) {
    json::Value e = json::Object();
    e.Set("type", json::Str("action")).Set("name", json::Str(name));
    return json::Write(e);
}

std::string SelectEventJson(Mode mode, const std::string& profile) {
    json::Value e = json::Object();
    e.Set("type", json::Str("select")).Set("mode", json::Str(ModeName(mode)));
    e.Set("profile", profile.empty() ? json::Null() : json::Str(profile));
    return json::Write(e);
}

std::string PickEventJson(size_t row) {
    json::Value e = json::Object();
    e.Set("type", json::Str("pick")).Set("row", json::Num(static_cast<double>(row)));
    return json::Write(e);
}

std::string TextEventJson(const std::string& selection, bool around, const std::string& before,
                          const std::string& after) {
    json::Value e = json::Object();
    e.Set("type", json::Str("text")).Set("selection", json::Str(selection));
    if (around) e.Set("before", json::Str(before)).Set("after", json::Str(after));
    return json::Write(e);
}

std::string FlushEventJson(const char* cause) {
    json::Value e = json::Object();
    e.Set("type", json::Str("flush")).Set("cause", json::Str(cause));
    return json::Write(e);
}

std::string ResetEventJson() { return R"({"type":"reset"})"; }

}  // namespace bada
