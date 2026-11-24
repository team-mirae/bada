// The published state's shared memory, as the text service writes it and another program reads it
// (docs/bada-compatible-v2.md §6.7, §12). The names here are the test's own, so the state of an
// input method that is in use is left alone.
//
//   bada_state_tests
#include <windows.h>

#include <string>
#include <thread>

#include "check.h"
#include "guids.h"
#include "state_shared.h"

using namespace bada;
using test::Check;

namespace {

UINT g_message = 0;
int g_heard = 0;
WPARAM g_thread = 0;
LPARAM g_flags = 0;

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_message) {
        ++g_heard;
        g_thread = wparam;
        g_flags = lparam;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Pump() {
    MSG msg;
    for (int i = 0; i < 20; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(5);
    }
}

}  // namespace

int main() {
    const std::wstring suffix = L".test." + std::to_wstring(GetCurrentProcessId());
    const std::wstring name = std::wstring(tip::kStateMapping) + suffix;
    const std::wstring lock = std::wstring(tip::kStateLock) + suffix;
    Check(std::wstring(tip::kStateMapping) == L"Local\\com.teammirae.bada.state.v2.{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}" &&
              std::wstring(tip::kStateMapping).find(tip::kClsidText) != std::wstring::npos,
          "the mapping's name is the standard's, with bada's CLSID");

    // A top-level window hears the state message (a message-only window gets no broadcasts).
    g_message = tip::StateMessage();
    Check(g_message >= 0xC000 && g_message == RegisterWindowMessageW(L"com.teammirae.bada.state"),
          "the state message is the registered one");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"com.teammirae.bada.state-test";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);

    Check(!tip::ReadSharedState(name.c_str()).has_value(), "before anything is published there is no mapping");
    tip::SharedState sink(name.c_str(), lock.c_str());
    StateSummary summary;
    summary.mode = Mode::kHenkan;
    summary.active = true;
    summary.composing = true;
    const std::string json = R"({"format":"bada-compatible-state/2","staged":"한"})";
    sink.Publish(json, summary);
    auto read = tip::ReadSharedState(name.c_str());
    Check(read && read->json == json && read->thread_id == GetCurrentThreadId() && read->process_id == GetCurrentProcessId() &&
              read->flags == (state::kFlagHenkan | state::kFlagActive | state::kFlagComposing),
          "a state is published, and another program reads it by the mapping's name");
    Pump();
    if (window) {
        Check(g_heard == 1 && g_thread == GetCurrentThreadId() &&
                  g_flags == static_cast<LPARAM>(state::kFlagHenkan | state::kFlagActive | state::kFlagComposing),
              "the state message is posted, with the thread and the flags");
    } else {
        std::printf("(no window could be made here: the state message isn't checked)\n");
    }
    // Typing within a composition changes the text, not what the message carries: no message.
    sink.Publish(R"({"format":"bada-compatible-state/2","staged":"한국"})", summary);
    Pump();
    Check(tip::ReadSharedState(name.c_str())->json.find("한국") != std::string::npos, "the text changes");
    if (window) Check(g_heard == 1, "…with no message: the flags are the same");
    summary.composing = false;
    sink.Publish(R"({"format":"bada-compatible-state/2","staged":""})", summary);
    Pump();
    if (window) Check(g_heard == 2, "a change of the flags posts the message");

    // Another thread: its field loses focus after this one's gained it. The state that shows is
    // the one of the field in focus.
    std::thread other([&] {
        StateSummary idle;
        idle.mode = Mode::kHenkan;
        sink.Publish(R"({"other":"idle"})", idle);
    });
    other.join();
    Check(tip::ReadSharedState(name.c_str())->thread_id == GetCurrentThreadId(),
          "a thread with no field in focus doesn't write over the one that has it");
    std::thread focused([&] {
        StateSummary active;
        active.active = true;
        sink.Publish(R"({"other":"active"})", active);
    });
    focused.join();
    read = tip::ReadSharedState(name.c_str());
    Check(read && read->json == R"({"other":"active"})", "a thread whose field has focus does");
    // That thread is gone now: its state is nobody's, and an idle one may replace it.
    StateSummary idle;
    sink.Publish(R"({"idle":true})", idle);
    read = tip::ReadSharedState(name.c_str());
    Check(read && read->json == R"({"idle":true})" && read->flags == 0, "the state of a thread that ended is replaced");

    // A state too long for the block loses its menu's rows.
    std::string big = R"({"menu":{"rows":[)";
    for (int i = 0; i < 4000; ++i) big += std::string(i ? "," : "") + R"({"text":"0123456789abcdef"})";
    big += R"(],"selected":0,"strip":false},"staged":"x"})";
    sink.Publish(big, summary);
    read = tip::ReadSharedState(name.c_str());
    Check(read && read->json == R"({"menu":{"rows":[],"selected":null,"strip":false},"staged":"x"})",
          "a state too long for the block is published without its rows");

    // Turned off: nothing is published.
    sink.Clear();
    Check(!tip::ReadSharedState(name.c_str()).has_value(), "cleared: nothing is published");
    if (window) DestroyWindow(window);
    return test::Finish("state");
}
