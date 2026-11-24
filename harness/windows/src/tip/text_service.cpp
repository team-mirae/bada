// Adapted from bada-windows src/tip/text_service.cpp (jis-moonshot's).
#include "text_service.h"

#include <InputScope.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <new>

#include "display_attribute.h"
#include "dll.h"
#include "edit_session.h"
#include "guids.h"
#include "keys.h"
#include "log.h"
#include "state_shared.h"

using Microsoft::WRL::ComPtr;

namespace bada::tip {
namespace {

// In an app that refuses a composition, or ends it right after each update, the marked text stays
// on screen as plain text and is replaced at every key.
constexpr bool kKeepTextWithoutComposition = true;
// An app that ends the composition this soon after our update did it by itself (measured in
// jis-moonshot: 15 to 31 ms).
constexpr ULONGLONG kOwnTermination = 100;
// Our injected Caps Lock press comes back through the key sink within this long.
constexpr ULONGLONG kLockEcho = 500;
// How many text fields a thread keeps an engine for. The one least recently used goes first.
constexpr size_t kMaxFields = 16;
// The most text read around the caret for the engine, in characters.
constexpr size_t kMaxAround = 256;

enum MenuId : UINT {
    kMenuMuhenkan = 1,
    kMenuAbout,
    kMenuNoProfile,
    kMenuProfileFirst = 100,  // + its place in the list the menu showed
};

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

bool SameObject(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> x, y;
    if (!a || !b || FAILED(a->QueryInterface(IID_PPV_ARGS(&x))) || FAILED(b->QueryInterface(IID_PPV_ARGS(&y)))) {
        return false;
    }
    return x == y;
}

HRESULT GetCompartment(IUnknown* owner, REFGUID guid, ComPtr<ITfCompartment>* out) {
    ComPtr<ITfCompartmentMgr> manager;
    HRESULT hr = owner->QueryInterface(IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    return manager->GetCompartment(guid, &*out);
}

LONG ReadCompartment(ITfCompartment* compartment) {
    VARIANT v;
    VariantInit(&v);
    if (!compartment || FAILED(compartment->GetValue(&v))) return 0;
    const LONG value = v.vt == VT_I4 ? v.lVal : 0;
    VariantClear(&v);  // another TIP or app may have stored a string or an object there
    return value;
}

void WriteCompartment(ITfCompartment* compartment, TfClientId client_id, LONG value) {
    if (!compartment) return;
    VARIANT v;
    v.vt = VT_I4;
    v.lVal = value;
    compartment->SetValue(client_id, &v);
}

// The key in hand was injected by the text service itself (guids.h, kInjectedMark).
bool Injected() { return static_cast<ULONG_PTR>(GetMessageExtraInfo()) == static_cast<ULONG_PTR>(kInjectedMark); }

// The first `n` characters (Unicode scalar values) of `text`, and the last `n`.
std::wstring FirstScalars(const std::wstring& text, size_t n) {
    size_t i = 0;
    for (; i < text.size() && n > 0; --n) {
        const bool pair = IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1]);
        i += pair ? 2 : 1;
    }
    return text.substr(0, i);
}

std::wstring LastScalars(const std::wstring& text, size_t n) {
    size_t i = text.size();
    for (; i > 0 && n > 0; --n) {
        const bool pair = IS_LOW_SURROGATE(text[i - 1]) && i >= 2 && IS_HIGH_SURROGATE(text[i - 2]);
        i -= pair ? 2 : 1;
    }
    return text.substr(i);
}

// The text of `range`, at most `limit` UTF-16 code units of it.
std::wstring TextOf(TfEditCookie ec, ITfRange* range, size_t limit) {
    std::wstring text;
    ComPtr<ITfRange> walk;
    if (!range || FAILED(range->Clone(&walk))) return text;
    WCHAR buffer[256];
    ULONG got = 0;
    while (text.size() < limit && SUCCEEDED(walk->GetText(ec, TF_TF_MOVESTART, buffer, ARRAYSIZE(buffer), &got)) && got) {
        text.append(buffer, got);
    }
    if (text.size() > limit) text.resize(limit);
    return text;
}

// Where the caret is on screen, for what is shown with no marked text to anchor it.
RECT CaretRect() {
    RECT rect = {};
    GUITHREADINFO gui = {sizeof(gui)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &gui) && gui.hwndCaret) {
        rect = gui.rcCaret;
        MapWindowPoints(gui.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    } else if (HWND focus = GetFocus()) {
        GetWindowRect(focus, &rect);
        rect.bottom = rect.top + 24;
    }
    return rect;
}

// A text field that only takes note of what is committed: for a flush whose text is put in the
// document by the caller, inside an edit session that is already running.
class CaptureField final : public HostField {
public:
    void Commit(const std::string& text, bool) override {
        commit += text;
        committed = true;
    }
    void SetPreedit(const Preedit&) override {}
    void ShowMenu(const Menu&, bool) override {}
    Around TextAround(size_t, size_t) override { return {}; }

    std::string commit;
    bool committed = false;
};

}  // namespace

// The document, as the harness sees it (src/core/reply.h). One reply is one edit session: its
// commit and its marked text go in together.
class TextService::TsfField final : public HostField {
public:
    TsfField(TextService* service, ITfContext* context) : service_(service), context_(context) {}

    void Begin() override { batching_ = true; }
    void End() override {
        batching_ = false;
        Run();
    }
    void Commit(const std::string& text, bool replace_selection) override {
        commit_ = Wide(text);
        replace_selection_ = replace_selection;
        dirty_ = true;
        if (!batching_) Run();
    }
    void SetPreedit(const Preedit& preedit) override {
        preedit_ = preedit;
        dirty_ = true;
        if (!batching_) Run();
    }
    void ShowMenu(const Menu& menu, bool) override { service_->menu_ = menu; }
    Around TextAround(size_t before, size_t after) override { return service_->ReadAround(context_, before, after); }
    void ForwardKey(const RawKey& key) override { service_->ForwardKey(key); }
    void Notice(const std::string& text) override { service_->notice_ = text; }

    // The marked text went in where nothing shows it: the engine mustn't keep it.
    bool invisible() const { return invisible_; }

private:
    void Run() {
        if (!dirty_) return;
        dirty_ = false;
        const bool synchronous =
            service_->UpdateText(context_, commit_, service_->MarkedOf(preedit_), replace_selection_);
        if (context_ && synchronous && !preedit_.text.empty() && !service_->composition_ && !service_->orphan_) {
            invisible_ = true;
        }
        commit_.clear();
        preedit_ = Preedit{};
        replace_selection_ = false;
    }

    TextService* service_;
    ITfContext* context_;
    std::wstring commit_;
    Preedit preedit_;
    bool replace_selection_ = false;
    bool batching_ = false;
    bool dirty_ = false;
    bool invisible_ = false;
};

TextService::TextService() { DllAddRef(); }

