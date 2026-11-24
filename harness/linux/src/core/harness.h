// The harness, without a framework (docs/bada-compatible-v2.md): what Fcitx5 and IBus share.
//
// A Session is the process's: the engine's data and session, the settings folder, the key table,
// and the mode and profile, which every text field follows. A Context is one text field: its
// engine, the last reply, and the keys held down. A frontend makes one Session and one Context per
// input context, gives each call its Field, and moves keys in and text out. Nothing else.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "keys.h"
#include "keytable.h"
#include "reply.h"
#include "settings.h"

namespace bada {

// Where the published state goes (§6.7). The frontends give a file and a D-Bus signal
// (src/common/publish.h).
class StateSink {
public:
    virtual ~StateSink() = default;
    virtual void Publish(const std::string& json) = 0;
    virtual void Clear() = 0;  // nothing is published (turned off)
};

// What a frontend knows about the text field in focus.
struct ContextInfo {
    std::string app;      // the program, as the framework names it; "" unknown
    bool secure = false;  // a password field: nothing composes, nothing typed is published
};

// A frontend's text field: reply.h's Field, and what only a real one does.
class HostField : public Field {
public:
    // Hands the app a key the harness held back and then let go: a key of a bound pair whose own
    // binding passes it on (§9.3).
    virtual void ForwardKey(const RawKey& key) { (void)key; }
    // Shows the user a short notice; "" takes it away.
    virtual void Notice(const std::string& text) { (void)text; }
};

class Session {
public:
    struct Options {
        std::filesystem::path config_dir;  // empty: SettingsFolder::DefaultDir()
        std::string frontend;              // "fcitx5" or "ibus"
        std::string version;               // the harness's own
        StateSink* sink = nullptr;
        // One line for the log. Never typed text.
        std::function<void(const std::string&)> log;
    };
    // Builds the data and the session and reads the settings folder. nullptr and `error` when the
    // engine can't be used: another major version, or it refuses to start.
    static std::unique_ptr<Session> Create(Options options, std::string* error);

    Mode mode() const { return state_.mode; }
    // The profile last selected; "" before the first.
    const std::string& profile() const { return state_.profile; }
    const std::vector<Profile>& profiles() const { return folder_.loaded().profiles; }
    const KeyTable& table() const { return folder_.loaded().table; }
    const HarnessSettings& harness() const { return folder_.loaded().harness; }
    const SettingsFolder& folder() const { return folder_; }
    std::vector<Problem> problems() const { return folder_.problems(); }
    // Set when a key asked for henkan and there is no profile (§8.3); "" otherwise.
    const std::string& notice() const { return notice_; }
    // Goes up with every change of mode or profile, and of the settings.
    std::uint64_t generation() const { return generation_; }

    // The indicator's text: "A" in muhenkan, the first character of the profile's name in henkan.
    std::string Label() const;
    // The profile's name in henkan, "muhenkan" otherwise.
    std::string ModeText() const;

    // Reads the settings folder again if it changed. True: something was read.
    bool Reload();
    // The published state (§6.7): the display fields of `last` (nullptr: nothing shows) and the
    // harness's own. `info` nullptr: no text field has focus.
    std::string State(const Reply* last, const ContextInfo* info) const;
    // Publishes the state with no text field in focus: at the start, and when the settings change.
    void PublishIdle();
    // For the frontend's event loop: see SettingsFolder.
    int watch_fd() const { return folder_.watch_fd(); }
    // The watch fired: drain it and reload. True: something was read.
    bool WatchFired();

private:
    friend class Context;
    Session(Options options, std::filesystem::path dir) : options_(std::move(options)), folder_(std::move(dir)) {}
    void Log(const std::string& line) const;
    // Mode and profile changed by a key, the menu or a settings file.
    void Select(Mode mode, const std::string& profile);
    // After a reload: a profile that is gone can't stay selected.
    void Validate();
    void Onboard();
    const Profile* Find(const std::string& id) const;

    Options options_;
    std::unique_ptr<EngineData> data_;
    std::unique_ptr<EngineSession> engine_;
    SettingsFolder folder_;
    TableState state_;       // mode, profile and the one before; the rest is each field's
    bool lock_ = false;      // the lock, as the harness has it (§9.4)
    std::string notice_;
    std::uint64_t generation_ = 1;
};

class Context {
public:
    explicit Context(Session& session);

    bool ok() const { return engine_.ok(); }
    // A key press. True: eaten, the app doesn't get it.
    bool KeyDown(const RawKey& key, HostField& field, const ContextInfo& info);
    // A key release. True: eaten, as its press was.
    bool KeyUp(const RawKey& key, HostField& field, const ContextInfo& info);
    // Focus came here: the settings and the session's mode are taken up, and the state published.
    void FocusIn(HostField& field, const ContextInfo& info);
    // What is staged is committed, before the field changes. `cause`: "blur" (focus left), "click"
    // (the app reset the field: a click in the text), "edit", or "harness".
    void Flush(HostField& field, const char* cause);
    // Focus left: flush, and say so in the state.
    void FocusOut(HostField& field);
    // A click on a row of the menu: move to it, then commit (§5, `pick`).
    void Pick(size_t row, HostField& field, const ContextInfo& info);
    // The input menu: muhenkan, or henkan with a profile.
    void Select(Mode mode, const std::string& profile, HostField& field, const ContextInfo& info);
    // The indicator was clicked: the other mode, as `mode.toggle` goes there.
    void Toggle(HostField& field, const ContextInfo& info);
    // The field's preedit and menu were taken away without the engine (IBus, when focus leaves):
    // forget them.
    void Forget();

    // The last reply applied: what shows.
    const Reply& last() const { return last_; }
    // The published state (§6.7), as it would be now with this field in focus.
    std::string State(const ContextInfo& info) const { return session_.State(&last_, &info); }

private:
    // Follows the session: `select` after bada_new, and when mode, profile or settings changed.
    void Sync(HostField& field, const ContextInfo& info);
    void SendSelect(HostField& field, const ContextInfo& info);
    // Sends an event and applies its reply (§6.3). Returns the reply to the event itself.
    Reply Send(const std::string& event_json, HostField& field);
    // One press the tracker handed over. True: eaten.
    bool Run(const KeyTracker::Step& step, const RawKey& key, const std::string& code, HostField& field,
             const ContextInfo& info);
    // Carries out what the table decided. True: eaten.
    bool Do(const Decision& d, const KeyTracker::Step& step, const RawKey& key, const std::string& code,
            HostField& field, const ContextInfo& info);
    // A key the table doesn't take: the engine reads it (§6.4). True: eaten.
    bool SendKey(const std::string& code, const RawKey& key, HostField& field, const ContextInfo& info);
    bool LockOn() const;
    void Publish(const ContextInfo& info);

    Session& session_;
    Engine engine_;
    Reply last_;
    KeyTracker tracker_;
    RawKey waiting_;               // the press of the key that waits in the tracker
    std::set<std::string> eaten_;  // keys down whose press was eaten: their release is too
    std::set<std::string> bound_;  // keys down that had a binding: their repeats are swallowed
    bool selected_ = false;        // the engine had its first select
    Mode engine_mode_ = Mode::kMuhenkan;  // what this engine was last selected into
    std::uint64_t generation_ = 0;  // the session's, when this field last followed it
    bool notice_shown_ = false;
    bool bound_now_ = false;  // the last press decided had a binding
};

}  // namespace bada
