// The IBus frontend: ibus-engine-bada, the engine `bada` of the component com.teammirae.Bada,
// started by ibus-daemon from share/ibus/component/com.teammirae.Bada.xml. IBus makes one engine
// object per input context; each holds a Context of the process's one Session
// (src/core/harness.h), which does everything but moving keys in and text out.
//
//   ibus-engine-bada --ibus     started by ibus-daemon
//   ibus-engine-bada            run by hand: it registers the component itself
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>

#include <glib-unix.h>
#include <ibus.h>

#include "harness.h"
#include "publish.h"
#include "tide_hub.h"

#ifndef BADA_VERSION
#define BADA_VERSION "0.0.0"
#endif

namespace {

constexpr const char* kComponent = "com.teammirae.Bada";
constexpr const char* kEngineName = "bada";

bada::Session* g_session = nullptr;
bada::TideHub* g_tide = nullptr;
bool g_caps = false;  // the lock, as the last key had it

// --- the engine object

struct BadaEngine {
    IBusEngine parent;
    bada::Context* context;
    std::set<unsigned>* down;  // keys held (to tell a repeat)
    IBusPropList* props;
    IBusProperty* mode_prop;
    gboolean secure;
};

struct BadaEngineClass {
    IBusEngineClass parent;
};

GType bada_engine_get_type();
#define BADA_ENGINE(o) (reinterpret_cast<BadaEngine*>(o))

G_DEFINE_TYPE(BadaEngine, bada_engine, IBUS_TYPE_ENGINE)

// The engine whose input context has focus, or nullptr: bada-tide takes keys only while there is one.
BadaEngine* g_focused = nullptr;
void SyncTide();

bada::ContextInfo InfoOf(BadaEngine* e) {
    bada::ContextInfo info;
    info.secure = e->secure;  // IBus doesn't tell an engine the program's name
    return info;
}

// The input context as the harness's text field (the standard, §6.3).
//
// The preedit is shown in IBUS_ENGINE_PREEDIT_COMMIT mode: when focus leaves, the client commits
// it itself, because ibus-daemon detaches the engine from a context that lost focus and a commit
// sent then is lost. So focus out flushes the engine into a field that drops the commit
// (`discard`). Where what shows isn't what a flush would commit, the preedit is shown in CLEAR
// mode instead.
class Field final : public bada::HostField {
public:
    explicit Field(IBusEngine* engine, bool discard = false) : engine_(engine), discard_(discard) {}

    void Commit(const std::string& text, bool replace_selection) override {
        if (discard_) return;
        if (replace_selection) DeleteSelection();
        ibus_engine_commit_text(engine_, ibus_text_new_from_string(text.c_str()));
    }

