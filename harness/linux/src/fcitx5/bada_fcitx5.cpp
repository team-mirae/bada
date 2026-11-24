// The Fcitx5 frontend: the addon `bada`, with one input method, `bada`, which holds both modes.
// The session and the contexts (src/core/harness.h) do everything but moving keys in and text out,
// which is this file's.
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/surroundingtext.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>

#include "harness.h"
#include "publish.h"

#ifndef BADA_VERSION
#define BADA_VERSION "0.0.0"
#endif

namespace {

FCITX_DEFINE_LOG_CATEGORY(bada_log, "bada");
#define BADA_INFO() FCITX_LOGC(bada_log, Info)
#define BADA_ERROR() FCITX_LOGC(bada_log, Error)

// Rows a page of the menu holds. The page follows the selected row.
constexpr int kPageSize = 10;

class BadaEngine;

bada::ContextInfo InfoOf(fcitx::InputContext* ic) {
    bada::ContextInfo info;
    info.app = ic->program();
    const auto caps = ic->capabilityFlags();
    info.secure = caps.test(fcitx::CapabilityFlag::Password) || caps.test(fcitx::CapabilityFlag::Sensitive);
    return info;
}

// Whether the marked text is drawn by the app itself, in its text.
bool ClientPreedit(fcitx::InputContext* ic) { return ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit); }

// A row of the menu. A click moves to it and commits (the standard, §5: `pick`, then `flush`).
class Row final : public fcitx::CandidateWord {
public:
    Row(BadaEngine* engine, size_t row, const bada::Row& r, bool strip) : engine_(engine), row_(row) {
        setText(fcitx::Text(r.text));
        if (!r.note.empty()) setComment(fcitx::Text(r.note));
        // Script forms are labelled with their entries (−2, −1). A strip's rows have the digits
        // that pick them: 1 to 9, then 0. The other rows have no label.
        std::string label;
        if (r.entry < 0) label = "\xE2\x88\x92" + std::to_string(-r.entry) + " ";
        else if (strip && row < 10) label = std::to_string((row + 1) % 10) + " ";
        setCustomLabel(fcitx::Text(label));
    }
    void select(fcitx::InputContext* ic) const override;

private:
    BadaEngine* engine_;
    size_t row_;
};

// An input context as the harness's text field (the standard, §6.3).
class Field final : public bada::HostField {
public:
    // `discard`: the text is leaving the harness's hands. Whoever took the marked text (the app,
    // or Fcitx5 when focus leaves) commits it, so a commit here would insert it twice.
    Field(BadaEngine* engine, fcitx::InputContext* ic, bool discard = false)
        : engine_(engine), ic_(ic), discard_(discard) {}
    ~Field() override { Update(); }

    void Commit(const std::string& text, bool replace_selection) override {
        if (discard_) return;
        if (replace_selection) DeleteSelection();
        ic_->commitString(text);
    }

    void SetPreedit(const bada::Preedit& preedit) override {
        marked_ = !preedit.text.empty();
        fcitx::Text text;
        if (marked_) {
            // What a flush wouldn't commit as it shows must not be committed by Fcitx5 either.
            const fcitx::TextFormatFlags keep =
                preedit.is_staged ? fcitx::TextFormatFlag::NoFlag : fcitx::TextFormatFlag::DontCommit;
            size_t covered = 0;
            for (const bada::Segment& s : preedit.segments) covered += s.len;
            if (preedit.segments.empty() || covered != bada::Scalars(preedit.text)) {
                text.append(preedit.text, keep | fcitx::TextFormatFlag::Underline);
            } else {
                // The parts are drawn apart: what is still being read is underlined, what is
                // fixed is underlined and bold, and the row the user moved to is highlighted.
                size_t at = 0;
                for (const bada::Segment& s : preedit.segments) {
                    const size_t from = bada::ByteOffset(preedit.text, at);
                    const size_t to = bada::ByteOffset(preedit.text, at + s.len);
                    fcitx::TextFormatFlags flags = keep | fcitx::TextFormatFlag::Underline;
                    if (s.kind == bada::Segment::Kind::kFixed) flags |= fcitx::TextFormatFlag::Bold;
                    if (s.kind == bada::Segment::Kind::kPicked) flags |= fcitx::TextFormatFlag::HighLight;
                    text.append(preedit.text.substr(from, to - from), flags);
                    at += s.len;
                }
            }
            text.setCursor(static_cast<int>(bada::ByteOffset(preedit.text, preedit.caret)));
        }
        auto& panel = ic_->inputPanel();
        if (ClientPreedit(ic_)) {
            panel.setClientPreedit(text);
            panel.setPreedit(fcitx::Text());
        } else {
            panel.setPreedit(text);
        }
        dirty_ = true;
    }