TextService::~TextService() {
    // A text service that was only asked for its display attributes never made the session.
    if (Process* process = PeekProcess()) {
        std::lock_guard<std::recursive_mutex> lock(process->mutex);
        fields_.clear();
    }
    DllRelease();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor || riid == IID_ITfTextInputProcessorEx) {
        *object = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (riid == IID_ITfKeyEventSink) {
        *object = static_cast<ITfKeyEventSink*>(this);
    } else if (riid == IID_ITfThreadMgrEventSink) {
        *object = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (riid == IID_ITfActiveLanguageProfileNotifySink) {
        *object = static_cast<ITfActiveLanguageProfileNotifySink*>(this);
    } else if (riid == IID_ITfCompositionSink) {
        *object = static_cast<ITfCompositionSink*>(this);
    } else if (riid == IID_ITfCompartmentEventSink) {
        *object = static_cast<ITfCompartmentEventSink*>(this);
    } else if (riid == IID_ITfDisplayAttributeProvider) {
        *object = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) TextService::Release() {
    const LONG refs = InterlockedDecrement(&refs_);
    if (!refs) delete this;
    return refs;
}

// --- activation

STDMETHODIMP TextService::Activate(ITfThreadMgr* thread_mgr, TfClientId client_id) {
    return ActivateEx(thread_mgr, client_id, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* thread_mgr, TfClientId client_id, DWORD) {
    thread_mgr_ = thread_mgr;
    client_id_ = client_id;
    active_ = true;

    ComPtr<ITfCategoryMgr> categories;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories)))) {
        categories->RegisterGUID(kDisplayAttributeLive, &live_attribute_);
        categories->RegisterGUID(kDisplayAttributeFixed, &fixed_attribute_);
        categories->RegisterGUID(kDisplayAttributePicked, &picked_attribute_);
    }
    ComPtr<ITfKeystrokeMgr> keystrokes;
    if (SUCCEEDED(thread_mgr_.As(&keystrokes))) {
        const HRESULT hr = keystrokes->AdviseKeyEventSink(client_id_, this, TRUE);
        if (FAILED(hr)) log::Write(L"AdviseKeyEventSink failed %#lx", hr);
    }
    ComPtr<ITfSource> source;
    if (SUCCEEDED(thread_mgr_.As(&source))) {
        source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &thread_sink_cookie_);
        source->AdviseSink(IID_ITfActiveLanguageProfileNotifySink,
                           static_cast<ITfActiveLanguageProfileNotifySink*>(this), &profile_sink_cookie_);
    }
    lang_bar_ = new (std::nothrow) LangBarButton(
        [this](ITfMenu* menu) { InitMenu(menu); }, [this](UINT id) { OnMenu(id); },
        [this] {
            // A click is the toggle: the other mode, as a key bound to `mode.toggle` goes there.
            Process& process = GetProcess();
            std::lock_guard<std::recursive_mutex> lock(process.mutex);
            ComPtr<ITfContext> focus = FocusContext();
            Context* field_context = FieldFor(focus.Get());
            if (!field_context) return;
            TsfField field(this, focus.Get());
            field_context->Toggle(field, Info());
            AfterHarness();
        });
    ComPtr<ITfLangBarItemMgr> items;
    if (lang_bar_ && SUCCEEDED(thread_mgr_.As(&items))) items->AddItem(lang_bar_);
    list_.OnPick([this](size_t row) {
        // A click on a row of the menu: move to it, then commit.
        Process& process = GetProcess();
        std::lock_guard<std::recursive_mutex> lock(process.mutex);
        ITfContext* target = Target(nullptr);
        Context* field_context = FindField(target);
        if (!field_context) return;
        TsfField field(this, target);
        field_context->Pick(row, field, Info());
        AfterHarness();
    });

    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    log::Write(L"ActivateEx");
    if (!process.session) log::Write(L"every key passes: %s", Wide(process.error).c_str());
    // The session's mode and profile: every app shares them.
    FollowSession();
    AdviseCompartments(true);
    if (process.session) {
        SetOpen(process.session->mode() == Mode::kHenkan);
        process.session->PublishIdle();
    }
    UpdateIndicator();
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    log::Write(L"Deactivate");
    if (ITfContext* pending = Target(nullptr)) Flush(pending, "harness");
    active_ = false;
    conversion_pinned_ = false;
    refused_ = false;
    orphan_.Reset();
    orphan_context_.Reset();
    list_.Hide();
    list_.OnPick(nullptr);
    AdviseCompartments(false);
    if (thread_mgr_) {
        ComPtr<ITfLangBarItemMgr> items;
        if (lang_bar_ && SUCCEEDED(thread_mgr_.As(&items))) items->RemoveItem(lang_bar_);
        ComPtr<ITfKeystrokeMgr> keystrokes;
        if (SUCCEEDED(thread_mgr_.As(&keystrokes))) keystrokes->UnadviseKeyEventSink(client_id_);
        ComPtr<ITfSource> source;
        if (SUCCEEDED(thread_mgr_.As(&source))) {
            if (thread_sink_cookie_ != TF_INVALID_COOKIE) source->UnadviseSink(thread_sink_cookie_);
            if (profile_sink_cookie_ != TF_INVALID_COOKIE) source->UnadviseSink(profile_sink_cookie_);
        }
    }
    if (lang_bar_) {
        lang_bar_->Detach();
        lang_bar_->Release();
        lang_bar_ = nullptr;
    }
    thread_sink_cookie_ = profile_sink_cookie_ = TF_INVALID_COOKIE;
    composition_.Reset();
    composition_context_.Reset();
    fields_.clear();
    if (process.session) process.session->PublishIdle();
    thread_mgr_.Reset();
    client_id_ = TF_CLIENTID_NULL;
    return S_OK;
}

// --- the fields

Context* TextService::FindField(ITfContext* context) {
    ComPtr<IUnknown> identity;
    if (context && FAILED(context->QueryInterface(IID_PPV_ARGS(&identity)))) return nullptr;
    for (FieldEntry& entry : fields_) {
        if (entry.identity == identity) {
            entry.used = GetTickCount64();
            return entry.context.get();
        }
    }
    return nullptr;
}

Context* TextService::FieldFor(ITfContext* context) {
    if (Context* found = FindField(context)) return found;
    Process& process = GetProcess();
    if (!process.session || !active_) return nullptr;
    ComPtr<IUnknown> identity;
    if (context && FAILED(context->QueryInterface(IID_PPV_ARGS(&identity)))) return nullptr;
    // One engine per text field. A thread that has seen very many fields lets the one it used
    // longest ago go, unless something is staged there.
    if (fields_.size() >= kMaxFields) {
        auto oldest = fields_.end();
        for (auto it = fields_.begin(); it != fields_.end(); ++it) {
            if (it->context->composing()) continue;
            if (oldest == fields_.end() || it->used < oldest->used) oldest = it;
        }
        if (oldest != fields_.end()) fields_.erase(oldest);
    }
    auto made = std::make_unique<Context>(*process.session);
    if (!made->ok()) {
        log::Write(L"bada_new answered NULL");
        return nullptr;
    }
    fields_.push_back({identity, std::move(made), GetTickCount64()});
    return fields_.back().context.get();
}

void TextService::DropField(ITfContext* context) {
    ComPtr<IUnknown> identity;
    if (!context || FAILED(context->QueryInterface(IID_PPV_ARGS(&identity)))) return;
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const FieldEntry& entry) { return entry.identity == identity; }),
                  fields_.end());
}

