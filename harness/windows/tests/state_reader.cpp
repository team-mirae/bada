// bada_state: reads the published state as another program would (docs/bada-compatible-v2.md
// §6.7): the shared memory by its name, with the system's own calls.
//
//   bada_state            the state now, once
//   bada_state --watch    again whenever the state message arrives, until Ctrl+C
#include <windows.h>

#include <cstdio>
#include <string>

#include "guids.h"
#include "state_shared.h"

namespace {

void Show() {
    const auto snapshot = bada::tip::ReadSharedState(bada::tip::kStateMapping);
    if (!snapshot) {
        std::printf("nothing is published (bada hasn't run in this session, or publish_state is off)\n");
        return;
    }
    std::printf("thread %lu of process %lu, flags %#lx\n%s\n", static_cast<unsigned long>(snapshot->thread_id),
                static_cast<unsigned long>(snapshot->process_id), static_cast<unsigned long>(snapshot->flags),
                snapshot->json.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Show();
    if (argc < 2 || std::wstring(argv[1]) != L"--watch") return 0;
    // A message-only window can't receive broadcasts: a hidden top-level one.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"com.teammirae.bada.state-reader";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    const UINT changed = bada::tip::StateMessage();
    MSG msg;
    while (window && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == changed) {
            std::printf("-- thread %llu, flags %#llx\n", static_cast<unsigned long long>(msg.wParam),
                        static_cast<unsigned long long>(msg.lParam));
            Show();
        }
        DispatchMessageW(&msg);
    }
    return 0;
}