    void SetPreedit(const bada::Preedit& preedit) override {
        marked_ = !preedit.text.empty();
        if (discard_ || !marked_) {
            ibus_engine_hide_preedit_text(engine_);
            ibus_engine_update_preedit_text_with_mode(engine_, ibus_text_new_from_static_string(""), 0, FALSE,
                                                      IBUS_ENGINE_PREEDIT_CLEAR);
            return;
        }
        IBusText* text = ibus_text_new_from_string(preedit.text.c_str());
        const guint length = static_cast<guint>(bada::Scalars(preedit.text));
        size_t covered = 0;
        for (const bada::Segment& s : preedit.segments) covered += s.len;
        if (preedit.segments.empty() || covered != length) {
            ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE, IBUS_ATTR_UNDERLINE_SINGLE, 0, length);
        } else {
            // The parts are drawn apart: what is still being read has a single underline, what is
            // fixed a low one, and the row the user moved to a double one.
            guint at = 0;
            for (const bada::Segment& s : preedit.segments) {
                const guint to = at + static_cast<guint>(s.len);
                const guint line = s.kind == bada::Segment::Kind::kFixed    ? IBUS_ATTR_UNDERLINE_LOW
                                   : s.kind == bada::Segment::Kind::kPicked ? IBUS_ATTR_UNDERLINE_DOUBLE
                                                                            : IBUS_ATTR_UNDERLINE_SINGLE;
                ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE, line, at, to);
                at = to;
            }
        }
        ibus_engine_update_preedit_text_with_mode(
            engine_, text, static_cast<guint>(preedit.caret), TRUE,
            preedit.is_staged ? IBUS_ENGINE_PREEDIT_COMMIT : IBUS_ENGINE_PREEDIT_CLEAR);
    }

    void ShowMenu(const bada::Menu& menu, bool) override {
        if (discard_ || menu.rows.empty()) {
            ibus_engine_hide_lookup_table(engine_);
            return;
        }
        const guint n = static_cast<guint>(menu.rows.size());
        const bool shown = menu.selected.has_value();
        IBusLookupTable* table = ibus_lookup_table_new(std::min<guint>(n, 10), shown ? static_cast<guint>(*menu.selected) : 0,
                                                       shown, FALSE);
        ibus_lookup_table_set_orientation(table, menu.strip ? IBUS_ORIENTATION_HORIZONTAL : IBUS_ORIENTATION_VERTICAL);
        for (guint row = 0; row < n; ++row) {
            const bada::Row& r = menu.rows[row];
            std::string text = r.text;
            if (!r.note.empty()) text += "  " + r.note;
            ibus_lookup_table_append_candidate(table, ibus_text_new_from_string(text.c_str()));
        }
        // Labels go by place in the page. Only a strip has them: the digits that pick its rows.
        for (guint i = 0; i < 10; ++i) {
            const std::string label = menu.strip ? std::to_string((i + 1) % 10) : std::string();
            ibus_lookup_table_append_label(table, ibus_text_new_from_string(label.c_str()));
        }
        ibus_engine_update_lookup_table(engine_, table, TRUE);
    }

    bada::Around TextAround(size_t before, size_t after) override {
        bada::Around around;
        if (BADA_ENGINE(engine_)->secure || !(engine_->client_capabilities & IBUS_CAP_SURROUNDING_TEXT)) return around;
        IBusText* text = nullptr;
        guint cursor = 0, anchor = 0;
        ibus_engine_get_surrounding_text(engine_, &text, &cursor, &anchor);
        if (!text) return around;
        const std::string s = ibus_text_get_text(text);
        const size_t length = bada::Scalars(s);
        const size_t from = std::min<size_t>(std::min(cursor, anchor), length);
        const size_t to = std::min<size_t>(std::max(cursor, anchor), length);
        auto slice = [&](size_t a, size_t b) {
            const size_t x = bada::ByteOffset(s, a), y = bada::ByteOffset(s, b);
            return s.substr(x, y - x);
        };
        if (!marked_) around.selection = slice(from, to);
        around.before = slice(from - std::min(before, from), from);
        around.after = slice(to, std::min(length, to + after));
        return around;
    }

    void ForwardKey(const bada::RawKey& key) override {
        guint state = 0;
        if (key.mods.shift) state |= IBUS_SHIFT_MASK;
        if (key.mods.ctrl) state |= IBUS_CONTROL_MASK;
        if (key.mods.alt) state |= IBUS_MOD1_MASK;
        if (key.mods.meta) state |= IBUS_SUPER_MASK;
        ibus_engine_forward_key_event(engine_, key.sym, key.code, state);
        ibus_engine_forward_key_event(engine_, key.sym, key.code, state | IBUS_RELEASE_MASK);
    }

    void Notice(const std::string& text) override {
        if (text.empty()) ibus_engine_hide_auxiliary_text(engine_);
        else ibus_engine_update_auxiliary_text(engine_, ibus_text_new_from_string(text.c_str()), TRUE);
    }

private:
    // For `replace: "selection"`, in apps that don't replace the selection on a commit.
    void DeleteSelection() {
        if (!(engine_->client_capabilities & IBUS_CAP_SURROUNDING_TEXT)) return;
        IBusText* text = nullptr;
        guint cursor = 0, anchor = 0;
        ibus_engine_get_surrounding_text(engine_, &text, &cursor, &anchor);
        if (!text || cursor == anchor) return;
        const guint from = std::min(cursor, anchor), to = std::max(cursor, anchor);
        ibus_engine_delete_surrounding_text(engine_, static_cast<gint>(from) - static_cast<gint>(cursor), to - from);
    }

    IBusEngine* engine_;
    bool discard_;
    bool marked_ = false;
};