STDMETHODIMP TextService::OnPopContext(ITfContext* context) {
    // The text field is gone, and its engine with it.
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (composition_context_ && SameObject(composition_context_.Get(), context)) {
        composition_.Reset();
        composition_context_.Reset();
    }
    if (orphan_context_ && SameObject(orphan_context_.Get(), context)) {
        orphan_.Reset();
        orphan_context_.Reset();
    }
    DropField(context);
    return S_OK;
}

ComPtr<ITfContext> TextService::FocusContext() const {
    ComPtr<ITfDocumentMgr> document;
    ComPtr<ITfContext> context;
    if (thread_mgr_ && SUCCEEDED(thread_mgr_->GetFocus(&document)) && document) document->GetTop(&context);
    return context;
}

// --- keys

bool TextService::Composing() {
    for (const FieldEntry& entry : fields_) {
        if (entry.context->composing()) return true;
    }
    return false;
}

ITfContext* TextService::Target(ITfContext* context) const {
    if (context) return context;
    return composition_context_ ? composition_context_.Get() : orphan_context_.Get();
}

ContextInfo TextService::Info() const {
    ContextInfo info;
    info.app = Utf8(ExeName());
    info.secure = private_ || GetProcess().off;
    return info;
}

void TextService::NoContext() {
    // No document to type into (a game window without an input context): the key is the app's, and
    // nothing stays in the engine where it can't be seen.
    static bool logged = false;
    if (!logged) log::Write(L"a key without an input context: passed on");
    logged = true;
    if (Composing()) DropInvisible();
}

void TextService::DropInvisible() {
    orphan_.Reset();
    orphan_context_.Reset();
    // What is staged has nowhere to go: the engines drop it, and nothing is committed.
    for (FieldEntry& entry : fields_) {
        if (entry.context->composing()) entry.context->Forget();
    }
    menu_ = Menu{};
    list_.Hide();
}

void TextService::CheckOrphan() {
    if (!orphan_) return;
    bool inside = true, gone = false;
    ComPtr<ITfRange> orphan = orphan_;
    ComPtr<ITfContext> context = orphan_context_;
    RunReadSessionNow(context.Get(), client_id_, [&](TfEditCookie ec) -> HRESULT {
        BOOL empty = FALSE;
        gone = SUCCEEDED(orphan->IsEmpty(ec, &empty)) && empty;
        inside = CaretInside(ec, context.Get(), orphan.Get());
        return S_OK;
    });
    if (gone || !inside) DropInvisible();
}

void TextService::Settle() {
    if (!refused_) return;
    refused_ = false;
    // The marked text went in as plain text: the engine lets go of it.
    for (FieldEntry& entry : fields_) {
        if (entry.context->composing()) entry.context->Forget();
    }
    menu_ = Menu{};
    list_.Hide();
}

bool TextService::PrivateField(ITfContext* context) {
    // A password-style edit control: ES_PASSWORD, or a password character (Edit and RichEdit only).
    if (HWND focus = GetFocus()) {
        wchar_t name[64] = L"";
        GetClassNameW(focus, name, 64);
        for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
        if (wcsstr(name, L"edit")) {
            if (GetWindowLongW(focus, GWL_STYLE) & ES_PASSWORD) return Private(L"ES_PASSWORD");
            if (SendMessageW(focus, EM_GETPASSWORDCHAR, 0, 0) != 0) return Private(L"password character");
        }
    }
    if (!context) return false;
    // The app turned the keyboard off for this field (password boxes through the IMM layer).
    for (const GUID* guid : {&GUID_COMPARTMENT_KEYBOARD_DISABLED, &GUID_COMPARTMENT_EMPTYCONTEXT}) {
        ComPtr<ITfCompartment> compartment;
        if (SUCCEEDED(GetCompartment(context, *guid, &compartment)) && ReadCompartment(compartment.Get()) != 0) {
            return Private(guid == &GUID_COMPARTMENT_KEYBOARD_DISABLED ? L"keyboard disabled" : L"empty context");
        }
    }
    // A password or PIN by its input scope (browsers' password inputs, XAML's PasswordBox).
    bool secret = false;
    RunReadSessionNow(context, client_id_, [&](TfEditCookie ec) -> HRESULT {
        ComPtr<ITfProperty> property;
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        if (FAILED(context->GetProperty(kPropInputScope, &property)) ||
            FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || !fetched ||
            !selection.range) {
            return S_OK;
        }
        VARIANT value;
        VariantInit(&value);
        ComPtr<ITfInputScope> input_scope;
        if (SUCCEEDED(property->GetValue(ec, selection.range, &value)) && value.vt == VT_UNKNOWN && value.punkVal &&
            SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&input_scope)))) {
            InputScope* list = nullptr;
            UINT count = 0;
            if (SUCCEEDED(input_scope->GetInputScopes(&list, &count)) && list) {
                for (UINT i = 0; i < count; ++i) {
                    switch (list[i]) {
                        case IS_PASSWORD: case IS_PRIVATE: case IS_NUMERIC_PASSWORD:
                        case IS_NUMERIC_PIN: case IS_ALPHANUMERIC_PIN: case IS_ALPHANUMERIC_PIN_SET:
                            secret = true;
                            break;
                        default:
                            break;
                    }
                }
                CoTaskMemFree(list);
            }
        }
        VariantClear(&value);
        selection.range->Release();
        return S_OK;
    });
    return secret ? Private(L"input scope") : false;
}

bool TextService::Private(const wchar_t* why) {
    if (!private_) log::Write(L"private field (%s): nothing composes, nothing typed is published", why);
    return true;
}

RawKey TextService::MakeKey(WPARAM wparam, LPARAM lparam, bool up) const {
    RawKey key;
    key.vk = static_cast<unsigned>(wparam);
    key.scan = ScanOrMapped(key.vk, static_cast<unsigned>((lparam >> 16) & 0xFF),
                            [](unsigned v) { return static_cast<unsigned>(MapVirtualKeyW(v, MAPVK_VK_TO_VSC)); });
    key.extended = (lparam & (LPARAM{1} << 24)) != 0;
    // Bit 30 says the key was down before this message: a repeat. Not for the Japanese layout's
    // lock-like keys, which Windows never says were released: every press of カタカナ/ひらがな or
    // 英数 after the first comes marked so, and each is a press.
    key.repeat = !up && (lparam & (LPARAM{1} << 30)) != 0 && !ReleasesLate(key.vk);
    key.mods.shift = Down(VK_SHIFT);
    key.mods.ctrl = Down(VK_CONTROL);
    key.mods.alt = Down(VK_MENU);
    key.mods.meta = Down(VK_LWIN) || Down(VK_RWIN);
    // A modifier's own press isn't held with itself: only its other side is.
    const std::string code = CodeOf(key);
    if (code == "ShiftLeft") key.mods.shift = Down(VK_RSHIFT);
    else if (code == "ShiftRight") key.mods.shift = Down(VK_LSHIFT);
    else if (code == "ControlLeft") key.mods.ctrl = Down(VK_RCONTROL);
    else if (code == "ControlRight") key.mods.ctrl = Down(VK_LCONTROL);
    else if (code == "AltLeft") key.mods.alt = Down(VK_RMENU);
    else if (code == "AltRight") key.mods.alt = Down(VK_LMENU);
    else if (code == "MetaLeft") key.mods.meta = Down(VK_RWIN);
    else if (code == "MetaRight") key.mods.meta = Down(VK_LWIN);
    key.caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    if (!up && !IsModifierCode(code)) {
        // What Windows' own layout would type for the key: read only to see whether Windows types
        // what the engine's layout says (the standard, §6.4). Flag 4 leaves the keyboard's state
        // (a dead key waiting) as it is.
        BYTE state[256];
        wchar_t text[8];
        if (GetKeyboardState(state)) {
            const int n = ToUnicodeEx(key.vk, key.scan, state, text, ARRAYSIZE(text), 4, GetKeyboardLayout(0));
            if (n > 0) key.os_text = Utf8(text, static_cast<size_t>(n));
        }
    }
    return key;
}