    void ShowMenu(const bada::Menu& menu, bool) override {
        auto& panel = ic_->inputPanel();
        dirty_ = true;
        if (menu.rows.empty()) {
            panel.setCandidateList(nullptr);
            return;
        }
        auto list = std::make_unique<fcitx::CommonCandidateList>();
        list->setPageSize(kPageSize);
        list->setLabels(std::vector<std::string>(kPageSize, ""));
        list->setLayoutHint(menu.strip ? fcitx::CandidateLayoutHint::Horizontal
                                       : fcitx::CandidateLayoutHint::Vertical);
        for (size_t row = 0; row < menu.rows.size(); ++row) {
            list->append<Row>(engine_, row, menu.rows[row], menu.strip);
        }
        if (menu.selected) {
            list->setGlobalCursorIndex(static_cast<int>(*menu.selected));
            list->setPage(static_cast<int>(*menu.selected) / kPageSize);
        }
        panel.setCandidateList(std::move(list));
    }

    bada::Around TextAround(size_t before, size_t after) override {
        bada::Around around;
        // A password field gives nothing away.
        if (InfoOf(ic_).secure || !ic_->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) return around;
        const auto& s = ic_->surroundingText();
        if (!s.isValid()) return around;
        const std::string& text = s.text();
        const size_t length = bada::Scalars(text);
        const size_t from = std::min<size_t>(std::min(s.anchor(), s.cursor()), length);
        const size_t to = std::min<size_t>(std::max(s.anchor(), s.cursor()), length);
        auto slice = [&](size_t a, size_t b) {
            const size_t x = bada::ByteOffset(text, a), y = bada::ByteOffset(text, b);
            return text.substr(x, y - x);
        };
        // Nothing counts as selected while something is marked.
        if (!marked_) around.selection = slice(from, to);
        around.before = slice(from - std::min(before, from), from);
        around.after = slice(to, std::min(length, to + after));
        return around;
    }

    void ForwardKey(const bada::RawKey& key) override {
        fcitx::KeyStates states;
        if (key.mods.shift) states |= fcitx::KeyState::Shift;
        if (key.mods.ctrl) states |= fcitx::KeyState::Ctrl;
        if (key.mods.alt) states |= fcitx::KeyState::Alt;
        if (key.mods.meta) states |= fcitx::KeyState::Super;
        const fcitx::Key raw(static_cast<fcitx::KeySym>(key.sym), states, key.code ? static_cast<int>(key.code + 8) : 0);
        ic_->forwardKey(raw, false);
        ic_->forwardKey(raw, true);
    }

    void Notice(const std::string& text) override {
        ic_->inputPanel().setAuxUp(fcitx::Text(text));
        dirty_ = true;
    }

    // Shows what changed. Once per event, after every reply of it was applied.
    void Update() {
        if (!dirty_) return;
        dirty_ = false;
        ic_->updatePreedit();
        ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

private:
    // For `replace: "selection"`, in apps that don't replace the selection on a commit.
    void DeleteSelection() {
        if (!ic_->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) return;
        const auto& s = ic_->surroundingText();
        if (!s.isValid() || s.anchor() == s.cursor()) return;
        const int from = static_cast<int>(std::min(s.anchor(), s.cursor()));
        const int to = static_cast<int>(std::max(s.anchor(), s.cursor()));
        ic_->deleteSurroundingText(from - static_cast<int>(s.cursor()), static_cast<unsigned>(to - from));
    }

    BadaEngine* engine_;
    fcitx::InputContext* ic_;
    bool discard_;
    bool marked_ = false;
    bool dirty_ = false;
};

bada::RawKey KeyOf(const fcitx::KeyEvent& event) {
    const fcitx::Key raw = event.rawKey();
    bada::RawKey k;
    k.code = raw.code() > 8 ? static_cast<unsigned>(raw.code() - 8) : 0;  // XKB keycode → evdev
    k.sym = static_cast<unsigned>(raw.sym());
    const fcitx::KeyStates s = raw.states();
    k.mods.shift = s.test(fcitx::KeyState::Shift);
    k.mods.ctrl = s.test(fcitx::KeyState::Ctrl);
    k.mods.alt = s.test(fcitx::KeyState::Alt);
    k.mods.meta = s.test(fcitx::KeyState::Super) || s.test(fcitx::KeyState::Super2);
    k.caps = s.test(fcitx::KeyState::CapsLock);
    // What the app's own layout types for this key, if the key is let through (§6.4).
    k.os_text = fcitx::Key::keySymToUTF8(raw.sym());
    return k;
}

// Per input context: its Context, and the keys held down (to tell a repeat).
class State final : public fcitx::InputContextProperty {
public:
    std::unique_ptr<bada::Context> context;
    std::set<unsigned> down;
};

class BadaEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit BadaEngine(fcitx::Instance* instance)
        : instance_(instance), factory_([](fcitx::InputContext&) { return new State(); }) {
        bada::Session::Options options;
        options.frontend = "fcitx5";
        options.version = BADA_VERSION;
        options.sink = &publisher_;
        options.log = [](const std::string& line) { BADA_INFO() << line; };
        std::string error;
        session_ = bada::Session::Create(std::move(options), &error);
        if (!session_) {
            // Nothing types through bada then: every key goes to the app.
            BADA_ERROR() << "the engine can't be used: " << error;
        } else {
            session_->PublishIdle();
            // The settings folder is watched: a file written there is taken up at once (§10.1).
            if (session_->watch_fd() >= 0) {
                watch_ = instance_->eventLoop().addIOEvent(
                    session_->watch_fd(), fcitx::IOEventFlag::In,
                    [this](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                        if (session_->WatchFired()) SettingsChanged();
                        return true;
                    });
            }
        }
        instance_->inputContextManager().registerProperty("badaState", &factory_);
        mode_action_.setMenu(&menu_);
        instance_->userInterfaceManager().registerAction("bada-mode", &mode_action_);
        BuildMenu();
    }

