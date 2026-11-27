// The text service: one COM object per thread with TSF. It moves key events in and text out as a
// TSF composition, shows the engine's menu, keeps the mode and the profile the session's, and
// sets the lock. Everything it decides is the harness's (src/core/harness.h): the key table, the
// engine, how a reply is applied.
//
// One TSF language profile does everything: muhenkan and henkan are the harness's own state,
// shared by every app of the session through a TSF global compartment (the profile by its id), and
// mirrored in the open/close compartment for the input indicator.
//
// Adapted from bada-windows src/tip/text_service.*, which is jis-moonshot's: the key handling, the
// composition and the edit sessions keep what was measured there (README, "TSF").
#pragma once

#include <msctf.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "candidate_window.h"
#include "harness.h"
#include "lang_bar.h"
#include "shared_session.h"
#include "tide_ipc.h"

namespace bada::tip {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfActiveLanguageProfileNotifySink,
                          public ITfCompositionSink,
                          public ITfCompartmentEventSink,
                          public ITfDisplayAttributeProvider {
public:
    TextService();
    TextService(const TextService&) = delete;
    TextService& operator=(const TextService&) = delete;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor(Ex)
    STDMETHODIMP Activate(ITfThreadMgr* thread_mgr, TfClientId client_id) override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* thread_mgr, TfClientId client_id, DWORD flags) override;
    STDMETHODIMP Deactivate() override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL foreground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* previous) override;
    STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext* context) override;

    // ITfActiveLanguageProfileNotifySink
    STDMETHODIMP OnActivated(REFCLSID clsid, REFGUID profile, BOOL activated) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ec, ITfComposition* composition) override;

    // ITfCompartmentEventSink
    STDMETHODIMP OnChange(REFGUID guid) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** infos) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) override;

private:
    class TsfField;
    friend class TsfField;
    ~TextService();

    // The marked text of one reply, in UTF-16: its caret and its segments' display attributes.
    struct Marked {
        std::wstring text;
        LONG caret = 0;  // in UTF-16 code units from the start
        struct Part {
            LONG length = 0;
            TfGuidAtom attribute = TF_INVALID_GUIDATOM;
        };
        std::vector<Part> parts;
    };
    Marked MarkedOf(const Preedit& preedit) const;

    // One text field of this thread: a TSF context, and the harness's side of it (its engine).
    struct FieldEntry {
        Microsoft::WRL::ComPtr<IUnknown> identity;  // the context's; null for keys with no context
        std::unique_ptr<Context> context;
        ULONGLONG used = 0;
    };
    // The field of `context` (nullptr: the one for keys that come with none), made if it isn't
    // there. nullptr when the engine can't be used. Call with the process's lock held.
    Context* FieldFor(ITfContext* context);
    // The same, never made.
    Context* FindField(ITfContext* context);
    void DropField(ITfContext* context);
    // The context that has the keyboard on this thread, if any.
    Microsoft::WRL::ComPtr<ITfContext> FocusContext() const;

    // A key through the harness. True: eaten.
    bool KeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam);
    bool KeyUp(ITfContext* context, WPARAM wparam, LPARAM lparam);
    RawKey MakeKey(WPARAM wparam, LPARAM lparam, bool up) const;
    ContextInfo Info() const;
    // After any call into the harness: the session's compartment, the lock, the indicator, the menu.
    void AfterHarness();

    // A password or PIN field, or one the app turned the keyboard off for.
    bool PrivateField(ITfContext* context);
    bool Private(const wchar_t* why);
    bool Composing();
    ITfContext* Target(ITfContext* context) const;
    void NoContext();
    void DropInvisible();
    void CheckOrphan();
    void Flush(ITfContext* context, const char* cause);
    void Settle();

    // A reply's text, applied to the document in one edit session (the standard, §6.3).
    bool UpdateText(ITfContext* context, const std::wstring& commit, const Marked& marked, bool replace_selection);
    HRESULT EditText(TfEditCookie ec, ITfContext* context, ITfComposition* expected, const std::wstring& commit,
                     const Marked& marked, bool replace_selection);
    Around ReadAround(ITfContext* context, size_t before, size_t after);
    void ShowList();
    void ForwardKey(const RawKey& key);

    // The session's mode and profile (a global compartment), and the open/close mirror.
    std::optional<Shared> ReadSession();
    void WriteSession();
    void FollowSession();
    void SetOpen(bool open);
    // The conversion mode Windows is told: alphanumeric, always (the .cpp says why). `again`:
    // told anew, after a key that made Windows believe otherwise.
    void PinConversion(bool again);
    void AdviseCompartments(bool on);
    void UpdateIndicator();

    // The lock, set to what the harness decided (the standard, §9.4).
    void ReconcileLock();

    // bada-tide (src/tide/): the keys a keyboard reported, that the helper hands this thread, and
    // what the helper is told of the harness's state. The thread's message-only window takes the
    // helper's messages (src/core/tide_ipc.h).
    void OpenTideWindow();
    void CloseTideWindow();
    void TideKey(const tide::ipc::KeyEvent& key);
    void SendTideHost(bool force);
    void TideStatusChanged();
    static LRESULT CALLBACK TideWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void InitMenu(ITfMenu* menu);
    void OnMenu(UINT id);

    LONG refs_ = 1;
    bool active_ = false;
    bool refused_ = false;
    bool in_update_ = false;
    ULONGLONG updated_tick_ = 0;
    Microsoft::WRL::ComPtr<ITfThreadMgr> thread_mgr_;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    DWORD thread_sink_cookie_ = TF_INVALID_COOKIE;
    DWORD profile_sink_cookie_ = TF_INVALID_COOKIE;
    DWORD session_sink_cookie_ = TF_INVALID_COOKIE;
    DWORD openclose_sink_cookie_ = TF_INVALID_COOKIE;
    TfGuidAtom live_attribute_ = TF_INVALID_GUIDATOM;
    TfGuidAtom fixed_attribute_ = TF_INVALID_GUIDATOM;
    TfGuidAtom picked_attribute_ = TF_INVALID_GUIDATOM;
    Microsoft::WRL::ComPtr<ITfComposition> composition_;
    Microsoft::WRL::ComPtr<ITfContext> composition_context_;
    Microsoft::WRL::ComPtr<ITfRange> orphan_;
    Microsoft::WRL::ComPtr<ITfContext> orphan_context_;
    RECT anchor_ = {};
    Menu menu_;           // the last reply's menu
    std::string notice_;  // the harness's notice, shown until the next key
    CandidateWindow list_;
    LangBarButton* lang_bar_ = nullptr;
    std::vector<FieldEntry> fields_;
    std::vector<std::string> menu_profiles_;  // the profiles the input menu listed, by id
    // The last key-down and key-up tested: TSF asks about a key before it hands it over, and the
    // harness runs once for both.
    struct Tested {
        WPARAM wparam = 0;
        LPARAM lparam = 0;
        LONG time = 0;
        ULONGLONG tick = 0;
        BOOL eaten = FALSE;
        bool valid = false;
        bool pending = false;
    };
    Tested tested_, tested_up_;
    bool private_ = false;
    bool conversion_pinned_ = false;
    // What the lock must be, until the next key that isn't Caps Lock.
    std::optional<bool> lock_target_;
    ULONGLONG lock_injected_tick_ = 0;
    HWND tide_window_ = nullptr;
    HWND tide_helper_ = nullptr;   // the helper's window, when it was last found
    std::uint32_t tide_sent_ = 0;  // the host word it was last sent
    bool tide_sent_valid_ = false;
};

}  // namespace bada::tip