bool TextService::KeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam) {
    const RawKey key = MakeKey(wparam, lparam, false);
    const std::string code = CodeOf(key);
    if (code.empty()) return false;
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (!process.session) return false;  // the engine can't be used: every key is the app's
    Session& session = *process.session;
    // The table's own keys work with no document to type into: they switch mode for the next one.
    const bool harness_key = IsHarnessKey(code) || IsModifierCode(code);
    ITfContext* target = Target(context);
    if (!target && !harness_key) {
        NoContext();
        return false;
    }
    // The settings folder is looked at with a key, a few times a second at most.
    if (!key.repeat && session.Poll(GetTickCount64())) log::Enable(session.harness().log);
    Settle();
    CheckOrphan();
    if (!Composing() && !harness_key) private_ = PrivateField(target);
    if (code != "CapsLock") {
        // A lock the harness decided has had its chance to be set by now.
        ReconcileLock();
        lock_target_.reset();
    }
    Context* field_context = FieldFor(target);
    if (!field_context) return false;
    TsfField field(this, target);
    const ContextInfo info = Info();
    const bool eaten = field_context->KeyDown(key, field, info);
    // The Japanese layout's lock-like keys send no release when they are let go: the press is
    // the whole key.
    if (ReleasesLate(key.vk)) field_context->KeyUp(key, field, info);
    if (field.invisible()) DropInvisible();
    AfterHarness();
    if (ReleasesLate(key.vk)) PinConversion(true);
    // The table's own keys, for the log: never a key that types.
    if (harness_key && !IsModifierCode(code)) {
        log::Write(L"key %hs: vk %#x scan %#x%s, %hs -> %hs", code.c_str(), key.vk, key.scan,
                   (lparam & (LPARAM{1} << 30)) ? L" (marked as held)" : L"", eaten ? "eaten" : "passed",
                   session.ModeText().c_str());
    }
    return eaten;
}

bool TextService::KeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam) {
    const RawKey key = MakeKey(wparam, lparam, true);
    const std::string code = CodeOf(key);
    if (code.empty()) return false;
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (!process.session) return false;
    ITfContext* target = Target(context);
    // A release only matters to a field that saw the press.
    Context* field_context = FindField(target);
    if (!field_context) return false;
    TsfField field(this, target);
    const bool eaten = field_context->KeyUp(key, field, Info());
    if (field.invisible()) DropInvisible();
    AfterHarness();
    return eaten;
}

void TextService::AfterHarness() {
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (!process.session) return;
    Session& session = *process.session;
    // This process switched mode or profile: the others follow.
    if (session.switches() != process.written_switches) {
        process.written_switches = session.switches();
        WriteSession();
    }
    SetOpen(session.mode() == Mode::kHenkan);
    if (const std::optional<bool> wanted = session.TakeLockWanted()) lock_target_ = wanted;
    ReconcileLock();
    UpdateIndicator();
    ShowList();
}

void TextService::ReconcileLock() {
    if (!lock_target_) return;
    if (((GetKeyState(VK_CAPITAL) & 1) != 0) == *lock_target_) return;
    // Windows flips the lock on a press, and only on one that starts with the key up: while the
    // user still holds Caps Lock, an injected press would do nothing. It is tried again when the
    // key is released.
    if (GetAsyncKeyState(VK_CAPITAL) & 0x8000) return;
    // Our own press is on its way.
    if (lock_injected_tick_ && GetTickCount64() - lock_injected_tick_ < kLockEcho) return;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_CAPITAL;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    in[0].ki.dwExtraInfo = in[1].ki.dwExtraInfo = static_cast<ULONG_PTR>(kInjectedMark);
    lock_injected_tick_ = GetTickCount64();
    if (SendInput(2, in, sizeof(INPUT)) != 2) {
        lock_injected_tick_ = 0;
        log::Write(L"Caps Lock: SendInput failed (%lu)", GetLastError());
    }
}

