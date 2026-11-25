// bada_tsf_host: the text service, end to end, with no window and no injected key.
//
// The test is the app. It makes a TSF thread manager and a document on a text store of its own
// (tests/text_store.h), has the text service activated on it, and types: every key goes in through
// TSF's keystroke manager, as TSF hands an app's keys to a text service, and the text comes out of
// the store, where the text service's edit sessions put it.
//
//   bada_tsf_host --dll <bada-tsf.dll>   loads the DLL itself: nothing needs to be registered
//   bada_tsf_host --registered           the registered text service: activated by TSF from its
//                                        language profile, or else made by its CLSID
//   … --own-desktop                      on a new desktop of its own (never shown), with --out
//
// TSF works only in an interactive logon, so from a logon that isn't one (SSH, a service) the test
// is run in the interactive user's session. tools\test.ps1 -InSession does that with a scheduled
// task (an interactive logon type, so no password), and this program's --own-desktop puts its
// windows (the menu, COM's own) on a desktop nobody is viewing: nothing shows on the user's
// screen, nothing is typed into their apps. The settings folder is a new one in the temporary
// folder (%BADA_CONFIG_DIR%): the user's own %APPDATA%\bada is never read or written.
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "guids.h"
#include "json.h"
#include "state_shared.h"
#include "text_store.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace json = bada::json;
using bada::tip::kClsid;
using bada::tip::kProfile;

namespace {

int g_checks = 0, g_failures = 0;
// The desktop this process moved to, as CreateProcess names one; "" when it stayed where it was.
std::wstring g_desktop;

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Text for the console, which may not show UTF-8: what isn't ASCII as \u escapes.
std::string Shown(const std::string& utf8) {
    std::string out;
    for (const wchar_t c : Wide(utf8)) {
        if (c == L'\n') {
            out += "\\n";
        } else if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\u%04X", static_cast<unsigned>(c));
            out += escape;
        }
    }
    return out;
}

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what + (got == want ? "" : ": got \"" + Shown(got) + "\", want \"" + Shown(want) + "\""));
}

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
    fs::rename(tmp, path);
}

void Pump(DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ms) Sleep(5);
    } while (GetTickCount64() < until);
}

// A key by its W3C code: its scan code, and its virtual key where the layout doesn't say.
struct KeySpec {
    const char* code;
    WORD scan;
    bool extended;
    WORD vk;  // 0: what Windows' layout has at that position
};
constexpr KeySpec kKeys[] = {
    {"Digit1", 0x02}, {"Digit2", 0x03}, {"Digit3", 0x04}, {"Digit4", 0x05}, {"Digit5", 0x06},
    {"Digit6", 0x07}, {"Digit7", 0x08}, {"Digit8", 0x09}, {"Digit9", 0x0A}, {"Digit0", 0x0B},
    {"Minus", 0x0C}, {"Equal", 0x0D}, {"KeyQ", 0x10}, {"KeyW", 0x11}, {"KeyE", 0x12}, {"KeyR", 0x13},
    {"KeyT", 0x14}, {"KeyY", 0x15}, {"KeyU", 0x16}, {"KeyI", 0x17}, {"KeyO", 0x18}, {"KeyP", 0x19},
    {"BracketLeft", 0x1A}, {"BracketRight", 0x1B}, {"KeyA", 0x1E}, {"KeyS", 0x1F}, {"KeyD", 0x20},
    {"KeyF", 0x21}, {"KeyG", 0x22}, {"KeyH", 0x23}, {"KeyJ", 0x24}, {"KeyK", 0x25}, {"KeyL", 0x26},
    {"Semicolon", 0x27}, {"Quote", 0x28}, {"Backquote", 0x29}, {"Backslash", 0x2B}, {"KeyZ", 0x2C},
    {"KeyX", 0x2D}, {"KeyC", 0x2E}, {"KeyV", 0x2F}, {"KeyB", 0x30}, {"KeyN", 0x31}, {"KeyM", 0x32},
    {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35}, {"IntlRo", 0x73}, {"IntlYen", 0x7D},
    {"Space", 0x39, false, VK_SPACE}, {"Enter", 0x1C, false, VK_RETURN}, {"Backspace", 0x0E, false, VK_BACK},
    {"Tab", 0x0F, false, VK_TAB}, {"Escape", 0x01, false, VK_ESCAPE},
    {"ArrowUp", 0x48, true, VK_UP}, {"ArrowDown", 0x50, true, VK_DOWN},
    {"ArrowLeft", 0x4B, true, VK_LEFT}, {"ArrowRight", 0x4D, true, VK_RIGHT},
    {"F7", 0x41, false, VK_F7}, {"F13", 0x64, false, VK_F13},
    {"NonConvert", 0x7B, false, VK_NONCONVERT}, {"Convert", 0x79, false, VK_CONVERT},
    {"KanaMode", 0x70, false, 0xF2}, {"Lang1", 0x72, false, VK_HANGUL}, {"Lang2", 0x71, false, VK_HANJA},
    {"CapsLock", 0x3A, false, VK_CAPITAL},
    {"ShiftLeft", 0x2A, false, VK_SHIFT}, {"ShiftRight", 0x36, false, VK_SHIFT},
    {"ControlLeft", 0x1D, false, VK_CONTROL}, {"AltLeft", 0x38, false, VK_MENU},
};

