// What every text service of one process shares: the harness's session (the engine's data and
// session, the settings folder, the mode and the profile) and the lock they take turns with. A
// text service is one per thread with TSF; the session is one per process (the standard, §3.1).
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "harness.h"

namespace bada::tip {

std::wstring Wide(const std::string& utf8);
std::string Utf8(const std::wstring& wide);
std::string Utf8(const wchar_t* text, size_t length);

// The running program's file name, lowercase ("notepad.exe").
const std::wstring& ExeName();

struct Process {
    // The session isn't made for threads: every call into it holds this.
    std::recursive_mutex mutex;
    // nullptr: the engine couldn't be used (another major version of the standard, or it refused
    // to start). The text service then passes every key on.
    std::unique_ptr<Session> session;
    std::string error;
    // session->switches() when the session's compartment was last written.
    std::uint64_t written_switches = 0;
    // Nothing composes in this program: one whose password boxes don't say they are one.
    bool off = false;
};

// The process's, made at the first call.
Process& GetProcess();
// The same if it was made already, nullptr otherwise.
Process* PeekProcess();
// Frees it when nothing uses the DLL (DllCanUnloadNow).
void ReleaseProcess();

// The key table's preset for `keyboard: "auto"` (the standard, §9.5): the JIS PC one when Windows
// reports a Japanese keyboard, the one with no language keys otherwise.
Preset DetectKeyboard();

}  // namespace bada::tip
