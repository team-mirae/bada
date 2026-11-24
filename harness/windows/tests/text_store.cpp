#include "text_store.h"

#include <olectl.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace test {
namespace {

constexpr TsViewCookie kView = 1;

}  // namespace

STDMETHODIMP TextStore::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITextStoreACP) {
        *object = static_cast<ITextStoreACP*>(this);
    } else if (riid == IID_ITfContextOwnerCompositionSink) {
        *object = static_cast<ITfContextOwnerCompositionSink*>(this);
    } else {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextStore::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) TextStore::Release() {
    const LONG refs = InterlockedDecrement(&refs_);
    if (!refs) delete this;
    return refs;
}

bool TextStore::Marked(LONG* start, LONG* length) const {
    if (!composition_) return false;
    ComPtr<ITfRange> range;
    ComPtr<ITfRangeACP> acp;
    if (FAILED(composition_->GetRange(&range)) || !range || FAILED(range.As(&acp))) return false;
    return SUCCEEDED(acp->GetExtent(start, length));
}

std::wstring TextStore::MarkedText() const {
    LONG start = 0, length = 0;
    if (!Marked(&start, &length) || start < 0 || start + length > static_cast<LONG>(text_.size())) return {};
    return text_.substr(static_cast<size_t>(start), static_cast<size_t>(length));
}

void TextStore::Replace(LONG start, LONG end, const WCHAR* text, ULONG count) {
    text_.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), text ? text : L"", count);
}

void TextStore::Type(const std::wstring& text) {
    const TS_TEXTCHANGE change = {start_, end_, start_ + static_cast<LONG>(text.size())};
    Replace(start_, end_, text.data(), static_cast<ULONG>(text.size()));
    start_ = end_ = change.acpNewEnd;
    if (sink_) {
        sink_->OnTextChange(0, &change);
        sink_->OnSelectionChange();
    }
}

void TextStore::Backspace() {
    if (start_ == end_ && start_ > 0) --start_;
    Type({});
}

void TextStore::Select(LONG start, LONG end) {
    start_ = std::clamp(start, LONG{0}, static_cast<LONG>(text_.size()));
    end_ = std::clamp(end, start_, static_cast<LONG>(text_.size()));
    if (sink_) sink_->OnSelectionChange();
}

void TextStore::Clear() {
    Select(0, static_cast<LONG>(text_.size()));
    Type({});
}

STDMETHODIMP TextStore::AdviseSink(REFIID riid, IUnknown* sink, DWORD) {
    if (riid != IID_ITextStoreACPSink || !sink) return E_INVALIDARG;
    ComPtr<ITextStoreACPSink> advised;
    if (FAILED(sink->QueryInterface(IID_PPV_ARGS(&advised)))) return E_NOINTERFACE;
    if (sink_ && sink_ != advised) return CONNECT_E_ADVISELIMIT;
    sink_ = advised;
    return S_OK;
}

STDMETHODIMP TextStore::UnadviseSink(IUnknown*) {
    if (!sink_) return CONNECT_E_NOCONNECTION;
    sink_.Reset();
    return S_OK;
}

STDMETHODIMP TextStore::RequestLock(DWORD flags, HRESULT* session) {
    if (!sink_ || !session) return E_FAIL;
    if (lock_) {
        // One lock at a time. A request that must be answered now can't be; another waits its turn.
        if (flags & TS_LF_SYNC) {
            *session = TS_E_SYNCHRONOUS;
        } else {
            pending_ = flags;
            *session = TS_S_ASYNC;
        }
        return S_OK;
    }
    lock_ = flags & ~static_cast<DWORD>(TS_LF_SYNC);
    *session = sink_->OnLockGranted(lock_);
    lock_ = 0;
    while (pending_) {
        const DWORD next = pending_;
        pending_ = 0;
        lock_ = next;
        sink_->OnLockGranted(lock_);
        lock_ = 0;
    }
    return S_OK;
}

STDMETHODIMP TextStore::GetStatus(TS_STATUS* status) {
    if (!status) return E_INVALIDARG;
    status->dwDynamicFlags = 0;
    status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
    return S_OK;
}

STDMETHODIMP TextStore::QueryInsert(LONG start, LONG end, ULONG, LONG* result_start, LONG* result_end) {
    if (start < 0 || end < start || end > static_cast<LONG>(text_.size())) return E_INVALIDARG;
    if (result_start) *result_start = start;
    if (result_end) *result_end = end;
    return S_OK;
}

STDMETHODIMP TextStore::GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) {
    if (!selection || !fetched) return E_INVALIDARG;
    *fetched = 0;
    if (!Locked(TS_LF_READ)) return TS_E_NOLOCK;
    if (index != TS_DEFAULT_SELECTION && index != 0) return TS_E_NOSELECTION;
    if (!count) return S_OK;
    selection[0].acpStart = start_;
    selection[0].acpEnd = end_;
    selection[0].style.ase = TS_AE_END;
    selection[0].style.fInterimChar = interim_ ? TRUE : FALSE;
    *fetched = 1;
    return S_OK;
}

STDMETHODIMP TextStore::SetSelection(ULONG count, const TS_SELECTION_ACP* selection) {
    if (!Locked(TS_LF_READWRITE)) return TS_E_NOLOCK;
    if (count != 1 || !selection) return E_INVALIDARG;
    if (selection->acpStart < 0 || selection->acpEnd < selection->acpStart ||
        selection->acpEnd > static_cast<LONG>(text_.size())) {
        return TS_E_INVALIDPOS;
    }
    start_ = selection->acpStart;
    end_ = selection->acpEnd;
    interim_ = selection->style.fInterimChar != FALSE;
    return S_OK;
}

