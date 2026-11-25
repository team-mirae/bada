#include "engine.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <map>

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
    if (const json::Value* data = parsed->Get("data"); data && data->IsArray()) {
        for (const json::Value& item : data->array) {
            if (!item.IsObject() || item.GetString("name").empty()) continue;
            const json::Value* borrow = item.Get("borrow");
            d.data.push_back({item.GetString("name"), borrow && borrow->IsBool() && borrow->boolean});
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

EngineData::Mapping EngineData::Map(const std::filesystem::path& file) {
    Mapping mapping;
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return mapping;
    struct stat st {};
    if (::fstat(fd, &st) == 0 && st.st_size > 0) {
        void* ptr = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr != MAP_FAILED) mapping = {ptr, static_cast<std::size_t>(st.st_size)};
    }
    // The mapping stays after the file is closed.
    ::close(fd);
    return mapping;
}

void EngineData::Unmap(const Mapping& mapping) {
    if (mapping.ptr) ::munmap(const_cast<void*>(mapping.ptr), mapping.len);
}

std::unique_ptr<EngineData> EngineData::Load(std::string* error, const std::filesystem::path& dir) {
    std::unique_ptr<EngineData> data(new EngineData());
    // The files of the folder, by their names without the extension.
    std::map<std::string, std::filesystem::path> files;
    std::error_code failed;
    if (!dir.empty()) {
        for (std::filesystem::directory_iterator it(dir, failed), end; !failed && it != end; it.increment(failed)) {
            std::error_code ignored;
            if (!it->is_regular_file(ignored)) continue;
            // A buffer's name is ASCII: a file with another name is no buffer's.
            std::string name;
            bool ascii = true;
            for (const auto c : it->path().stem().native()) {
                // A char may be signed, and a wchar_t wider: compared as one unsigned number.
                const unsigned long code = static_cast<unsigned long>(c) & 0xFFFFFFFFul;
                ascii &= code > 0 && code < 128;
                name.push_back(static_cast<char>(c));
            }
            if (ascii && !name.empty()) files.emplace(std::move(name), it->path());
        }
    }
    Describe description;
    ReadDescribe(&description, nullptr);
    std::vector<Mapping> mapped;
    std::vector<bool> borrowed;
    for (const DataBuffer& buffer : description.data) {
        const auto file = files.find(buffer.name);
        if (file == files.end()) continue;
        const Mapping mapping = Map(file->second);
        if (!mapping.ptr) continue;
        mapped.push_back(mapping);
        borrowed.push_back(buffer.borrow);
        data->names_.push_back(buffer.name);
    }
    std::vector<bada_buf> bufs;
    for (std::size_t i = 0; i < mapped.size(); ++i) {
        bufs.push_back({data->names_[i].c_str(), static_cast<const std::uint8_t*>(mapped[i].ptr), mapped[i].len,
                        borrowed[i] ? BADA_BORROWED : 0u});
    }
    data->data_ = bada_data_new(bufs.empty() ? nullptr : bufs.data(), bufs.size());
    // The engine copied what it keeps of the buffers it wasn't lent.
    for (std::size_t i = 0; i < mapped.size(); ++i) {
        if (borrowed[i] && data->data_) data->kept_.push_back(mapped[i]);
        else Unmap(mapped[i]);
    }
    if (!data->data_) {
        if (error) *error = "bada_data_new answered NULL";
        return nullptr;
    }
    return data;
}

// The data first, then the buffers it may point into.
EngineData::~EngineData() {
    bada_data_free(data_);
    for (const Mapping& mapping : kept_) Unmap(mapping);
}

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
    // segments: the preedit's parts are drawn apart. pick: a click on a row. around: the text
    // around the caret, where the app gives it. Not rtl: neither framework draws right to left.
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

}  // namespace bada