void TextService::ForwardKey(const RawKey& key) {
    // A key the harness held back and now lets go: pressed and released again, marked as ours so
    // that the key sink lets it through.
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    for (INPUT& i : in) {
        if (key.scan) {
            i.ki.wScan = static_cast<WORD>(key.scan);
            i.ki.dwFlags = KEYEVENTF_SCANCODE | (key.extended ? KEYEVENTF_EXTENDEDKEY : 0);
        } else {
            i.ki.wVk = static_cast<WORD>(key.vk);
        }
        i.ki.dwExtraInfo = static_cast<ULONG_PTR>(kInjectedMark);
    }
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2) log::Write(L"a key handed on late: SendInput failed (%lu)", GetLastError());
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
    tested_ = {};
    tested_up_ = {};
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (!process.session || !active_) return S_OK;
    if (foreground) {
        // The keys come here again: the settings, the session's mode, and the state.
        FollowSession();
        ComPtr<ITfContext> focus = FocusContext();
        if (!Composing()) private_ = focus ? PrivateField(focus.Get()) : false;
        if (Context* field_context = focus ? FieldFor(focus.Get()) : nullptr) {
            TsfField field(this, focus.Get());
            field_context->FocusIn(field, Info());
            if (field.invisible()) DropInvisible();
        }
        log::Enable(process.session->harness().log);
        AfterHarness();
    } else {
        // Another program has the keyboard. What is marked stays as it shows, as on Windows.
        list_.Hide();
        process.session->PublishIdle();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
    // The harness runs here, not in OnKeyDown: a key reported as eaten here never reaches the app,
    // even if OnKeyDown then hands it back (jis-moonshot measured Enter, the arrows and Tab lost).
    // So a key the engine doesn't consume is committed before and reported as not eaten; OnKeyDown,
    // which follows for eaten keys, takes the result from here.
    *eaten = FALSE;
    if (Injected()) {
        if (wparam == VK_CAPITAL) lock_injected_tick_ = 0;
        return S_OK;
    }
    // The same key-down tested again (a message loop that peeks before it takes): the harness
    // already ran for it.
    const LONG time = GetMessageTime();
    const ULONGLONG tick = GetTickCount64();
    if (tested_.valid &&
        SameKeyDown(tested_.wparam, tested_.lparam, tested_.time, tested_.tick, wparam, lparam, time, tick)) {
        *eaten = tested_.eaten;
        return S_OK;
    }
    tested_ = {};
    *eaten = KeyDown(context, wparam, lparam) ? TRUE : FALSE;
    tested_ = {wparam, lparam, time, tick, *eaten, true, true};
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
    *eaten = FALSE;
    if (Injected()) return S_OK;
    // The same key, already handled by OnTestKeyDown. Its lParam comes without the repeat count
    // here (measured), so the scan code and the extended flag are compared. Apps going through the
    // IMM layer call OnKeyDown even for keys OnTestKeyDown didn't eat (measured: Enter).
    constexpr LPARAM kScanAndExtended = 0x01FF0000;
    if (tested_.pending && tested_.wparam == wparam &&
        (tested_.lparam & kScanAndExtended) == (lparam & kScanAndExtended)) {
        tested_.pending = false;
        *eaten = tested_.eaten;
        return S_OK;
    }
    tested_ = {};
    *eaten = KeyDown(context, wparam, lparam) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
    *eaten = FALSE;
    if (tested_.wparam == wparam) tested_ = {};
    if (Injected()) return S_OK;
    // A release is one event too: a key of a pair runs when it is let go (the standard, §9.3).
    const LONG time = GetMessageTime();
    if (tested_up_.valid && tested_up_.wparam == wparam && tested_up_.lparam == lparam && tested_up_.time == time) {
        *eaten = tested_up_.eaten;
        return S_OK;
    }
    *eaten = KeyUp(context, wparam, lparam) ? TRUE : FALSE;
    tested_up_ = {wparam, lparam, time, 0, *eaten, true, true};
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
    *eaten = FALSE;
    if (tested_.wparam == wparam) tested_ = {};
    if (Injected()) return S_OK;
    constexpr LPARAM kScanAndExtended = 0x01FF0000;
    if (tested_up_.pending && tested_up_.wparam == wparam &&
        (tested_up_.lparam & kScanAndExtended) == (lparam & kScanAndExtended)) {
        // The key-up of a key-down the app never saw stays away from it too.
        *eaten = tested_up_.eaten;
        tested_up_ = {};
        return S_OK;
    }
    tested_up_ = {};
    *eaten = KeyUp(context, wparam, lparam) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    *eaten = FALSE;
    return S_OK;
}

// --- text out

TextService::Marked TextService::MarkedOf(const Preedit& preedit) const {
    Marked marked;
    marked.text = Wide(preedit.text);
    // Positions are counted in Unicode scalar values (the standard, §3.4); TSF counts UTF-16.
    auto units = [&](size_t from, size_t count) {
        return static_cast<LONG>(Wide(preedit.text.substr(ByteOffset(preedit.text, from),
                                                          ByteOffset(preedit.text, from + count) -
                                                              ByteOffset(preedit.text, from)))
                                     .size());
    };
    marked.caret = units(0, preedit.caret);
    size_t covered = 0;
    for (const Segment& s : preedit.segments) covered += s.len;
    if (preedit.segments.empty() || covered != Scalars(preedit.text)) {
        // No segments: the whole of it is still being read.
        marked.parts.push_back({static_cast<LONG>(marked.text.size()), live_attribute_});
        return marked;
    }
    size_t at = 0;
    for (const Segment& s : preedit.segments) {
        const TfGuidAtom attribute = s.kind == Segment::Kind::kFixed    ? fixed_attribute_
                                     : s.kind == Segment::Kind::kPicked ? picked_attribute_
                                                                        : live_attribute_;
        marked.parts.push_back({units(at, s.len), attribute});
        at += s.len;
    }
    return marked;
}

Around TextService::ReadAround(ITfContext* context, size_t before, size_t after) {
    Around around;
    if (!context) return around;
    before = std::min(before, kMaxAround);
    after = std::min(after, kMaxAround);
    std::wstring selected, text_before, text_after;
    ComPtr<ITfComposition> composition =
        composition_ && SameObject(composition_context_.Get(), context) ? composition_ : nullptr;
    ComPtr<ITfRange> orphan = orphan_ && SameObject(orphan_context_.Get(), context) ? orphan_ : nullptr;
    RunReadSessionNow(context, client_id_, [&](TfEditCookie ec) -> HRESULT {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || !fetched ||
            !selection.range) {
            return S_OK;
        }
        ComPtr<ITfRange> range;
        range.Attach(selection.range);
        // With text marked: around that text, and nothing counts as selected (the standard, §5).
        ComPtr<ITfRange> marked;
        if (composition) composition->GetRange(&marked);
        if (!marked && orphan) orphan->Clone(&marked);
        ITfRange* anchor = marked ? marked.Get() : range.Get();
        if (!marked) selected = TextOf(ec, range.Get(), 4096);
        ComPtr<ITfRange> side;
        LONG moved = 0;
        if (before && SUCCEEDED(anchor->Clone(&side)) && side) {
            side->Collapse(ec, TF_ANCHOR_START);
            side->ShiftStart(ec, -static_cast<LONG>(before * 2), &moved, nullptr);
            text_before = TextOf(ec, side.Get(), before * 2);
        }
        side.Reset();
        if (after && SUCCEEDED(anchor->Clone(&side)) && side) {
            side->Collapse(ec, TF_ANCHOR_END);
            side->ShiftEnd(ec, static_cast<LONG>(after * 2), &moved, nullptr);
            text_after = TextOf(ec, side.Get(), after * 2);
        }
        return S_OK;
    });
    around.selection = Utf8(selected);
    around.before = Utf8(LastScalars(text_before, before));
    around.after = Utf8(FirstScalars(text_after, after));
    return around;
}

void TextService::ShowList() {
    if (!menu_.rows.empty()) {
        list_.Show(menu_, anchor_);
    } else if (!notice_.empty()) {
        list_.ShowNotice(notice_, CaretRect());
    } else {
        list_.Hide();
    }
}

void TextService::Flush(ITfContext* context, const char* cause) {
    Settle();
    Context* field_context = FindField(context);
    if (!field_context) return;
    TsfField field(this, context);
    field_context->Flush(field, cause);
    AfterHarness();
}

// --- the session's mode

std::optional<Shared> TextService::ReadSession() {
    ComPtr<ITfCompartmentMgr> global;
    ComPtr<ITfCompartment> compartment;
    if (!thread_mgr_ || FAILED(thread_mgr_->GetGlobalCompartment(&global)) ||
        FAILED(global->GetCompartment(kSessionCompartment, &compartment))) {
        return std::nullopt;
    }
    VARIANT v;
    VariantInit(&v);
    if (FAILED(compartment->GetValue(&v))) return std::nullopt;
    // Text: the mode, and the profile by its id (src/core/harness.h, WriteShared). Never set in
    // this session: empty.
    std::optional<Shared> shared;
    if (v.vt == VT_BSTR && v.bstrVal) shared = ReadShared(Utf8(v.bstrVal, SysStringLen(v.bstrVal)));
    VariantClear(&v);
    return shared;
}