const KeySpec* Find(const std::string& code) {
    for (const KeySpec& k : kKeys) {
        if (code == k.code) return &k;
    }
    return nullptr;
}

// An edit session of the app's own, to read what the text service left in the document.
class ReadSession final : public ITfEditSession {
public:
    explicit ReadSession(std::function<void(TfEditCookie)> body) : body_(std::move(body)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        body_(ec);
        return S_OK;
    }

private:
    ~ReadSession() = default;
    ULONG refs_ = 1;
    std::function<void(TfEditCookie)> body_;
};

enum class How { kDll, kClsid, kProfile };

// The app: a TSF thread, one document, and the text service on it.
class Host {
public:
    bool Start(How how, const std::wstring& dll, std::string* error);
    void Stop();

    test::TextStore& store() { return *store_.Get(); }
    std::string Text() const { return Utf8(store_->text()); }
    std::string MarkedText() const { return Utf8(store_->MarkedText()); }

    // Types keys: W3C codes separated by spaces. S-, C- and A- before a code hold Shift, Control
    // and Alt for it; D- and U- send only its press or its release; L- its press as Windows reports
    // a lock-like key pressed again (marked as a repeat, and never released).
    void Type(const std::string& keys);
    // One key. Returns whether the text service ate its press.
    bool Press(const std::string& key);
    void SetCapsLock(bool on) { caps_ = on; }
    // A compartment of the thread, as Windows and apps read it: -1 when it holds no number.
    LONG ThreadCompartment(REFGUID guid) {
        ComPtr<ITfCompartmentMgr> compartments;
        ComPtr<ITfCompartment> compartment;
        VARIANT v;
        VariantInit(&v);
        if (FAILED(threads_.As(&compartments)) || FAILED(compartments->GetCompartment(guid, &compartment)) ||
            FAILED(compartment->GetValue(&v)) || v.vt != VT_I4) {
            return -1;
        }
        return v.lVal;
    }
    // A second document, to move focus to; and back.
    void FocusOther(bool secure);
    void FocusMain();
    test::TextStore& other() { return *other_store_.Get(); }

    // What an app does to end the text service's composition itself, as at a click in the text.
    void AppEndsComposition();
    // The display attributes over the document: "live 0+1, fixed 1+2", by the text service's kinds.
    std::string Attributes();
    // The published state, as another program reads it.
    json::Value State() const;
    bool profile_activated() const { return how_ == How::kProfile; }

private:
    bool Down(const KeySpec& key);
    void Up(const KeySpec& key);
    void AppKey(const KeySpec& key, WORD vk);
    WORD VkOf(const KeySpec& key) const;
    void ApplyState();
    LPARAM Param(const KeySpec& key, bool up) const {
        return static_cast<LPARAM>(1 | (static_cast<LPARAM>(key.scan) << 16) | (key.extended ? (LPARAM{1} << 24) : 0) |
                                   (up ? (LPARAM{3} << 30) : 0) | (was_down_ && !up ? (LPARAM{1} << 30) : 0));
    }
    // The next press comes as Windows reports a key it thinks is still down (lParam bit 30).
    bool was_down_ = false;

    How how_ = How::kDll;
    HMODULE module_ = nullptr;
    ComPtr<ITfThreadMgr> threads_;
    ComPtr<ITfKeystrokeMgr> keystrokes_;
    TfClientId app_ = TF_CLIENTID_NULL;
    ComPtr<ITfDocumentMgr> document_, other_document_;
    ComPtr<ITfContext> context_, other_context_;
    ComPtr<test::TextStore> store_, other_store_;
    ComPtr<ITfTextInputProcessorEx> tip_;
    ComPtr<ITfKeyEventSink> sink_;  // the TIP's, for the by-hand paths: keys are fed to it directly
    ComPtr<ITfInputProcessorProfileMgr> profiles_;
    bool other_focused_ = false;
    BYTE state_[256] = {};
    bool caps_ = false;
};