void ShowMode(BadaEngine* e) {
    const std::string label = g_session->Label();
    ibus_property_set_symbol(e->mode_prop, ibus_text_new_from_string(label.c_str()));
    const std::string name = "bada (" + g_session->ModeText() + ")";
    ibus_property_set_label(e->mode_prop, ibus_text_new_from_string(name.c_str()));
    ibus_engine_update_property(IBUS_ENGINE(e), e->mode_prop);
}

gboolean ProcessKey(IBusEngine* engine, guint keyval, guint keycode, guint state) {
    BadaEngine* e = BADA_ENGINE(engine);
    bada::RawKey key;
    key.code = keycode;  // IBus gives the evdev code (the X keycode − 8)
    key.sym = keyval;
    key.mods.shift = state & IBUS_SHIFT_MASK;
    key.mods.ctrl = state & IBUS_CONTROL_MASK;
    key.mods.alt = state & IBUS_MOD1_MASK;
    key.mods.meta = state & (IBUS_SUPER_MASK | IBUS_MOD4_MASK);
    key.caps = state & IBUS_LOCK_MASK;
    g_caps = key.caps;
    // What the app's own layout types for this key, if the key is let through (§6.4).
    if (const gunichar c = ibus_keyval_to_unicode(keyval)) {
        char utf8[8] = {};
        utf8[g_unichar_to_utf8(c, utf8)] = 0;
        key.os_text = utf8;
    }
    Field field(engine);
    const auto before = g_session->generation();
    gboolean eaten = FALSE;
    if (state & IBUS_RELEASE_MASK) {
        e->down->erase(keycode);
        eaten = e->context->KeyUp(key, field, InfoOf(e));
    } else {
        key.repeat = keycode && !e->down->insert(keycode).second;
        eaten = e->context->KeyDown(key, field, InfoOf(e));
    }
    if (g_session->generation() != before) ShowMode(e);
    SyncTide();
    return eaten;
}

void FocusIn(IBusEngine* engine) {
    BadaEngine* e = BADA_ENGINE(engine);
    ibus_engine_register_properties(engine, e->props);
    // ibus-daemon sends a context's surrounding text only after the engine asked for it once.
    if (engine->client_capabilities & IBUS_CAP_SURROUNDING_TEXT) {
        IBusText* text = nullptr;
        guint cursor = 0, anchor = 0;
        ibus_engine_get_surrounding_text(engine, &text, &cursor, &anchor);
    }
    Field field(engine);
    e->context->FocusIn(field, InfoOf(e));
    ShowMode(e);
    g_focused = e;
    SyncTide();
    IBUS_ENGINE_CLASS(bada_engine_parent_class)->focus_in(engine);
}

// Focus left: the client commits the preedit itself (PREEDIT_COMMIT, above); the engine only lets
// go of it, into a field that drops the commit. Where what shows isn't what is staged, the
// preedit was in CLEAR mode, and nothing of it is committed.
void FocusOut(IBusEngine* engine) {
    BadaEngine* e = BADA_ENGINE(engine);
    e->down->clear();
    Field field(engine, true);
    e->context->FocusOut(field);
    if (g_focused == e) g_focused = nullptr;
    SyncTide();
    IBUS_ENGINE_CLASS(bada_engine_parent_class)->focus_out(engine);
}

// The app reset the field (a click in the text), or the engine was switched away: what is staged
// is committed (§6.3). A preedit shown in PREEDIT_COMMIT mode is committed by IBus itself then,
// so the engine only lets go of it; anything else is the harness's to commit.
void Commit(IBusEngine* engine, const char* cause) {
    BadaEngine* e = BADA_ENGINE(engine);
    e->down->clear();
    const bada::Preedit& shown = e->context->last().preedit;
    Field field(engine, !shown.text.empty() && shown.is_staged);
    e->context->Flush(field, cause);
}

void Reset(IBusEngine* engine) {
    Commit(engine, "click");
    IBUS_ENGINE_CLASS(bada_engine_parent_class)->reset(engine);
}

void Disable(IBusEngine* engine) {
    Commit(engine, "harness");
    if (g_focused == BADA_ENGINE(engine)) g_focused = nullptr;
    SyncTide();
    IBUS_ENGINE_CLASS(bada_engine_parent_class)->disable(engine);
}

