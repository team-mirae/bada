// A text field for the text service's tests: a TSF text store (ITextStoreACP) over a string, with
// no window. It is what an app gives TSF: the text service reads and writes it through edit
// sessions, exactly as it does in a real app, and the test reads the text back.
#pragma once

#include <msctf.h>
#include <textstor.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

namespace test {

class TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
public:
    TextStore() = default;

    // The document.
    const std::wstring& text() const { return text_; }
    LONG selection_start() const { return start_; }
    LONG selection_end() const { return end_; }
    // The marked text: the extent of the composition, if there is one.
    bool Marked(LONG* start, LONG* length) const;
    std::wstring MarkedText() const;
    int compositions_started() const { return started_; }
    // An app that refuses every composition.
    void RefuseCompositions(bool refuse) { refuse_ = refuse; }

    // What the app itself does to its text, outside any edit session: TSF is told.
    void Type(const std::wstring& text);  // replaces the selection, caret after it
    void Backspace();
    void Select(LONG start, LONG end);
    void Clear();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITextStoreACP
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* sink, DWORD mask) override;
    STDMETHODIMP UnadviseSink(IUnknown* sink) override;
    STDMETHODIMP RequestLock(DWORD flags, HRESULT* session) override;
    STDMETHODIMP GetStatus(TS_STATUS* status) override;
    STDMETHODIMP QueryInsert(LONG start, LONG end, ULONG count, LONG* result_start, LONG* result_end) override;
    STDMETHODIMP GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) override;
    STDMETHODIMP SetSelection(ULONG count, const TS_SELECTION_ACP* selection) override;
    STDMETHODIMP GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_size, ULONG* plain_got, TS_RUNINFO* runs,
                         ULONG runs_size, ULONG* runs_got, LONG* next) override;
    STDMETHODIMP SetText(DWORD flags, LONG start, LONG end, const WCHAR* text, ULONG count, TS_TEXTCHANGE* change) override;
    STDMETHODIMP GetFormattedText(LONG, LONG, IDataObject**) override { return E_NOTIMPL; }
    STDMETHODIMP GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* insertable) override;
    STDMETHODIMP InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    STDMETHODIMP InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count, LONG* start, LONG* end,
                                       TS_TEXTCHANGE* change) override;
    STDMETHODIMP InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    STDMETHODIMP RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override { return S_OK; }
    STDMETHODIMP RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    STDMETHODIMP FindNextAttrTransition(LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD, LONG* next, BOOL* found,
                                        LONG* offset) override;
    STDMETHODIMP RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) override;
    STDMETHODIMP GetEndACP(LONG* end) override;
    STDMETHODIMP GetActiveView(TsViewCookie* view) override;
    STDMETHODIMP GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rect, BOOL* clipped) override;
    STDMETHODIMP GetScreenExt(TsViewCookie view, RECT* rect) override;
    STDMETHODIMP GetWnd(TsViewCookie view, HWND* window) override;

    // ITfContextOwnerCompositionSink
    STDMETHODIMP OnStartComposition(ITfCompositionView* composition, BOOL* ok) override;
    STDMETHODIMP OnUpdateComposition(ITfCompositionView* composition, ITfRange* range) override;
    STDMETHODIMP OnEndComposition(ITfCompositionView* composition) override;

private:
    ~TextStore() = default;
    bool Locked(DWORD flags) const { return (lock_ & flags) == flags; }
    void Replace(LONG start, LONG end, const WCHAR* text, ULONG count);

    LONG refs_ = 1;
    std::wstring text_;
    LONG start_ = 0, end_ = 0;  // the selection; a caret when equal
    bool interim_ = false;
    Microsoft::WRL::ComPtr<ITextStoreACPSink> sink_;
    DWORD lock_ = 0;      // the lock held now
    DWORD pending_ = 0;   // a lock asked for while another was held
    Microsoft::WRL::ComPtr<ITfCompositionView> composition_;
    int started_ = 0;
    bool refuse_ = false;
};

}  // namespace test