bool Host::Start(How how, const std::wstring& dll, std::string* error) {
    how_ = how;
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&threads_));
    if (FAILED(hr)) {
        char text[96];
        std::snprintf(text, sizeof text, "no TSF thread manager (%#lx)", static_cast<unsigned long>(hr));
        return *error = text, false;
    }
    if (FAILED(threads_->Activate(&app_))) return *error = "ITfThreadMgr::Activate failed", false;
    threads_.As(&keystrokes_);
    auto document = [&](ComPtr<ITfDocumentMgr>* doc, ComPtr<ITfContext>* context, ComPtr<test::TextStore>* store) {
        store->Attach(new test::TextStore());
        TfEditCookie cookie = 0;
        return SUCCEEDED(threads_->CreateDocumentMgr(&*doc)) &&
               SUCCEEDED((*doc)->CreateContext(app_, 0, static_cast<ITextStoreACP*>(store->Get()), &*context, &cookie)) &&
               SUCCEEDED((*doc)->Push(context->Get()));
    };
    if (!document(&document_, &context_, &store_) || !document(&other_document_, &other_context_, &other_store_)) {
        return *error = "the documents weren't made", false;
    }

    if (how == How::kProfile) {
        // As Windows does it when the user picks the keyboard: TSF finds the profile in the
        // registry, makes the text service and activates it. For this process only.
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles_));
        if (SUCCEEDED(hr)) {
            hr = profiles_->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, bada::tip::kLanguage, kClsid, kProfile, nullptr,
                                            TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
        }
        if (FAILED(hr)) {
            char text[96];
            std::snprintf(text, sizeof text, "ActivateProfile answered %#lx", static_cast<unsigned long>(hr));
            return *error = text, false;
        }
    } else {
        ComPtr<IClassFactory> factory;
        if (how == How::kDll) {
            module_ = LoadLibraryW(dll.c_str());
            if (!module_) return *error = "the DLL didn't load (" + std::to_string(GetLastError()) + ")", false;
            using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
            auto get = reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(module_, "DllGetClassObject")));
            hr = get ? get(kClsid, IID_PPV_ARGS(&factory)) : E_FAIL;
            if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&tip_));
        } else {
            hr = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tip_));
        }
        if (FAILED(hr) || !tip_) {
            char text[96];
            std::snprintf(text, sizeof text, "the text service wasn't made (%#lx)", static_cast<unsigned long>(hr));
            return *error = text, false;
        }
        // TSF's part of an activation, by hand: a client id for the class, and ActivateEx. TSF
        // won't route keys to a TIP it didn't activate itself (AdviseKeyEventSink is refused), so
        // the keys are fed to the TIP's key sink directly (Down/Up): the same OnTestKeyDown →
        // harness path TSF would drive, on the real text store, with real edit sessions.
        ComPtr<ITfClientId> ids;
        TfClientId id = TF_CLIENTID_NULL;
        if (FAILED(threads_.As(&ids)) || FAILED(ids->GetClientId(kClsid, &id))) return *error = "no client id", false;
        if (FAILED(tip_->ActivateEx(threads_.Get(), id, 0))) return *error = "ActivateEx failed", false;
        if (FAILED(tip_.As(&sink_))) return *error = "the text service is no key event sink", false;
    }
    threads_->SetFocus(document_.Get());
    Pump(0);
    return true;
}

void Host::Stop() {
    sink_.Reset();
    if (tip_) tip_->Deactivate();
    tip_.Reset();
    if (profiles_) profiles_->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, bada::tip::kLanguage, kClsid, kProfile, nullptr, TF_IPPMF_FORPROCESS);
    profiles_.Reset();
    if (document_) document_->Pop(TF_POPF_ALL);
    if (other_document_) other_document_->Pop(TF_POPF_ALL);
    context_.Reset();
    other_context_.Reset();
    document_.Reset();
    other_document_.Reset();
    keystrokes_.Reset();
    if (threads_) threads_->Deactivate();
    threads_.Reset();
}

WORD Host::VkOf(const KeySpec& key) const {
    if (key.vk) return key.vk;
    const UINT vk = MapVirtualKeyExW(key.scan, MAPVK_VSC_TO_VK, GetKeyboardLayout(0));
    return vk ? static_cast<WORD>(vk) : 0xFF;
}

// The keyboard's state as GetKeyState reads it in this thread: the text service asks it for the
// modifiers and the lock.
void Host::ApplyState() {
    state_[VK_CAPITAL] = static_cast<BYTE>((state_[VK_CAPITAL] & 0x80) | (caps_ ? 1 : 0));
    SetKeyboardState(state_);
}

bool Host::Down(const KeySpec& key) {
    const WORD vk = VkOf(key);
    state_[vk] |= 0x80;
    if (vk == VK_SHIFT) state_[key.scan == 0x36 ? VK_RSHIFT : VK_LSHIFT] |= 0x80;
    if (vk == VK_CONTROL) state_[VK_LCONTROL] |= 0x80;
    if (vk == VK_MENU) state_[VK_LMENU] |= 0x80;
    ApplyState();
    ITfContext* context = other_focused_ ? other_context_.Get() : context_.Get();
    BOOL eaten = FALSE;
    if (sink_) {
        // Fed to the TIP directly (the by-hand paths). OnTestKeyDown runs the harness; a key it
        // ate is reported again to OnKeyDown, which takes the cached result, as TSF does.
        sink_->OnTestKeyDown(context, vk, Param(key, false), &eaten);
        BOOL again = FALSE;
        sink_->OnKeyDown(context, vk, Param(key, false), &again);
    } else {
        keystrokes_->TestKeyDown(vk, Param(key, false), &eaten);
        if (eaten) keystrokes_->KeyDown(vk, Param(key, false), &eaten);
    }
    if (!eaten) AppKey(key, vk);
    Pump(0);
    return eaten != FALSE;
}