void TextService::WriteSession() {
    Process& process = GetProcess();
    ComPtr<ITfCompartmentMgr> global;
    ComPtr<ITfCompartment> compartment;
    if (!process.session || !thread_mgr_ || FAILED(thread_mgr_->GetGlobalCompartment(&global)) ||
        FAILED(global->GetCompartment(kSessionCompartment, &compartment))) {
        return;
    }
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(Wide(WriteShared(process.session->shared())).c_str());
    if (v.bstrVal) {
        const HRESULT hr = compartment->SetValue(client_id_, &v);
        if (FAILED(hr)) log::Write(L"the session's compartment wasn't written %#lx", hr);
    }
    VariantClear(&v);
}

void TextService::FollowSession() {
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (!process.session) return;
    // Another thread or app switched mode or profile: every field here follows at its next event,
    // committing what it has staged (a `select`).
    if (const std::optional<Shared> shared = ReadSession()) process.session->Adopt(*shared);
    SetOpen(process.session->mode() == Mode::kHenkan);
    UpdateIndicator();
}

void TextService::SetOpen(bool open) {
    // The thread's open/close compartment is what the input indicator and apps read: open in henkan.
    ComPtr<ITfCompartment> openclose;
    if (thread_mgr_ && SUCCEEDED(GetCompartment(thread_mgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openclose)) &&
        (ReadCompartment(openclose.Get()) != 0) != open) {
        WriteCompartment(openclose.Get(), client_id_, open ? 1 : 0);
    }
    PinConversion(false);
}

// Windows' Japanese layout (kbd106) takes カタカナ/ひらがな and 英数 as keys that set a mode. It
// reports a press of one only when the mode it knows isn't that key's already, and the mode it
// knows is what the input method says: open or closed, and the conversion mode. bada has no such
// mode: its profiles are its own. So it says "alphanumeric" whether open or not, and
// カタカナ/ひらがな is a press every time. (Open with the default conversion mode, which is kana,
// the key did nothing in henkan.) After such a key Windows believes the mode is the key's, so the
// conversion mode is told again, with the full-shape bit, which means nothing here, flipped so
// that it is a change.
void TextService::PinConversion(bool again) {
    ComPtr<ITfCompartment> conversion;
    if (!thread_mgr_ ||
        FAILED(GetCompartment(thread_mgr_.Get(), GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &conversion))) {
        return;
    }
    const LONG now = ReadCompartment(conversion.Get());
    LONG want = now & TF_CONVERSIONMODE_FULLSHAPE;
    if (again) want ^= TF_CONVERSIONMODE_FULLSHAPE;
    if (conversion_pinned_ && want == now) return;
    conversion_pinned_ = true;
    WriteCompartment(conversion.Get(), client_id_, want);
}

void TextService::AdviseCompartments(bool on) {
    if (!thread_mgr_) return;
    ComPtr<ITfCompartmentMgr> global;
    ComPtr<ITfCompartment> session, openclose;
    if (SUCCEEDED(thread_mgr_->GetGlobalCompartment(&global))) global->GetCompartment(kSessionCompartment, &session);
    GetCompartment(thread_mgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openclose);
    struct Pair {
        ITfCompartment* compartment;
        DWORD* cookie;
    };
    for (Pair p : {Pair{session.Get(), &session_sink_cookie_}, Pair{openclose.Get(), &openclose_sink_cookie_}}) {
        ComPtr<ITfSource> source;
        if (!p.compartment || FAILED(p.compartment->QueryInterface(IID_PPV_ARGS(&source)))) continue;
        if (on && *p.cookie == TF_INVALID_COOKIE) {
            source->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(this), p.cookie);
        } else if (!on && *p.cookie != TF_INVALID_COOKIE) {
            source->UnadviseSink(*p.cookie);
            *p.cookie = TF_INVALID_COOKIE;
        }
    }
}

STDMETHODIMP TextService::OnChange(REFGUID guid) {
    if (IsEqualGUID(guid, kSessionCompartment)) {
        FollowSession();
    } else if (IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) && thread_mgr_) {
        // The open/close compartment only mirrors the mode. Windows closes it when a profile
        // activates, so a change from elsewhere is put back: the key table, the indicator and the
        // menu are what switch.
        Process& process = GetProcess();
        std::lock_guard<std::recursive_mutex> lock(process.mutex);
        if (!process.session) return S_OK;
        ComPtr<ITfCompartment> openclose;
        const bool open = process.session->mode() == Mode::kHenkan;
        if (SUCCEEDED(GetCompartment(thread_mgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openclose)) &&
            (ReadCompartment(openclose.Get()) != 0) != open) {
            WriteCompartment(openclose.Get(), client_id_, open ? 1 : 0);
        }
    }
    return S_OK;
}

void TextService::UpdateIndicator() {
    if (!lang_bar_) return;
    Process& process = GetProcess();
    if (!process.session) {
        lang_bar_->SetState(L"!", L"bada: the engine can't be used");
        return;
    }
    lang_bar_->SetState(Wide(process.session->Label()), L"bada: " + Wide(process.session->ModeText()));
}

// --- the input menu

void TextService::InitMenu(ITfMenu* menu) {
    auto add = [](ITfMenu* m, UINT id, const std::wstring& text, bool checked, bool enabled = true) {
        DWORD flags = checked ? TF_LBMENUF_CHECKED : 0;
        if (!enabled) flags |= TF_LBMENUF_GRAYED;
        m->AddMenuItem(id, flags, nullptr, nullptr, text.c_str(), static_cast<ULONG>(text.size()), nullptr);
    };
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    menu_profiles_.clear();
    if (process.session) {
        const Session& session = *process.session;
        const bool henkan = session.mode() == Mode::kHenkan;
        add(menu, kMenuMuhenkan, L"Muhenkan (無変換)", !henkan);
        // The henkan profiles, by name, in the settings' order (the standard, §8.1).
        for (const Profile& profile : session.profiles()) {
            add(menu, kMenuProfileFirst + static_cast<UINT>(menu_profiles_.size()), Wide(profile.name),
                henkan && session.profile() == profile.id);
            menu_profiles_.push_back(profile.id);
        }
        if (menu_profiles_.empty()) add(menu, kMenuNoProfile, L"No henkan profile yet (see the README)", false, false);
    } else {
        add(menu, kMenuNoProfile, L"The engine can't be used", false, false);
    }
    menu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, L"", 0, nullptr);
    add(menu, kMenuAbout, L"bada " + Wide(BADA_VERSION), false, false);
}

void TextService::OnMenu(UINT id) {
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    const bool profile = id >= kMenuProfileFirst && id - kMenuProfileFirst < menu_profiles_.size();
    if (id != kMenuMuhenkan && !profile) return;
    ComPtr<ITfContext> focus = FocusContext();
    Context* field_context = FieldFor(focus.Get());
    if (!field_context) return;
    TsfField field(this, focus.Get());
    if (profile) {
        field_context->Select(Mode::kHenkan, menu_profiles_[id - kMenuProfileFirst], field, Info());
    } else {
        field_context->Select(Mode::kMuhenkan, {}, field, Info());
    }
    AfterHarness();
}