    ~BadaEngine() override {
        for (auto& action : profile_actions_) instance_->userInterfaceManager().unregisterAction(action.get());
        instance_->userInterfaceManager().unregisterAction(&mode_action_);
    }

    void keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) override {
        auto* ic = event.inputContext();
        bada::Context* ctx = ContextFor(ic);
        if (!ctx) return;
        State* state = ic->propertyFor(&factory_);
        bada::RawKey key = KeyOf(event);
        const auto before = session_->generation();
        bool eaten;
        {
            Field field(this, ic);
            if (event.isRelease()) {
                state->down.erase(key.code);
                eaten = ctx->KeyUp(key, field, InfoOf(ic));
            } else {
                key.repeat = key.code && !state->down.insert(key.code).second;
                eaten = ctx->KeyDown(key, field, InfoOf(ic));
            }
        }
        if (eaten) event.filterAndAccept();
        if (session_->generation() != before) ModeChanged(ic);
    }

    void activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
        auto* ic = event.inputContext();
        ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, &mode_action_);
        if (bada::Context* ctx = ContextFor(ic)) {
            Field field(this, ic);
            ctx->FocusIn(field, InfoOf(ic));
        }
        UpdateAction(ic);
    }

    void deactivate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
        auto* ic = event.inputContext();
        State* state = ic->propertyFor(&factory_);
        state->down.clear();
        if (!state->context) return;
        if (event.type() == fcitx::EventType::InputContextSwitchInputMethod) {
            // Another input method takes over: what is staged is committed here, by the harness.
            Field field(this, ic);
            state->context->Flush(field, "harness");
        } else {
            // Focus left. Marked text the app draws is committed by Fcitx5, or by the app itself
            // (ClientUnfocusCommit), as it shows; the harness only lets go of it. Marked text in
            // the panel is nobody's but the harness's to commit.
            Field field(this, ic, TakenByOthers(ic, *state->context));
            state->context->FocusOut(field);
        }
        ClearPanel(ic);
    }

    // The app reset the field: a click in the text, or an edit of its own. Apps commit or drop the
    // marked text they draw before they say so; the rest is the harness's to commit.
    void reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
        auto* ic = event.inputContext();
        State* state = ic->propertyFor(&factory_);
        state->down.clear();
        if (!state->context) return;
        {
            Field field(this, ic, TakenByOthers(ic, *state->context));
            state->context->Flush(field, "click");
        }
        ClearPanel(ic);
    }

    std::string subMode(const fcitx::InputMethodEntry&, fcitx::InputContext&) override {
        return session_ ? session_->ModeText() : std::string();
    }

    std::string subModeLabelImpl(const fcitx::InputMethodEntry&, fcitx::InputContext&) override {
        return session_ ? session_->Label() : std::string();
    }

    std::string subModeIconImpl(const fcitx::InputMethodEntry& entry, fcitx::InputContext&) override {
        return entry.icon();
    }

    // A click on a row of the menu.
    void Pick(fcitx::InputContext* ic, size_t row) {
        bada::Context* ctx = ContextFor(ic);
        if (!ctx) return;
        Field field(this, ic);
        ctx->Pick(row, field, InfoOf(ic));
    }