void Host::Up(const KeySpec& key) {
    const WORD vk = VkOf(key);
    state_[vk] &= 1;
    if (vk == VK_SHIFT) state_[VK_LSHIFT] = state_[VK_RSHIFT] = 0;
    if (vk == VK_CONTROL) state_[VK_LCONTROL] = 0;
    if (vk == VK_MENU) state_[VK_LMENU] = 0;
    ApplyState();
    ITfContext* context = other_focused_ ? other_context_.Get() : context_.Get();
    BOOL eaten = FALSE;
    if (sink_) {
        sink_->OnTestKeyUp(context, vk, Param(key, true), &eaten);
        BOOL again = FALSE;
        sink_->OnKeyUp(context, vk, Param(key, true), &again);
    } else {
        keystrokes_->TestKeyUp(vk, Param(key, true), &eaten);
        if (eaten) keystrokes_->KeyUp(vk, Param(key, true), &eaten);
    }
    Pump(0);
}

// What an app does with a key the text service didn't eat: it types what Windows' layout has for
// it, as TranslateMessage would.
void Host::AppKey(const KeySpec& key, WORD vk) {
    if ((state_[VK_CONTROL] | state_[VK_MENU]) & 0x80) return;  // a shortcut
    test::TextStore& store = other_focused_ ? *other_store_.Get() : *store_.Get();
    if (vk == VK_BACK) return store.Backspace();
    if (vk == VK_RETURN) return store.Type(L"\n");
    wchar_t text[8];
    const int n = ToUnicodeEx(vk, key.scan, state_, text, ARRAYSIZE(text), 4, GetKeyboardLayout(0));
    if (n > 0 && text[0] >= 0x20) store.Type(std::wstring(text, static_cast<size_t>(n)));
}

bool Host::Press(const std::string& word) {
    std::string key = word;
    std::vector<const KeySpec*> held;
    bool down = true, up = true;
    while (key.size() > 2 && key[1] == '-') {
        const char prefix = key[0];
        if (prefix == 'S') held.push_back(Find("ShiftLeft"));
        else if (prefix == 'C') held.push_back(Find("ControlLeft"));
        else if (prefix == 'A') held.push_back(Find("AltLeft"));
        else if (prefix == 'D') up = false;
        else if (prefix == 'U') down = false;
        else if (prefix == 'L') was_down_ = true, up = false;  // a lock-like key, pressed again
        else break;
        key = key.substr(2);
    }
    const KeySpec* spec = Find(key);
    if (!spec) {
        Check(false, "the test knows no key named " + key);
        return false;
    }
    for (const KeySpec* modifier : held) Down(*modifier);
    const bool eaten = down ? Down(*spec) : false;
    was_down_ = false;
    if (up) Up(*spec);
    for (auto it = held.rbegin(); it != held.rend(); ++it) Up(**it);
    return eaten;
}

void Host::Type(const std::string& keys) {
    std::istringstream words(keys);
    std::string word;
    while (words >> word) Press(word);
}

void Host::FocusOther(bool secure) {
    // An app marks a password field this way through the IMM layer: the keyboard is off for it.
    ComPtr<ITfCompartmentMgr> compartments;
    ComPtr<ITfCompartment> disabled;
    if (SUCCEEDED(other_context_.As(&compartments)) &&
        SUCCEEDED(compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &disabled))) {
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = secure ? 1 : 0;
        disabled->SetValue(app_, &v);
    }
    other_focused_ = true;
    threads_->SetFocus(other_document_.Get());
    Pump(0);
}

void Host::FocusMain() {
    other_focused_ = false;
    threads_->SetFocus(document_.Get());
    Pump(0);
}

void Host::AppEndsComposition() {
    ComPtr<ITfContextOwnerCompositionServices> services;
    if (SUCCEEDED(context_.As(&services))) services->TerminateComposition(nullptr);
    Pump(0);
}

std::string Host::Attributes() {
    std::string out;
    ComPtr<ITfCategoryMgr> categories;
    CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
    ComPtr<ITfContext> context = context_;
    ComPtr<ReadSession> session;
    session.Attach(new ReadSession([&](TfEditCookie ec) {
        ComPtr<ITfProperty> property;
        ComPtr<IEnumTfRanges> ranges;
        if (FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)) || FAILED(property->EnumRanges(ec, &ranges, nullptr))) return;
        ComPtr<ITfRange> range;
        while (ranges->Next(1, &range, nullptr) == S_OK) {
            VARIANT value;
            VariantInit(&value);
            GUID guid = {};
            ComPtr<ITfRangeACP> acp;
            LONG start = 0, length = 0;
            if (SUCCEEDED(property->GetValue(ec, range.Get(), &value)) && value.vt == VT_I4 && categories &&
                SUCCEEDED(categories->GetGUID(static_cast<TfGuidAtom>(value.lVal), &guid)) && SUCCEEDED(range.As(&acp)) &&
                SUCCEEDED(acp->GetExtent(&start, &length)) && length > 0) {
                const char* kind = IsEqualGUID(guid, bada::tip::kDisplayAttributeLive)     ? "live"
                                   : IsEqualGUID(guid, bada::tip::kDisplayAttributeFixed)  ? "fixed"
                                   : IsEqualGUID(guid, bada::tip::kDisplayAttributePicked) ? "picked"
                                                                                           : "other";
                if (!out.empty()) out += ", ";
                out += std::string(kind) + " " + std::to_string(start) + "+" + std::to_string(length);
            }
            VariantClear(&value);
            range.Reset();
        }
    }));
    HRESULT hr = E_FAIL;
    context_->RequestEditSession(app_, session.Get(), TF_ES_SYNC | TF_ES_READ, &hr);
    return out;
}