STDMETHODIMP TextStore::GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_size, ULONG* plain_got, TS_RUNINFO* runs,
                                ULONG runs_size, ULONG* runs_got, LONG* next) {
    if (!Locked(TS_LF_READ)) return TS_E_NOLOCK;
    const LONG length = static_cast<LONG>(text_.size());
    if (end == -1) end = length;
    if (start < 0 || start > length || end < start || end > length) return TS_E_INVALIDPOS;
    ULONG count = static_cast<ULONG>(end - start);
    if (plain && plain_size) {
        count = std::min(count, plain_size);
        std::copy_n(text_.data() + start, count, plain);
    } else if (runs && runs_size) {
        // Counting only: the whole range is one run of plain text.
    } else {
        count = 0;
    }
    if (plain_got) *plain_got = plain && plain_size ? count : 0;
    if (runs_got) *runs_got = 0;
    if (runs && runs_size && count) {
        runs[0].uCount = count;
        runs[0].type = TS_RT_PLAIN;
        if (runs_got) *runs_got = 1;
    }
    if (next) *next = start + static_cast<LONG>(count);
    return S_OK;
}

STDMETHODIMP TextStore::SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG count, TS_TEXTCHANGE* change) {
    if (!Locked(TS_LF_READWRITE)) return TS_E_NOLOCK;
    const LONG length = static_cast<LONG>(text_.size());
    if (end == -1) end = length;
    if (start < 0 || start > length || end < start || end > length) return TS_E_INVALIDPOS;
    Replace(start, end, text, count);
    const LONG new_end = start + static_cast<LONG>(count);
    // The selection keeps its place in the text around the change.
    auto moved = [&](LONG at) { return at <= start ? at : at >= end ? at + (new_end - end) : new_end; };
    start_ = moved(start_);
    end_ = moved(end_);
    if (change) *change = {start, end, new_end};
    return S_OK;
}

STDMETHODIMP TextStore::QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* insertable) {
    if (insertable) *insertable = FALSE;
    return S_OK;
}

STDMETHODIMP TextStore::InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count, LONG* start, LONG* end,
                                              TS_TEXTCHANGE* change) {
    if (flags & TS_IAS_QUERYONLY) {
        if (!Locked(TS_LF_READ)) return TS_E_NOLOCK;
        if (start) *start = start_;
        if (end) *end = end_;
        return S_OK;
    }
    if (!Locked(TS_LF_READWRITE)) return TS_E_NOLOCK;
    const LONG at = start_, old_end = end_;
    Replace(at, old_end, text, count);
    const LONG new_end = at + static_cast<LONG>(count);
    start_ = end_ = new_end;
    if (!(flags & TS_IAS_NOQUERY)) {
        if (start) *start = at;
        if (end) *end = new_end;
    }
    if (change) *change = {at, old_end, new_end};
    return S_OK;
}

STDMETHODIMP TextStore::FindNextAttrTransition(LONG, LONG halt, ULONG, const TS_ATTRID*, DWORD, LONG* next, BOOL* found,
                                               LONG* offset) {
    if (next) *next = halt;
    if (found) *found = FALSE;
    if (offset) *offset = 0;
    return S_OK;
}

STDMETHODIMP TextStore::RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) {
    if (fetched) *fetched = 0;
    return S_OK;
}

STDMETHODIMP TextStore::GetEndACP(LONG* end) {
    if (!end) return E_INVALIDARG;
    if (!Locked(TS_LF_READ)) return TS_E_NOLOCK;
    *end = static_cast<LONG>(text_.size());
    return S_OK;
}

STDMETHODIMP TextStore::GetActiveView(TsViewCookie* view) {
    if (!view) return E_INVALIDARG;
    *view = kView;
    return S_OK;
}

STDMETHODIMP TextStore::GetTextExt(TsViewCookie, LONG start, LONG end, RECT* rect, BOOL* clipped) {
    if (!rect || !clipped) return E_INVALIDARG;
    if (!Locked(TS_LF_READ)) return TS_E_NOLOCK;
    // Eight pixels a character, on one line.
    *rect = {100 + start * 8, 100, 100 + std::max(end, start + 1) * 8, 120};
    *clipped = FALSE;
    return S_OK;
}

STDMETHODIMP TextStore::GetScreenExt(TsViewCookie, RECT* rect) {
    if (!rect) return E_INVALIDARG;
    *rect = {0, 0, 1024, 768};
    return S_OK;
}

STDMETHODIMP TextStore::GetWnd(TsViewCookie, HWND* window) {
    if (!window) return E_INVALIDARG;
    *window = nullptr;
    return S_OK;
}

STDMETHODIMP TextStore::OnStartComposition(ITfCompositionView* composition, BOOL* ok) {
    if (!ok) return E_INVALIDARG;
    if (refuse_) {
        *ok = FALSE;
        return S_OK;
    }
    composition_ = composition;
    ++started_;
    *ok = TRUE;
    return S_OK;
}

STDMETHODIMP TextStore::OnUpdateComposition(ITfCompositionView*, ITfRange*) { return S_OK; }

STDMETHODIMP TextStore::OnEndComposition(ITfCompositionView*) {
    composition_.Reset();
    return S_OK;
}

}  // namespace test
