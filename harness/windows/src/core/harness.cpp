#include "harness.h"

namespace bada {
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kPollEvery = 250;  // ms: how often a key may look at the settings folder

std::string Utf8Path(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

std::string WriteShared(const Shared& shared) {
    json::Value v = json::Object();
    v.Set("mode", json::Str(ModeName(shared.mode)));
    v.Set("profile", shared.profile.empty() ? json::Null() : json::Str(shared.profile));
    v.Set("previous", shared.previous.empty() ? json::Null() : json::Str(shared.previous));
    return json::Write(v);
}

std::optional<Shared> ReadShared(const std::string& text) {
    const auto v = json::Parse(text);
    if (!v || !v->IsObject()) return std::nullopt;
    const std::string mode = v->GetString("mode");
    if (mode != "henkan" && mode != "muhenkan") return std::nullopt;
    Shared shared;
    shared.mode = mode == "henkan" ? Mode::kHenkan : Mode::kMuhenkan;
    shared.profile = v->GetString("profile");
    shared.previous = v->GetString("previous");
    return shared;
}

// --- Session

std::unique_ptr<Session> Session::Create(Options options, std::string* error) {
    fs::path dir = options.config_dir.empty() ? SettingsFolder::DefaultDir() : options.config_dir;
    std::unique_ptr<Session> s(new Session(std::move(options), std::move(dir)));
    s->data_ = EngineData::Load(error, s->options_.data_dir);
    if (!s->data_) return nullptr;
    {
        std::string names;
        for (const std::string& name : s->data_->names()) names += (names.empty() ? "" : " ") + name;
        s->Log("data: " + (names.empty() ? std::string("none") : names));
    }
    s->engine_ = EngineSession::Create(s->data_.get(), InitJson(s->options_.version), error);
    if (!s->engine_) return nullptr;
    s->folder_.Reload(*s->engine_);
    for (const Problem& p : s->folder_.problems()) {
        s->Log(p.file + ": " + p.level + " " + p.code + (p.path.empty() ? "" : " at " + p.path) + ": " + p.message);
    }
    return s;
}

void Session::Log(const std::string& line) const {
    if (options_.log) options_.log(line);
}

const Profile* Session::Find(const std::string& id) const {
    for (const Profile& p : profiles()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::string Session::Label() const {
    if (state_.mode == Mode::kMuhenkan) return "A";
    const Profile* p = Find(state_.profile);
    const std::string& name = p && !p->name.empty() ? p->name : state_.profile;
    return name.substr(0, ByteOffset(name, 1));
}

std::string Session::ModeText() const {
    if (state_.mode == Mode::kMuhenkan) return "muhenkan";
    const Profile* p = Find(state_.profile);
    return p && !p->name.empty() ? p->name : state_.profile;
}

bool Session::Reload() {
    if (!folder_.Reload(*engine_)) return false;
    Validate();
    ++generation_;
    for (const Problem& p : folder_.problems()) {
        Log(p.file + ": " + p.level + " " + p.code + (p.path.empty() ? "" : " at " + p.path) + ": " + p.message);
    }
    if (!harness().publish_state && options_.sink) options_.sink->Clear();
    return true;
}

bool Session::Poll(std::uint64_t now_ms) {
    if (polled_ && now_ms - polled_ms_ < kPollEvery) return false;
    polled_ = true;
    polled_ms_ = now_ms;
    return Reload();
}

void Session::Select(Mode mode, const std::string& profile) {
    state_.Selected(mode, profile);
    notice_.clear();
    ++generation_;
    ++switches_;
}

bool Session::Adopt(const Shared& shared) {
    if (shared == this->shared()) return false;
    // The process that switched may have read a newer settings file than this one has.
    Reload();
    if (shared.mode == Mode::kHenkan && !Find(shared.profile)) return false;
    // An unknown id is never taken, and never replaced by another profile (§8.1).
    const std::string profile = Find(shared.profile) ? shared.profile : state_.profile;
    const std::string previous = Find(shared.previous) ? shared.previous : std::string();
    if (state_.mode == shared.mode && state_.profile == profile && state_.previous == previous) return false;
    state_.mode = shared.mode;
    state_.profile = profile;
    state_.previous = previous;
    notice_.clear();
    ++generation_;
    return true;
}

void Session::Validate() {
    // An unknown id is an error, never a fallback to another profile (§8.1): a profile that was
    // removed while selected leaves henkan.
    if (!state_.profile.empty() && !Find(state_.profile)) {
        state_.profile.clear();
        state_.mode = Mode::kMuhenkan;
    }
    if (!state_.previous.empty() && !Find(state_.previous)) state_.previous.clear();
    if (!profiles().empty()) notice_.clear();
}

void Session::Onboard() {
    // §8.3: with no profile, a key that would enter henkan doesn't. The shared settings page will
    // open here; until it exists, the user is told where to write a profile.
    notice_ = "bada: no henkan profile yet. Add one in " + Utf8Path(folder_.dir() / "settings.json") +
              " (the README says how).";
    Log("a key asked for henkan, but the settings have no henkan profile (README, \"First run\")");
}

std::optional<bool> Session::TakeLockWanted() {
    std::optional<bool> wanted;
    wanted.swap(lock_wanted_);
    return wanted;
}

// --- Context

Context::Context(Session& session) : session_(session), engine_(*session.engine_) {}

Reply Context::Send(const std::string& event_json, HostField& field) {
    const Reply first = ParseReply(engine_.Send(event_json));
    const Reply shown =
        Apply(first, field, [this](const std::string& answer) { return ParseReply(engine_.Send(answer)); });
    if (shown.ok) last_ = shown;
    else if (shown.error_code == "panic") last_ = Reply{};  // the engine reset itself: nothing shows
    return first;
}

void Context::Sync(HostField& field, const ContextInfo& info) {
    const Mode mode = info.secure ? Mode::kMuhenkan : session_.mode();
    if (selected_ && generation_ == session_.generation() && mode == engine_mode_) return;
    SendSelect(field, info);
}

void Context::SendSelect(HostField& field, const ContextInfo& info) {
    generation_ = session_.generation();
    // A password field stays in muhenkan, whatever the session's mode: nothing composes there.
    Mode mode = info.secure ? Mode::kMuhenkan : session_.mode();
    std::string profile = session_.Find(session_.profile()) ? session_.profile() : std::string();
    Reply r = Send(SelectEventJson(mode, profile), field);
    if (!r.ok && mode == Mode::kHenkan) {
        // The engine doesn't have the profile after all: muhenkan, for every field of this process.
        session_.Log("select answered " + r.error_code + ": back to muhenkan");
        session_.state_.mode = Mode::kMuhenkan;
        session_.state_.profile.clear();
        ++session_.generation_;
        generation_ = session_.generation();
        mode = Mode::kMuhenkan;
        profile.clear();
        Send(SelectEventJson(mode, profile), field);
    }
    selected_ = true;
    engine_mode_ = mode;
}

bool Context::LockOn() const {
    // transient: Caps Lock in muhenkan only (§9.4).
    if (session_.table().caps_lock == CapsSetting::kTransient && session_.mode() == Mode::kHenkan) return false;
    return session_.lock_;
}

bool Context::KeyDown(const RawKey& key, HostField& field, const ContextInfo& info) {
    const std::string code = CodeOf(key);
    if (code.empty()) return false;  // no key press at all
    Sync(field, info);
    if (notice_shown_) {
        field.Notice("");
        notice_shown_ = false;
    }
    const KeyTable& table = session_.table();
    // The lock is Windows', as each key reports it. Not the Caps Lock key's own press: Windows
    // has flipped the lock by the time the key arrives, and the table decides from what it was.
    if (code != "CapsLock") {
        session_.lock_ = key.caps;
        session_.lock_decided_ = false;
        // transient: if another program turned the lock on in henkan, the harness turns it off.
        if (key.caps && table.caps_lock == CapsSetting::kTransient && session_.mode() == Mode::kHenkan) {
            session_.lock_ = false;
            session_.lock_wanted_ = false;
        }
    }
    // A binding runs once for each press: the repeats of a held key that has one are swallowed,
    // but for a key that was passed on, whose repeats pass too (§9.1). A language key or the lock
    // with no binding stays Windows', and isn't decided again.
    if (key.repeat && (bound_.count(code) || IsHarnessKey(code))) return eaten_.count(code) > 0;

    // The tracker reads the mode and profile as they are when the key goes down: a key waits
    // only for a pair binding that holds then (§9.3).
    const KeyTracker::Down down = tracker_.KeyDown(table, session_.state_, code, key.mods);
    bool eaten = down.waits, bound = down.waits;
    for (const KeyTracker::Step& step : down.steps) {
        // A key that waited runs first, as its own press.
        const bool e = Run(step, step.late ? waiting_ : key, step.late ? step.press.key : code, field, info);
        if (!step.late) {
            eaten = e;
            bound = bound_now_;
        }
    }
    if (down.waits) waiting_ = key;
    if (eaten) eaten_.insert(code);
    else eaten_.erase(code);
    if (bound) bound_.insert(code);
    else bound_.erase(code);
    Publish(info);
    return eaten;
}

bool Context::KeyUp(const RawKey& key, HostField& field, const ContextInfo& info) {
    const std::string code = CodeOf(key);
    if (code.empty()) return false;
    const bool eaten = eaten_.erase(code) > 0;
    bound_.erase(code);
    // A Caps Lock that was Windows' has flipped the lock by now.
    const bool lock_changed = !session_.lock_decided_ && session_.lock_ != key.caps;
    if (lock_changed) session_.lock_ = key.caps;
    const std::vector<KeyTracker::Step> steps = tracker_.KeyUp(code);
    if (steps.empty()) {
        if (lock_changed) Publish(info);
        return eaten;
    }
    // A key of a bound pair released alone, a pair with both keys up, or a modifier tapped (§9.3).
    Sync(field, info);
    for (const KeyTracker::Step& step : steps) Run(step, waiting_, step.press.key, field, info);
    Publish(info);
    return eaten;
}

bool Context::Run(const KeyTracker::Step& step, const RawKey& key, const std::string& code, HostField& field,
                  const ContextInfo& info) {
    bound_now_ = false;
    const KeyTable& table = session_.table();
    // A modifier tapped alone is a key only where a binding says so.
    if (step.tap && !table.HasBindingOn(step.press.key, Mods{})) return false;

    TableState state = session_.state_;
    state.menu_rows = last_.menu.rows.size();
    state.staged = !last_.staged.empty();
    state.caps_lock = session_.lock_;
    return Do(Decide(table, session_.profiles(), state, step.press), step, key, code, field, info);
}

bool Context::Do(const Decision& d, const KeyTracker::Step& step, const RawKey& key, const std::string& code,
                 HostField& field, const ContextInfo& info) {
    bound_now_ = d.binding >= 0;
    if (step.press.key == "CapsLock" && step.press.key2.empty()) {
        // §9.4. A Caps Lock that is passed on and only flips the lock is Windows' own key: the
        // harness reads the lock back afterwards. Anything else is the harness's decision, which
        // Windows doesn't know of: the lock is set to it.
        const bool before = session_.lock_;
        const bool after = d.caps_lock.value_or(before);
        session_.lock_decided_ = !(d.then == Decision::Then::kPass && after != before);
        if (session_.lock_decided_) {
            session_.lock_ = after;
            session_.lock_wanted_ = after;
        }
    } else if (d.caps_lock) {
        session_.lock_ = *d.caps_lock;
        session_.lock_wanted_ = *d.caps_lock;
    }
    switch (d.then) {
        case Decision::Then::kSelect:
            session_.Select(d.mode, d.profile);
            SendSelect(field, info);
            return true;
        case Decision::Then::kAction:
            Send(ActionEventJson(d.name), field);
            return true;
        case Decision::Then::kOnboarding:
            // The engine is not asked (§8.3).
            session_.Onboard();
            field.Notice(session_.notice());
            notice_shown_ = true;
            return true;
        case Decision::Then::kSwallow:
            return true;
        case Decision::Then::kPass:
            // A key that waited was eaten when it went down: hand it over now.
            if (step.late && !step.tap) field.ForwardKey(key);
            return false;
        case Decision::Then::kType:
            Send(FlushEventJson("harness"), field);
            field.Commit(d.text, false);
            return true;
        case Decision::Then::kKey:
            if (step.late || step.tap) return false;
            return SendKey(code, key, field, info);
    }
    return false;
}

bool Context::SendKey(const std::string& code, const RawKey& key, HostField& field, const ContextInfo& info) {
    Reply r = Send(KeyEventJson(code, key.mods, key.repeat), field);
    if (!r.ok && (r.error_code == "no-profile" || r.error_code == "unknown-profile")) {
        // The field's profile is gone (§8.2): muhenkan, and the key once more.
        session_.Log("a key answered " + r.error_code + ": back to muhenkan");
        session_.state_.mode = Mode::kMuhenkan;
        session_.state_.profile.clear();
        ++session_.generation_;
        SendSelect(field, info);
        r = Send(KeyEventJson(code, key.mods, key.repeat), field);
    }
    // A refused key event is swallowed: nothing changed, and the app doesn't get the key either.
    if (!r.ok) return true;
    if (r.consumed) return true;
    // §6.4. null: the engine has no say, and the app gets the key as Windows has it.
    if (!r.types || r.types->empty()) return false;
    std::string text = *r.types;
    if (LockOn()) text = Capitals(text);
    // A string: the key types that text, whatever layout Windows has. Windows types it when its
    // layout gives the same text; otherwise the harness does, and keeps the key from the app.
    const bool typed_by_os = !key.mods.ctrl && !key.mods.alt && !key.mods.meta && key.os_text == text;
    if (typed_by_os) return false;
    field.Commit(text, false);
    return true;
}

void Context::FocusIn(HostField& field, const ContextInfo& info) {
    session_.Reload();
    Sync(field, info);
    Publish(info);
}

void Context::Flush(HostField& field, const char* cause) {
    tracker_.Clear();
    eaten_.clear();
    bound_.clear();
    if (!selected_) return;
    if (last_.staged.empty() && last_.preedit.text.empty() && last_.menu.rows.empty()) return;
    Send(FlushEventJson(cause), field);
}

void Context::FocusOut(HostField& field) {
    Flush(field, "blur");
    session_.PublishIdle();
}

void Context::Pick(size_t row, HostField& field, const ContextInfo& info) {
    Sync(field, info);
    // `pick` only moves to the row: a harness that commits on a click sends `flush` after it (§5).
    if (Send(PickEventJson(row), field).ok) Send(FlushEventJson("harness"), field);
    Publish(info);
}

void Context::Select(Mode mode, const std::string& profile, HostField& field, const ContextInfo& info) {
    Sync(field, info);
    if (mode == Mode::kHenkan && !session_.Find(profile)) {
        if (session_.profiles().empty()) {
            session_.Onboard();
            field.Notice(session_.notice());
            notice_shown_ = true;
        }
        return;
    }
    session_.Select(mode, mode == Mode::kHenkan ? profile : session_.profile());
    SendSelect(field, info);
    Publish(info);
}

void Context::Toggle(HostField& field, const ContextInfo& info) {
    Sync(field, info);
    // What a key bound to `mode.toggle` does (§9.2), the onboarding included.
    KeyTable table = session_.table();
    Binding toggle;
    toggle.key = "Toggle";
    toggle.actions = {"mode.toggle"};
    table.bindings = {toggle};
    TableState state = session_.state_;
    state.caps_lock = session_.lock_;
    const KeyTracker::Step step{Press{"Toggle", {}, {}}, false, false};
    Do(Decide(table, session_.profiles(), state, step.press), step, RawKey{}, "Toggle", field, info);
    Publish(info);
}

void Context::Forget() {
    tracker_.Clear();
    eaten_.clear();
    bound_.clear();
    // `reset` drops what is staged, and commits nothing (§5).
    if (selected_ && composing()) engine_.Send(ResetEventJson());
    last_ = Reply{};
}

std::string Session::State(const Reply* last, const ContextInfo* info) const {
    const bool active = info != nullptr;
    json::Value v = json::Object();
    v.Set("format", json::Str("bada-compatible-state/2"));
    v.Set("mode", json::Str(ModeName(state_.mode)));
    v.Set("profile", state_.profile.empty() ? json::Null() : json::Str(state_.profile));
    // The last reply's display fields. In a password field nothing typed is published (§6.7).
    const bool show = active && !info->secure && last && last->ok;
    const json::Value* preedit = show ? last->raw.Get("preedit") : nullptr;
    const json::Value* menu = show ? last->raw.Get("menu") : nullptr;
    const json::Value* ask = show ? last->raw.Get("ask") : nullptr;
    v.Set("staged", json::Str(show ? last->staged : std::string()));
    if (preedit && preedit->IsObject()) {
        v.Set("preedit", *preedit);
    } else {
        json::Value none = json::Object();
        none.Set("text", json::Str("")).Set("caret", json::Num(0)).Set("segments", json::Array());
        v.Set("preedit", std::move(none));
    }
    if (menu && menu->IsObject()) {
        v.Set("menu", *menu);
    } else {
        json::Value none = json::Object();
        none.Set("rows", json::Array()).Set("selected", json::Null()).Set("strip", json::Bool(false));
        v.Set("menu", std::move(none));
    }
    v.Set("ask", ask ? *ask : json::Null());
    v.Set("dir", json::Str(show && last->rtl ? "rtl" : "ltr"));

    const KeyTable& keytable = table();
    json::Value h = json::Object();
    h.Set("id", json::Str(kHarnessId));
    h.Set("version", json::Str(options_.version));
    h.Set("app", json::Str(active ? info->app : std::string()));
    h.Set("secure", json::Bool(active && info->secure));
    h.Set("active", json::Bool(active));
    json::Value caps = json::Object();
    caps.Set("on", json::Bool(lock_));
    caps.Set("setting", json::Str(keytable.caps_lock == CapsSetting::kTransient ? "transient" : "respect"));
    caps.Set("rule", json::Str(keytable.caps_rule == CapsRule::kUpperSnake ? "upper_snake" : "allcaps"));
    h.Set("caps_lock", std::move(caps));
    // What the harness can detect now (§9.3). TSF hands the text service the focused app's keys
    // with their modifiers, so chords need no permission.
    json::Value keys = json::Object();
    json::Value available = json::Array();
    for (const std::string& key : DetectedKeys()) available.Push(json::Str(key));
    keys.Set("available", std::move(available)).Set("chords", json::Bool(true)).Set("missing", json::Array());
    h.Set("keys", std::move(keys));
    json::Value list = json::Array();
    for (const Problem& p : problems()) {
        json::Value item = json::Object();
        item.Set("file", json::Str(p.file)).Set("path", json::Str(p.path)).Set("code", json::Str(p.code));
        item.Set("message", json::Str(p.message)).Set("level", json::Str(p.level));
        list.Push(std::move(item));
    }
    h.Set("problems", std::move(list));
    h.Set("notice", json::Str(notice_));
    v.Set("harness", std::move(h));
    return json::Write(v);
}

StateSummary Session::Summary(const Reply* last, const ContextInfo* info) const {
    StateSummary s;
    s.mode = state_.mode;
    s.active = info != nullptr;
    s.secure = info && info->secure;
    s.composing = s.active && !s.secure && last && last->ok && !last->staged.empty();
    s.caps_lock = lock_;
    return s;
}

void Session::PublishIdle() {
    if (!options_.sink || !harness().publish_state) return;
    options_.sink->Publish(State(nullptr, nullptr), Summary(nullptr, nullptr));
}

void Context::Publish(const ContextInfo& info) {
    if (!session_.options_.sink || !session_.harness().publish_state) return;
    session_.options_.sink->Publish(State(info), session_.Summary(&last_, &info));
}

}  // namespace bada