json::Value Host::State() const {
    const auto snapshot = bada::tip::ReadSharedState(bada::tip::kStateMapping);
    if (!snapshot) return json::Value{};
    auto parsed = json::Parse(snapshot->json);
    return parsed ? *parsed : json::Value{};
}

const char* kSettings = R"({
  "format": "bada-settings/2",
  "engine": {
    "muhenkan": {"layout": "en-qwerty"},
    "henkan": {"profiles": [
      {"id": "ko", "name": "한국어", "layouts": ["ko-2-set"]},
      {"id": "ja", "name": "日本語", "layouts": ["ja-romaji"]},
      {"id": "work", "name": "work", "layouts": ["ko-2-set", "en-qwerty"]}
    ]}
  },
  "keys": {
    "bindings": [
      {"on": "NonConvert", "do": ["mode:muhenkan"]},
      {"on": "Convert", "do": ["profile.first"]},
      {"on": "Convert", "do": ["menu.next", "profile.first"], "when": "henkan"},
      {"on": "shift+Convert", "do": ["menu.prev"], "when": "henkan"},
      {"on": "ctrl+Convert", "do": ["fix"], "when": "henkan"},
      {"on": "Backquote", "do": ["width.full"]},
      {"on": "shift+Backquote", "do": ["width.half"]},
      {"on": "KanaMode", "do": ["profile:ja", "menu.next"]},
      {"on": "F13", "do": ["profile:work"]},
      {"on": "Lang1+Lang2", "do": ["profile:work"]},
      {"on": "ShiftRight", "do": ["mode.toggle"]}
    ]
  },
  "harness": {"com.teammirae.bada.windows": {"keyboard": "jis-pc"}}
})";

std::string ModeOf(const json::Value& state) { return state.GetString("mode") + "/" + state.GetString("profile"); }

// The second process of the test of the mode across processes. It starts while the first is in
// henkan with the profile `ja`, says what mode it finds (read from the shared compartment), and
// switches to muhenkan for the first process to follow.
int Follow(Host& host) {
    const std::string found = ModeOf(host.State());
    host.Press("NonConvert");
    const std::string after = ModeOf(host.State());
    std::printf("     the second process found %s, and switched to %s\n", found.c_str(), after.c_str());
    Pump(100);
    return found == "henkan/ja" && after.rfind("muhenkan", 0) == 0 ? 0 : 1;
}