// --- focus and profiles

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
    tested_ = {};
    tested_up_ = {};
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    // Focus moved to another document: what was staged is committed where it was.
    if (ITfContext* pending = Target(nullptr)) {
        Flush(pending, "blur");
    } else if (Composing()) {
        DropInvisible();  // text that never reached a document doesn't follow focus
    }
    menu_ = Menu{};
    notice_.clear();
    list_.Hide();
    ComPtr<ITfContext> top;
    if (focus) focus->GetTop(&top);
    // Whether the new field is a password field, for what is published.
    private_ = top ? PrivateField(top.Get()) : false;
    if (!process.session || !active_) return S_OK;
    FollowSession();
    if (Context* field_context = top ? FieldFor(top.Get()) : nullptr) {
        TsfField field(this, top.Get());
        field_context->FocusIn(field, Info());
    } else {
        process.session->PublishIdle();
    }
    log::Enable(process.session->harness().log);
    AfterHarness();
    return S_OK;
}

STDMETHODIMP TextService::OnActivated(REFCLSID clsid, REFGUID, BOOL activated) {
    if (!IsEqualGUID(clsid, kClsid)) return S_OK;
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    if (activated) {
        FollowSession();
    } else {
        if (ITfContext* pending = Target(nullptr)) Flush(pending, "harness");
        if (process.session) process.session->PublishIdle();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ec, ITfComposition* composition) {
    if (!composition_ || composition != composition_.Get()) return S_OK;
    Process& process = GetProcess();
    std::lock_guard<std::recursive_mutex> lock(process.mutex);
    const ULONGLONG since = GetTickCount64() - updated_tick_;
    static bool logged = false;
    if (!logged) {
        log::Write(L"composition terminated %s%llu ms after update", in_update_ ? L"during the update, " : L"", since);
    }
    logged = true;
    ComPtr<ITfRange> range;
    if (FAILED(composition->GetRange(&range))) range.Reset();
    if (range && composition_context_ && CaretInside(ec, composition_context_.Get(), range.Get())) {
        CaretToEnd(ec, composition_context_.Get(), range.Get());
    }
    Context* field_context = FindField(composition_context_.Get());
    // Ended by the app right after our own update, not by the user: the text stays as plain text,
    // the engine keeps what it has staged, and the next update replaces it. Not through the IMM
    // layer (a transitory context): the text leaves the document there (measured).
    TF_STATUS status = {};
    const bool transitory = composition_context_ && SUCCEEDED(composition_context_->GetStatus(&status)) &&
                            (status.dwStaticFlags & TF_SS_TRANSITORY) != 0;
    if (kKeepTextWithoutComposition && range && !transitory && field_context && field_context->composing() &&
        (in_update_ || since < kOwnTermination)) {
        ComPtr<ITfRange> orphan;
        if (SUCCEEDED(range->Clone(&orphan)) && orphan) {
            orphan->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
            orphan_ = orphan;
            orphan_context_ = composition_context_;
            composition_.Reset();
            composition_context_.Reset();
            return S_OK;
        }
    }
    // The app ended the composition (a click in the text, an edit): what is staged is committed
    // where it shows, before the field changes (the standard, §6.3). The edit session is this one.
    if (field_context && active_) {
        const std::string shown = field_context->last().preedit.text;
        CaptureField capture;
        field_context->Flush(capture, "click");
        if (range && capture.committed && capture.commit != shown) {
            const std::wstring text = Wide(capture.commit);
            range->SetText(ec, 0, text.data(), static_cast<LONG>(text.size()));
        }
        ComPtr<ITfProperty> attribute;
        if (range && composition_context_ && SUCCEEDED(composition_context_->GetProperty(GUID_PROP_ATTRIBUTE, &attribute))) {
            attribute->Clear(ec, range.Get());
        }
        field_context->Publish(Info());
    }
    composition_.Reset();
    composition_context_.Reset();
    orphan_.Reset();
    orphan_context_.Reset();
    menu_ = Menu{};
    list_.Hide();
    return S_OK;
}

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** infos) {
    if (!infos) return E_INVALIDARG;
    *infos = NewDisplayAttributeEnum();
    return *infos ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
    if (!info) return E_INVALIDARG;
    *info = NewDisplayAttributeInfo(guid);
    return *info ? S_OK : E_INVALIDARG;
}

// --- the composition

bool TextService::UpdateText(ITfContext* context, const std::wstring& commit, const Marked& marked,
                             bool replace_selection) {
    if (!context) return true;
    // A composition left in another document (focus moved) ends there as it is.
    if (composition_ && composition_context_ && !SameObject(composition_context_.Get(), context)) {
        ComPtr<ITfComposition> old = composition_;
        RunEditSession(composition_context_.Get(), client_id_, [old](TfEditCookie ec) { return old->EndComposition(ec); });
        composition_.Reset();
        composition_context_.Reset();
    }
    if (orphan_ && !SameObject(orphan_context_.Get(), context)) {
        orphan_.Reset();
        orphan_context_.Reset();
    }
    if (!composition_ && !orphan_ && commit.empty() && marked.text.empty()) return true;
    // When TSF can't run the session at once it runs after this returns, maybe after Deactivate: it
    // holds the service, the context and the composition it was meant for, and copies of the text,
    // and never calls the engine.
    ComPtr<TextService> self(this);
    ComPtr<ITfContext> context_ref = context;
    ComPtr<ITfComposition> expected = composition_;
    auto edit = [self, context_ref, expected, commit, marked, replace_selection](TfEditCookie ec) -> HRESULT {
        return self->EditText(ec, context_ref.Get(), expected.Get(), commit, marked, replace_selection);
    };
    bool async = false;
    in_update_ = true;
    const HRESULT hr = RunEditSession(context, client_id_, edit, &async);
    in_update_ = false;
    updated_tick_ = GetTickCount64();
    if (FAILED(hr)) log::Write(L"edit session failed %#lx", hr);
    if (async) {
        static bool logged = false;
        if (!logged) log::Write(L"edit session queued (async): the text lands after the key");
        logged = true;
        return false;
    }
    return true;
}