void SetContentType(IBusEngine* engine, guint purpose, guint hints) {
    BadaEngine* e = BADA_ENGINE(engine);
    bool secure = purpose == IBUS_INPUT_PURPOSE_PASSWORD || purpose == IBUS_INPUT_PURPOSE_PIN;
#if IBUS_CHECK_VERSION(1, 5, 26)
    secure = secure || (hints & IBUS_INPUT_HINT_PRIVATE);
#else
    (void)hints;
#endif
    if (secure && !e->secure) Commit(engine, "harness");
    e->secure = secure;
}

// A click on the indicator switches mode.
void PropertyActivate(IBusEngine* engine, const gchar* name, guint) {
    BadaEngine* e = BADA_ENGINE(engine);
    if (std::strcmp(name, "InputMode") != 0) return;
    Field field(engine);
    e->context->Toggle(field, InfoOf(e));
    ShowMode(e);
    SyncTide();
}

// A click on a row of the menu: move to it, then commit.
void CandidateClicked(IBusEngine* engine, guint index, guint, guint) {
    BadaEngine* e = BADA_ENGINE(engine);
    Field field(engine);
    e->context->Pick(index, field, InfoOf(e));
}

void bada_engine_init(BadaEngine* e) {
    e->context = new bada::Context(*g_session);
    e->down = new std::set<unsigned>();
    e->secure = FALSE;
    e->props = ibus_prop_list_new();
    g_object_ref_sink(e->props);
    // "InputMode": GNOME Shell shows this property's symbol as the indicator.
    e->mode_prop = ibus_property_new("InputMode", PROP_TYPE_NORMAL, ibus_text_new_from_static_string("bada"), nullptr,
                                     ibus_text_new_from_static_string("Switch mode"), TRUE, TRUE, PROP_STATE_UNCHECKED,
                                     nullptr);
    g_object_ref_sink(e->mode_prop);
    ibus_prop_list_append(e->props, e->mode_prop);
}

void Destroy(IBusObject* object) {
    BadaEngine* e = BADA_ENGINE(object);
    if (g_focused == e) {
        g_focused = nullptr;
        SyncTide();
    }
    delete e->context;
    e->context = nullptr;
    delete e->down;
    e->down = nullptr;
    g_clear_object(&e->mode_prop);
    g_clear_object(&e->props);
    IBUS_OBJECT_CLASS(bada_engine_parent_class)->destroy(object);
}

void bada_engine_class_init(BadaEngineClass* klass) {
    IBUS_OBJECT_CLASS(klass)->destroy = Destroy;
    IBusEngineClass* e = IBUS_ENGINE_CLASS(klass);
    e->process_key_event = ProcessKey;
    e->focus_in = FocusIn;
    e->focus_out = FocusOut;
    e->reset = Reset;
    e->disable = Disable;
    e->set_content_type = SetContentType;
    e->property_activate = PropertyActivate;
    e->candidate_clicked = CandidateClicked;
}

// The settings folder changed (§10.1): read it again. Each context follows at its next event.
gboolean SettingsChanged(gint, GIOCondition, gpointer) {
    if (g_session->WatchFired()) {
        if (g_tide) g_tide->Reload();
        g_session->PublishIdle();
        SyncTide();
    }
    return G_SOURCE_CONTINUE;
}

// --- bada-tide (docs/bada-tide-v1.md; src/common/tide_hub.h)

// Tells the hub the harness's state (bada-tide §2). It takes the language keys the key table binds,
// never Caps Lock, whose lock is the system's, and only while bada has a field in focus.
void SyncTide() {
    if (!g_tide || !g_session) return;
    bada::tide::Host host;
    host.mode = g_session->mode();
    const std::vector<std::string>& cycle = g_session->table().cycle;
    const auto it = std::find(cycle.begin(), cycle.end(), g_session->profile());
    host.profile = g_session->profile().empty() || it == cycle.end() ? -1 : static_cast<int>(it - cycle.begin());
    host.caps = g_caps;
    if (g_focused) {
        for (const bada::Binding& b : g_session->table().bindings) {
            for (const std::string& k : {b.key, b.key2}) {
                if (k != "CapsLock" && bada::IsLanguageKey(k)) host.take.insert(k);
            }
        }
    }
    g_tide->SetHost(host);
}