private:
    bada::Context* ContextFor(fcitx::InputContext* ic) {
        if (!session_) return nullptr;
        State* state = ic->propertyFor(&factory_);
        if (!state->context) {
            state->context = std::make_unique<bada::Context>(*session_);
            if (!state->context->ok()) {
                BADA_ERROR() << "the engine made no text field";
                state->context.reset();
                return nullptr;
            }
        }
        return state->context.get();
    }

    // The marked text is committed by the app or by Fcitx5, exactly as a flush would commit it.
    static bool TakenByOthers(fcitx::InputContext* ic, const bada::Context& ctx) {
        return ClientPreedit(ic) && ctx.last().preedit.is_staged;
    }

    static void ClearPanel(fcitx::InputContext* ic) {
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    // The input menu: muhenkan, and the henkan profiles by name (the standard, §8.1).
    void BuildMenu() {
        auto& ui = instance_->userInterfaceManager();
        for (auto& action : profile_actions_) {
            menu_.removeAction(action.get());
            ui.unregisterAction(action.get());
        }
        profile_actions_.clear();
        if (!session_) return;
        auto add = [&](const std::string& label, bada::Mode mode, const std::string& id) {
            auto action = std::make_unique<fcitx::SimpleAction>();
            action->setShortText(label);
            action->setCheckable(true);
            action->connect<fcitx::SimpleAction::Activated>([this, mode, id](fcitx::InputContext* ic) {
                if (ic) SelectFromMenu(ic, mode, id);
            });
            ui.registerAction("bada-select-" + std::to_string(profile_actions_.size()), action.get());
            menu_.addAction(action.get());
            action_targets_.push_back({mode, id});
            profile_actions_.push_back(std::move(action));
        };
        action_targets_.clear();
        add("muhenkan", bada::Mode::kMuhenkan, "");
        for (const bada::Profile& p : session_->profiles()) add(p.name.empty() ? p.id : p.name, bada::Mode::kHenkan, p.id);
    }

    void UpdateAction(fcitx::InputContext* ic) {
        if (!session_) return;
        mode_action_.setShortText("bada: " + session_->ModeText());
        mode_action_.setLongText("bada: muhenkan, or a henkan profile");
        mode_action_.update(ic);
        for (size_t i = 0; i < profile_actions_.size(); ++i) {
            const auto& [mode, id] = action_targets_[i];
            const bool current = mode == session_->mode() && (mode == bada::Mode::kMuhenkan || id == session_->profile());
            profile_actions_[i]->setChecked(current);
            profile_actions_[i]->update(ic);
        }
    }

    void SelectFromMenu(fcitx::InputContext* ic, bada::Mode mode, const std::string& profile) {
        bada::Context* ctx = ContextFor(ic);
        if (!ctx) return;
        {
            Field field(this, ic);
            ctx->Select(mode, profile, field, InfoOf(ic));
        }
        ModeChanged(ic);
    }

    // Mode or profile changed, or the settings: the indicator follows.
    void ModeChanged(fcitx::InputContext* ic) {
        UpdateAction(ic);
        ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        instance_->showInputMethodInformation(ic);
    }

    void SettingsChanged() {
        BuildMenu();
        auto* ic = instance_->mostRecentInputContext();
        State* state = ic ? ic->propertyFor(&factory_) : nullptr;
        if (ic && ic->hasFocus() && state->context && instance_->inputMethod(ic) == "bada") {
            // The field in focus follows at once, and the state is published with what was read.
            Field field(this, ic);
            state->context->FocusIn(field, InfoOf(ic));
            field.Update();
            UpdateAction(ic);
            ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        } else {
            session_->PublishIdle();
        }
    }

    fcitx::Instance* instance_;
    bada::Publisher publisher_;
    std::unique_ptr<bada::Session> session_;
    std::unique_ptr<fcitx::EventSourceIO> watch_;
    fcitx::SimpleAction mode_action_;
    fcitx::Menu menu_;
    std::vector<std::unique_ptr<fcitx::SimpleAction>> profile_actions_;
    std::vector<std::pair<bada::Mode, std::string>> action_targets_;
    // After session_: destroyed first, so every Context goes before its Session.
    fcitx::FactoryFor<State> factory_;
};

void Row::select(fcitx::InputContext* ic) const { engine_->Pick(ic, row_); }

class Factory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override { return new BadaEngine(manager->instance()); }
};

}  // namespace

FCITX_ADDON_FACTORY(Factory);