void Run(Host& host, const fs::path& dir, const std::wstring& self, const std::wstring& child_arguments) {
    test::TextStore& store = host.store();

    // --- A new install: no settings file (the standard, §8.3).
    Check(!host.Press("KeyA") && host.Text() == "a", "new install: a key goes to the app, and Windows types it");
    json::Value state = host.State();
    Check(state.GetString("format") == "bada-compatible-state/2" && state.GetString("mode") == "muhenkan" &&
              state.Get("harness") && state.Get("harness")->GetString("id") == "com.teammirae.bada.windows" &&
              state.Get("harness")->GetBool("active"),
          "the state is published: " + Shown(json::Write(state)).substr(0, 120));
    // With no profile a key that would enter henkan can't: whatever this machine's keyboard preset
    // (§9.5), a new install stays in muhenkan. (The onboarding notice is exercised in full, with a
    // forced preset, by the headless core tests.)
    host.Press("Convert");
    state = host.State();
    Check(state.GetString("mode") == "muhenkan",
          "new install: no profile, so no henkan: " + ModeOf(state));
    Check(state.Get("harness")->Get("keys")->Get("available")->array.size() == 7 &&
              state.Get("harness")->Get("keys")->GetBool("chords") && state.Get("harness")->Get("problems")->array.empty(),
          "harness.keys and harness.problems are published");
    store.Clear();

    // --- The settings are written: the folder is looked at with the next key.
    WriteFile(dir / "settings.json", kSettings);
    Sleep(300);
    host.Type("KeyH KeyI");
    CheckEq(host.Text(), "hi", "muhenkan, en-qwerty: Windows types the letters itself");
    Check(host.State().Get("harness")->GetString("notice").empty(), "the settings were read: the notice is gone");

    // --- Muhenkan: `types` (§6.4). Windows' layout here isn't Japanese, so the JIS legends are the
    // harness's to type: ^ on the key after -, " on Shift+2.
    wchar_t os[8] = L"";
    BYTE none[256] = {};
    const int n = ToUnicodeEx(MapVirtualKeyW(0x0D, MAPVK_VSC_TO_VK), 0x0D, none, os, 8, 4, GetKeyboardLayout(0));
    const bool japanese = n == 1 && os[0] == L'^';
    Check(host.Press("Equal") != japanese, japanese ? "Windows' layout is Japanese: it types ^ itself"
                                                    : "Windows' layout has = there: the harness types ^ and eats the key");
    host.Press("S-Digit2");
    host.Press("S-KeyA");
    CheckEq(host.Text(), "hi^\"A", "the JIS legends, whatever layout Windows has");
    Check(host.Press("S-Digit0") && host.Text() == "hi^\"A", "Shift+0 is the JIS table's empty slot: it types nothing");
    // The lock: the letters of `types` in capitals, Shift or not.
    host.SetCapsLock(true);
    host.Type("KeyB S-KeyC Digit1");
    host.SetCapsLock(false);
    CheckEq(host.Text(), "hi^\"ABC1", "Caps Lock types capitals, with Shift too, and leaves digits alone");
    Check(!host.Press("C-KeyC") && host.Text() == "hi^\"ABC1", "a shortcut goes to the app");
    store.Clear();

    // --- Korean 2-set: 変換 enters the first profile; gks is 한, marked; Space commits.
    Check(host.Press("Convert"), "\xE5\xA4\x89\xE6\x8F\x9B is the key table's");
    CheckEq(ModeOf(host.State()), "henkan/ko", "the mode and the profile in the state");
    host.Type("KeyG KeyK KeyS");
    CheckEq(host.Text(), "\xED\x95\x9C", "ko-2-set: gks is \xED\x95\x9C");
    CheckEq(host.MarkedText(), "\xED\x95\x9C", "…and it is the marked text: a TSF composition");
    CheckEq(host.Attributes(), "live 0+1", "its display attribute is the live one");
    state = host.State();
    Check(state.GetString("staged") == "\xED\x95\x9C" && state.Get("preedit")->GetString("text") == "\xED\x95\x9C",
          "the state has what is staged");
    Check(!host.Press("Space"), "Space isn't eaten: it commits, and the app gets the key");
    CheckEq(host.Text(), "\xED\x95\x9C ", "Space commits \xED\x95\x9C, then types a space");
    Check(host.MarkedText().empty() && host.Attributes().empty(), "nothing is marked after the commit");
    host.Type("KeyD KeyK KeyS KeyS KeyU KeyD Enter");
    CheckEq(host.Text(), "\xED\x95\x9C \xEC\x95\x88\xEB\x85\x95\n", "안녕, then Enter commits and goes to the app");
    host.Type("KeyG KeyK Backspace");
    CheckEq(host.MarkedText(), "\xE3\x85\x8E", "Backspace takes back the last staged key");
    host.Press("Escape");
    CheckEq(host.Text(), "\xED\x95\x9C \xEC\x95\x88\xEB\x85\x95\n\xE3\x85\x8E", "Esc commits");
    store.Clear();

    // --- A mode switch through the key table: 無変換 commits what is staged and leaves henkan.
    host.Type("KeyG KeyK");
    Check(host.Press("NonConvert"), "\xE7\x84\xA1\xE5\xA4\x89\xE6\x8F\x9B is the key table's");
    CheckEq(host.Text(), "\xED\x95\x98", "the switch commits what was staged");
    CheckEq(ModeOf(host.State()), "muhenkan/ko", "muhenkan, with the profile kept for later");
    host.Type("KeyA");
    CheckEq(host.Text(), "\xED\x95\x98" "a", "and keys type directly again");
    store.Clear();

    // --- Romaji, and a profile switch: カタカナ is bound to profile:ja.
    Check(host.Press("KanaMode"), "\xE3\x82\xAB\xE3\x82\xBF\xE3\x82\xAB\xE3\x83\x8A is the key table's");
    CheckEq(ModeOf(host.State()), "henkan/ja", "a profile switch from muhenkan enters henkan");
    // The Japanese layout never says カタカナ was released, so Windows marks every later press of
    // it as a held key's repeat. Each is a press all the same.
    host.Press("NonConvert");
    Check(host.Press("L-KanaMode"), "\xE3\x82\xAB\xE3\x82\xBF\xE3\x82\xAB\xE3\x83\x8A again is the key table's");
    CheckEq(ModeOf(host.State()), "henkan/ja", "a later press of it, marked as a repeat, is a press");
    // Windows reports カタカナ/ひらがな only while the mode it was told isn't kana already. Open in
    // henkan, bada says alphanumeric; after the key, it says so anew (a change, so Windows hears).
    const LONG told = host.ThreadCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    Check(host.ThreadCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) == 1, "henkan: the keyboard is open");
    Check(told >= 0 && !(told & (TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA)),
          "henkan: the conversion mode Windows is told isn't kana");
    host.Press("L-KanaMode");
    const LONG again = host.ThreadCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    Check(again != told && again >= 0 && !(again & (TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA)),
          "after \xE3\x82\xAB\xE3\x82\xBF\xE3\x82\xAB\xE3\x83\x8A the conversion mode is told anew, still not kana");
    host.Type("KeyK KeyA");
    CheckEq(host.MarkedText(), "\xE3\x81\x8B", "ja-romaji: ka is \xE3\x81\x8B");
    // fix (Ctrl+変換): what shows no longer changes; the next keys are a new part.
    host.Press("C-Convert");
    host.Type("KeyN KeyA");
    CheckEq(host.MarkedText(), "\xE3\x81\x8B\xE3\x81\xAA", "fix, then na: \xE3\x81\x8B\xE3\x81\xAA");
    CheckEq(host.Attributes(), "fixed 0+1, live 1+1", "the fixed part and the live part have their own display attributes");
    // Another profile, by a chord: it commits what was staged.
    Check(host.Press("F13"), "F13 is bound: a chord");
    CheckEq(host.Text(), "\xE3\x81\x8B\xE3\x81\xAA", "a profile switch commits what was staged");
    CheckEq(ModeOf(host.State()), "henkan/work", "the profile is work");
    store.Clear();

    // --- Two layouts in a profile: a menu. 変換 is the next row, Shift+変換 the previous.
    host.Type("KeyG KeyK");
    state = host.State();
    Check(state.Get("menu")->Get("rows")->array.size() == 2 && state.Get("menu")->GetNumber("selected", -1) == 0,
          "a menu of both readings: " + Shown(json::Write(*state.Get("menu"))));
    CheckEq(host.MarkedText(), "\xED\x95\x98", "the first reading shows");
    host.Press("Convert");
    CheckEq(host.MarkedText(), "gk", "\xE5\xA4\x89\xE6\x8F\x9B moves to the next row");
    CheckEq(host.Attributes(), "picked 0+2", "the row moved to has the picked attribute");
    Check(host.State().Get("menu")->GetNumber("selected", -1) == 1, "the state follows the menu");
    host.Press("S-Convert");
    CheckEq(host.MarkedText(), "\xED\x95\x98", "Shift+\xE5\xA4\x89\xE6\x8F\x9B moves back");
    host.Press("Convert");
    host.Press("Space");
    CheckEq(host.Text(), "gk ", "Space commits the row shown");
    store.Clear();

    // --- `ask` (§6.3): 半角/全角 with nothing staged asks for the selection, once, and replaces it.
    host.Press("NonConvert");
    host.Type("KeyA KeyB KeyC");
    store.Select(0, 3);
    Check(host.Press("Backquote"), "\xE5\x8D\x8A\xE8\xA7\x92/\xE5\x85\xA8\xE8\xA7\x92 is the key table's");
    CheckEq(host.Text(), "\xEF\xBD\x81\xEF\xBD\x82\xEF\xBD\x83", "the selection is made full-width: the text service read it from the document");
    store.Clear();

    // --- A modifier tapped alone, and a pair of language keys (§9.3).
    host.Press("ShiftRight");
    CheckEq(ModeOf(host.State()), "henkan/work", "right Shift tapped alone is bound to the toggle");
    host.Press("S-KeyA");
    host.Press("NonConvert");
    store.Clear();
    host.Press("D-Lang1");
    host.Press("D-Lang2");
    CheckEq(ModeOf(host.State()), "muhenkan/work", "two language keys held: nothing yet");
    host.Press("U-Lang1");
    host.Press("U-Lang2");
    CheckEq(ModeOf(host.State()), "henkan/work", "both released: the pair's binding ran");
    host.Press("Convert");
    CheckEq(ModeOf(host.State()), "henkan/ko", "\xE5\xA4\x89\xE6\x8F\x9B with nothing staged goes to the first profile");

    // --- Focus: moving to another document commits what is staged where it was, and the other
    // field has an engine of its own that follows the same mode.
    host.Type("KeyG KeyK");
    host.FocusOther(false);
    CheckEq(host.Text(), "\xED\x95\x98", "focus left: what was staged is committed");
    host.Type("KeyS");
    CheckEq(Utf8(host.other().text()), "\xE3\x84\xB4", "the other document has an engine of its own, in the same mode");
    host.Press("Space");
    host.FocusMain();
    store.Clear();

    // (A password field, an app that ends or refuses the composition, and typing in a second
    // process are tested against the real engine in bada_core_tests, and need a real edit control
    // to exercise through TSF: the README's "Verify by hand" has the steps.)

    // --- Settings that can't be used change nothing, and their problems are published (§10.1).
    // Start from a clean henkan/ko with nothing staged.
    host.Press("NonConvert");
    host.Press("Convert");
    store.Clear();
    WriteFile(dir / "settings.json", R"({"format": "bada-settings/2", "keys": {"bindings": [{"on": "Convert", "do": ["dance"]}]}})");
    Sleep(300);
    host.Type("KeyG");
    state = host.State();
    const json::Value* problems = state.Get("harness")->Get("problems");
    Check(problems && problems->array.size() == 1 && problems->array[0].GetString("file") == "settings.json" &&
              problems->array[0].GetString("path") == "keys.bindings[0].do[0]" &&
              problems->array[0].GetString("code") == "unknown-action" && problems->array[0].GetString("level") == "error",
          "a key table with an error is a problem in the state: " + Shown(json::Write(*problems)));
    CheckEq(host.MarkedText(), "\xE3\x85\x8E", "…and what was in effect stays: still henkan, still ko-2-set");
    host.Press("Space");
    WriteFile(dir / "settings.json", kSettings);
    Sleep(300);
    host.Type("KeyA");
    Check(host.State().Get("harness")->Get("problems")->array.empty(), "a good file again: no problems");
    host.Press("Space");
    store.Clear();

    // --- The mode and the profile across processes: a TSF global compartment, the profile by id.
    host.Press("KanaMode");
    CheckEq(ModeOf(host.State()), "henkan/ja", "this process is in henkan, profile ja");
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION child = {};
    const fs::path follower_out = dir / "follower.txt";
    std::wstring command = L"\"" + self + L"\" " + child_arguments + L" --follow \"" + dir.wstring() + L"\" --out \"" +
                           follower_out.wstring() + L"\"";
    // The second process is on this one's desktop.
    std::wstring desktop = g_desktop;
    if (!desktop.empty()) startup.lpDesktop = desktop.data();
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        // The second process switches to muhenkan before it ends. This one hears of it.
        DWORD code = 1;
        while (WaitForSingleObject(child.hProcess, 20) == WAIT_TIMEOUT) Pump(0);
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        std::ifstream said(follower_out, std::ios::binary);
        std::stringstream text;
        text << said.rdbuf();
        std::fputs(text.str().c_str(), stdout);
        Check(code == 0, "a second process reads the session's mode and profile, and switches it");
        Pump(200);
        // The first process follows the switch: a change to the compartment comes to OnChange, or
        // to the next focus. The mode is now muhenkan for everyone (the profile is kept for later).
        host.FocusOther(false);
        host.FocusMain();
        Check(host.State().GetString("mode") == "muhenkan",
              "the second process switched to muhenkan, and this one followed: " + ModeOf(host.State()));
    } else {
        Check(false, "the second process didn't start");
    }
}

