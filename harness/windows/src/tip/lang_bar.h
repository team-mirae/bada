// The button in the input indicator (the taskbar, where the Microsoft IMEs show あ / A): "A" in
// muhenkan, the first character of the profile's name in henkan. A click switches mode; a right
// click opens the input menu, a popup the text service shows itself (the Windows 8+ indicator only
// reports the click).
// Adapted from bada-windows src/tip/lang_bar.* (jis-moonshot's).
#pragma once

#include <msctf.h>
#include <ctfutb.h>

#include <functional>
#include <string>

namespace bada::tip {

class LangBarButton final : public ITfLangBarItemButton, public ITfSource {
public:
    // `init_menu` fills the menu, `on_menu` runs the item picked, `on_click` a left click.
    LangBarButton(std::function<void(ITfMenu*)> init_menu, std::function<void(UINT)> on_menu,
                  std::function<void()> on_click);
    LangBarButton(const LangBarButton&) = delete;
    LangBarButton& operator=(const LangBarButton&) = delete;

    void SetState(const std::wstring& label, const std::wstring& tooltip);
    void Detach();  // drops the callbacks (the text service is going away)

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
    STDMETHODIMP GetStatus(DWORD* status) override;
    STDMETHODIMP Show(BOOL show) override;
    STDMETHODIMP GetTooltipString(BSTR* tooltip) override;
    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* area) override;
    STDMETHODIMP InitMenu(ITfMenu* menu) override;
    STDMETHODIMP OnMenuSelect(UINT id) override;
    STDMETHODIMP GetIcon(HICON* icon) override;
    STDMETHODIMP GetText(BSTR* text) override;
    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override;
    STDMETHODIMP UnadviseSink(DWORD cookie) override;

private:
    ~LangBarButton();
    LONG refs_ = 1;
    std::wstring label_ = L"?";
    std::wstring tooltip_;
    std::function<void(ITfMenu*)> init_menu_;
    std::function<void(UINT)> on_menu_;
    std::function<void()> on_click_;
    ITfLangBarItemSink* sink_ = nullptr;
};

}  // namespace bada::tip