// A key the keyboard reported over bada-tide, that bada took: it runs through the key table as the
// key itself would have, in the field in focus. A press the harness doesn't eat goes to the app.
void TideKey(const bada::tide::Link::Key& event) {
    BadaEngine* e = g_focused;
    bada::RawKey key;
    if (!e || !bada::tide::EvdevOf(event.code, &key.code, &key.sym)) return;
    key.mods = event.mods;
    key.caps = g_caps;
    IBusEngine* engine = IBUS_ENGINE(e);
    const auto before = g_session->generation();
    {
        Field field(engine);
        if (event.down) {
            if (!e->context->KeyDown(key, field, InfoOf(e))) field.ForwardKey(key);
        } else {
            e->context->KeyUp(key, field, InfoOf(e));
        }
    }
    if (g_session->generation() != before) ShowMode(e);
    SyncTide();
}

// The links changed: the state says so (`harness.tide`).
void TideStatusChanged() {
    if (!g_session) return;
    if (g_focused) g_focused->context->Publish(InfoOf(g_focused));
    else g_session->PublishIdle();
}

gboolean TideReady(gint, GIOCondition, gpointer) {
    if (g_tide) g_tide->Dispatch();
    return G_SOURCE_CONTINUE;
}

}  // namespace

int main(int argc, char** argv) {
    bool by_daemon = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ibus") {
            by_daemon = true;
        } else if (a == "--version") {
            std::printf("ibus-engine-bada " BADA_VERSION "\n");
            return 0;
        } else {
            std::fprintf(stderr, "usage: ibus-engine-bada [--ibus]\n");
            return 2;
        }
    }

    bada::Publisher publisher;
    bada::Session::Options options;
    options.frontend = "ibus";
    options.version = BADA_VERSION;
    options.data_dir = bada::Session::DataDir();
    options.sink = &publisher;
    options.log = [](const std::string& line) { std::fprintf(stderr, "ibus-engine-bada: %s\n", line.c_str()); };
    options.tide = [] { return g_tide ? g_tide->Status() : std::string(); };
    std::string error;
    const std::unique_ptr<bada::Session> session = bada::Session::Create(std::move(options), &error);
    if (!session) {
        std::fprintf(stderr, "ibus-engine-bada: the engine can't be used: %s\n", error.c_str());
        return 1;
    }
    g_session = session.get();
    session->PublishIdle();

    bada::TideHub::Options tide_options;
    tide_options.settings_dir = session->folder().dir();
    tide_options.log = [](const std::string& line) { std::fprintf(stderr, "ibus-engine-bada: %s\n", line.c_str()); };
    tide_options.key = TideKey;
    tide_options.changed = TideStatusChanged;
    std::unique_ptr<bada::TideHub> tide = std::make_unique<bada::TideHub>(std::move(tide_options));
    if (tide->fd() >= 0) {
        g_tide = tide.get();
        g_unix_fd_add(tide->fd(), G_IO_IN, TideReady, nullptr);
    }

    ibus_init();
    IBusBus* bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus)) {
        std::fprintf(stderr, "ibus-engine-bada: can't connect to ibus-daemon\n");
        return 1;
    }
    g_signal_connect(bus, "disconnected", G_CALLBACK(+[](IBusBus*, gpointer) { ibus_quit(); }), nullptr);
    if (session->watch_fd() >= 0) g_unix_fd_add(session->watch_fd(), G_IO_IN, SettingsChanged, nullptr);
    IBusFactory* factory = ibus_factory_new(ibus_bus_get_connection(bus));
    ibus_factory_add_engine(factory, kEngineName, bada_engine_get_type());
    if (by_daemon) {
        ibus_bus_request_name(bus, kComponent, 0);
    } else {
        // Run by hand: register the component ourselves.
        IBusComponent* component =
            ibus_component_new(kComponent, "bada", BADA_VERSION, "MIT", "Mirae Research", "https://github.com/team-mirae/bada", "", "");
        ibus_component_add_engine(component, ibus_engine_desc_new(kEngineName, "bada", "bada: one engine, every layout", "ja",
                                                                  "MIT", "Mirae Research", "input-keyboard", "default"));
        ibus_bus_register_component(bus, component);
    }
    ibus_main();
    g_tide = nullptr;
    tide.reset();
    g_session = nullptr;
    return 0;
}