HRESULT TextService::EditText(TfEditCookie ec, ITfContext* context, ITfComposition* expected,
                              const std::wstring& commit, const Marked& marked, bool replace_selection) {
    const std::wstring& preedit = marked.text;
    // Deactivated since this was requested: end what is shown with its commit, start nothing.
    if (!active_) {
        ComPtr<ITfRange> range;
        if (expected && SUCCEEDED(expected->GetRange(&range)) && range && !commit.empty()) {
            range->SetText(ec, 0, commit.data(), static_cast<LONG>(commit.size()));
        }
        if (expected) expected->EndComposition(ec);
        return S_OK;
    }
    if (expected && composition_.Get() != expected) return S_OK;  // ended in between: left as shown
    ComPtr<ITfProperty> attribute;
    if (FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, &attribute))) attribute.Reset();
    // Apps that reach TSF through the IMM compatibility layer (a transitory context) end a
    // composition with a plain caret at once: the composition is selected as an interim character
    // there, set on the first character and kept by TSF as it grows.
    TF_STATUS status = {};
    const bool transitory = SUCCEEDED(context->GetStatus(&status)) && (status.dwStaticFlags & TF_SS_TRANSITORY) != 0;
    // The caret at the reply's place in the marked text: most often its end.
    auto caret = [&](ITfRange* range) -> HRESULT {
        if (marked.caret >= static_cast<LONG>(preedit.size())) return CaretToEnd(ec, context, range);
        ComPtr<ITfRange> at;
        if (FAILED(range->Clone(&at)) || !at) return CaretToEnd(ec, context, range);
        LONG moved = 0;
        at->Collapse(ec, TF_ANCHOR_START);
        at->ShiftEnd(ec, marked.caret, &moved, nullptr);
        at->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION selection = {};
        selection.range = at.Get();
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        return context->SetSelection(ec, 1, &selection);
    };
    auto show = [&](ITfRange* range) -> HRESULT {
        if (!transitory) return caret(range);
        TF_SELECTION selection = {};
        selection.range = range;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = TRUE;
        if (SUCCEEDED(context->SetSelection(ec, 1, &selection))) return S_OK;
        TF_SELECTION now = {};
        ULONG fetched = 0;
        bool kept = false;
        if (SUCCEEDED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &now, &fetched)) && fetched && now.range) {
            LONG start = 1, end = 1;
            kept = now.style.fInterimChar && SUCCEEDED(now.range->CompareStart(ec, range, TF_ANCHOR_START, &start)) &&
                   SUCCEEDED(now.range->CompareEnd(ec, range, TF_ANCHOR_END, &end)) && start == 0 && end == 0;
            now.range->Release();
        }
        if (kept) return S_OK;
        return CaretToEnd(ec, context, range);
    };
    auto place = [&](ITfRange* range) {
        RECT rect = {};
        BOOL clipped = FALSE;
        ComPtr<ITfContextView> view;
        if (FAILED(context->GetActiveView(&view)) || FAILED(view->GetTextExt(ec, range, &rect, &clipped)) ||
            IsRectEmpty(&rect)) {
            rect = CaretRect();
        }
        anchor_ = rect;
    };
    // Each part of the marked text gets the display attribute of its kind (the standard, §6.1).
    auto mark = [&](ITfRange* range) {
        if (attribute) {
            LONG offset = 0;
            for (const Marked::Part& part : marked.parts) {
                ComPtr<ITfRange> piece;
                if (part.length > 0 && part.attribute != TF_INVALID_GUIDATOM && SUCCEEDED(range->Clone(&piece)) && piece) {
                    LONG moved = 0;
                    piece->Collapse(ec, TF_ANCHOR_START);
                    piece->ShiftEnd(ec, offset + part.length, &moved, nullptr);
                    piece->ShiftStart(ec, offset, &moved, nullptr);
                    VARIANT v;
                    v.vt = VT_I4;
                    v.lVal = static_cast<LONG>(part.attribute);
                    attribute->SetValue(ec, piece.Get(), &v);
                }
                offset += part.length;
            }
        }
        place(range);
    };
    auto set_text = [&](ITfRange* range, const std::wstring& text) {
        const HRESULT hr = range->SetText(ec, 0, text.data(), static_cast<LONG>(text.size()));
        if (FAILED(hr)) log::Write(L"SetText on the composition failed %#lx", hr);
        return hr;
    };
    ComPtr<ITfInsertAtSelection> insert;
    ComPtr<ITfContextComposition> compositions;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&insert))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&compositions)))) {
        insert.Reset();
        compositions.Reset();
    }
    auto compose = [&](ITfRange* over) -> bool {
        ComPtr<ITfComposition> started;
        ComPtr<ITfRange> range;
        HRESULT hr = compositions ? compositions->StartComposition(ec, over, this, &started) : E_NOINTERFACE;
        if (SUCCEEDED(hr) && started && (FAILED(started->GetRange(&range)) || !range)) {
            started->EndComposition(ec);
            started.Reset();
            hr = E_FAIL;
        }
        if (FAILED(hr) || !started) {
            static bool logged = false;
            if (!logged) log::Write(L"composition refused hr=%#lx", hr);
            logged = true;
            return false;
        }
        composition_ = started;
        composition_context_ = context;
        set_text(range.Get(), preedit);
        mark(range.Get());
        show(range.Get());
        return true;
    };

    if (replace_selection && !commit.empty()) {
        // The commit replaces the selection, even with text marked (the standard, §6.3). The engine
        // writes this only with nothing staged, so what is marked, if anything, ends as it shows.
        if (composition_) {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition_->GetRange(&range)) && range && attribute) attribute->Clear(ec, range.Get());
            composition_->EndComposition(ec);
            composition_.Reset();
            composition_context_.Reset();
        }
        orphan_.Reset();
        orphan_context_.Reset();
        InsertAtSelection(ec, context, commit);
    } else if (orphan_) {
        ComPtr<ITfRange> orphan = orphan_;
        orphan_.Reset();
        orphan_context_.Reset();
        if (!commit.empty() || preedit.empty()) {
            set_text(orphan.Get(), commit);
            CaretToEnd(ec, context, orphan.Get());
        } else {
            if (compose(orphan.Get())) return S_OK;
            if (FAILED(set_text(orphan.Get(), preedit))) {
                refused_ = true;
                return E_FAIL;
            }
            orphan_ = orphan;
            orphan_context_ = context;
            place(orphan.Get());
            return CaretToEnd(ec, context, orphan.Get());
        }
    } else if (composition_) {
        ComPtr<ITfRange> range;
        if (FAILED(composition_->GetRange(&range)) || !range) {
            log::Write(L"the composition has no range: left as shown");
            composition_.Reset();
            composition_context_.Reset();
        } else if (commit.empty() && !preedit.empty()) {  // still composing: just update it
            set_text(range.Get(), preedit);
            mark(range.Get());
            return show(range.Get());
        } else {
            // The composition ends; its text becomes `commit`. The caret goes after it only if it
            // is still inside: a key the app already handled may have moved it.
            const bool inside = CaretInside(ec, context, range.Get());
            if (attribute) attribute->Clear(ec, range.Get());
            set_text(range.Get(), commit);
            if (inside) CaretToEnd(ec, context, range.Get());
            composition_->EndComposition(ec);
            composition_.Reset();
            composition_context_.Reset();
        }
    } else if (!commit.empty()) {
        InsertAtSelection(ec, context, commit);
    }
    if (preedit.empty()) return S_OK;

    ComPtr<ITfRange> at;
    if (!insert || FAILED(insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &at)) || !at) {
        refused_ = true;
        return E_FAIL;
    }
    if (compose(at.Get())) return S_OK;
    // The app refused a composition: the marked text goes in as plain text, replaced at the next
    // update.
    ComPtr<ITfRange> plain;
    if (kKeepTextWithoutComposition &&
        SUCCEEDED(insert->InsertTextAtSelection(ec, 0, preedit.data(), static_cast<LONG>(preedit.size()), &plain)) &&
        plain) {
        plain->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_BACKWARD);
        orphan_ = plain;
        orphan_context_ = context;
        place(plain.Get());
        return CaretToEnd(ec, context, plain.Get());
    }
    refused_ = true;
    return plain ? CaretToEnd(ec, context, plain.Get()) : InsertAtSelection(ec, context, preedit);
}

}  // namespace bada::tip