// Moves this thread to a new desktop of the same window station: its windows (the menu, COM's
// own) are made there, where nobody sees them.
void OwnDesktop() {
    const std::wstring name = L"bada-tsf-host-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (desktop && SetThreadDesktop(desktop)) {
        g_desktop = L"WinSta0\\" + name;
    } else {
        std::printf("     a desktop of its own wasn't made (%lu): the test stays on the one it started on\n", GetLastError());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dll, follow, out;
    bool registered = false, own_desktop = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring flag = argv[i];
        if (flag == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (flag == L"--registered") registered = true;
        else if (flag == L"--own-desktop") own_desktop = true;
        else if (flag == L"--follow" && i + 1 < argc) follow = argv[++i];
        else if (flag == L"--out" && i + 1 < argc) out = argv[++i];
    }
    if (dll.empty() && !registered) {
        std::fprintf(stderr, "usage: bada_tsf_host (--dll <bada-tsf.dll> | --registered) [--own-desktop] [--out <file>]\n");
        return 2;
    }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring arguments = registered ? std::wstring(L"--registered") : L"--dll \"" + dll + L"\"";

    // Run through a scheduled task (--own-desktop, no console): its output goes to a file.
    if (!out.empty()) _wfreopen(out.c_str(), L"w", stdout);
    if (own_desktop) OwnDesktop();

    // The settings folder: the first process makes a new one; the second is given it.
    fs::path dir = follow;
    if (dir.empty()) {
        std::random_device random;
        dir = fs::temp_directory_path() / ("bada-tsf-host-" + std::to_string(random() % 1000000));
        fs::create_directories(dir);
    }
    SetEnvironmentVariableW(L"BADA_CONFIG_DIR", dir.c_str());
    SetEnvironmentVariableW(L"BADA_LOG_FILE", (dir / "bada.log").c_str());
    // No data, wherever the DLL is: what this test types doesn't depend on what is installed.
    SetEnvironmentVariableW(L"BADA_DATA_DIR", (dir / "no-data").c_str());

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    {
        Host host;
        std::string error;
        How how = registered ? How::kProfile : How::kDll;
        bool started = host.Start(how, dll, &error);
        if (!started && registered && error.find("ActivateProfile") != std::string::npos) {
            // TSF wouldn't activate the profile here: the registered class is still made through
            // COM, by its CLSID, and activated by hand.
            if (follow.empty()) std::printf("     %s: the registered class is activated by hand instead\n", error.c_str());
            host.Stop();
            how = How::kClsid;
            started = host.Start(how, dll, &error);
        }
        if (!started) {
            std::printf("%s\n", error.c_str());
            if (error.find("thread manager") != std::string::npos) {
                std::printf("TSF works only in an interactive logon: run this at the machine's desktop, or with tools\\test.ps1 -InSession\n");
            }
            std::fflush(stdout);
            return 2;
        }
        if (!follow.empty()) {
            result = Follow(host);
        } else {
            std::printf("the text service: %s\n", how == How::kDll       ? "loaded from its DLL, not registered"
                                                  : how == How::kProfile ? "registered, activated by TSF from its profile"
                                                                         : "registered, made by its CLSID");
            DWORD session = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &session);
            std::printf("     in session %lu%s\n", session, g_desktop.empty() ? "" : ", on a desktop of the test's own");
            Run(host, dir, self, arguments);
            if (g_failures) {
                // The text service's own log of the run.
                std::ifstream log(dir / "bada.log", std::ios::binary);
                std::stringstream text;
                text << log.rdbuf();
                std::printf("--- the log\n%s", text.str().c_str());
            }
            std::printf("tsf host: %d checks, %d failed\n", g_checks, g_failures);
            result = g_failures ? 1 : 0;
        }
        host.Stop();
    }
    CoUninitialize();
    std::fflush(stdout);
    if (follow.empty()) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    return result;
}
