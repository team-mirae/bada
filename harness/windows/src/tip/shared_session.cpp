#include "shared_session.h"

#include <objbase.h>
#include <shlobj.h>

#include <cwctype>
#include <filesystem>

#include "log.h"
#include "state_shared.h"

namespace bada::tip {
namespace {

std::mutex g_guard;
Process* g_process = nullptr;

// Programs nothing composes in: the local agents of Korean certificate programs, whose password
// boxes don't mark themselves (jis-moonshot's list, through bada-windows).
constexpr const wchar_t* kOff[] = {
    L"unicrslocalserver.exe", L"unicrsnoticenter.exe", L"ccdaemon.exe", L"magicline4nx.exe", L"skcertservice.exe",
};

// The settings folder (the standard, §10.1): %APPDATA%\bada. %BADA_CONFIG_DIR% names another, for
// tests. In a sandboxed program (a Store app) Windows gives that program a folder of its own, where
// there are no settings: the input method is a new install there.
std::filesystem::path SettingsDir() {
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"BADA_CONFIG_DIR", buffer, MAX_PATH);
    if (n && n < MAX_PATH) return std::filesystem::path(std::wstring(buffer, n));
    PWSTR roaming = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &roaming)) && roaming) {
        dir = std::filesystem::path(roaming) / L"bada";
    }
    CoTaskMemFree(roaming);
    return dir;
}

// The engine's data (the standard, §11): the folder `data` beside the DLL's own folder, which is
// where the installer puts it ({app}\data, with the DLLs in {app}\x64 and {app}\x86) and the
// build (build\data, with them in build\x64 and build\x86). %BADA_DATA_DIR% names another.
std::filesystem::path DataDir() {
    wchar_t buffer[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BADA_DATA_DIR", buffer, MAX_PATH);
    if (n && n < MAX_PATH) return std::filesystem::path(std::wstring(buffer, n));
    n = GetModuleFileNameW(DllInstance(), buffer, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buffer, n)).parent_path().parent_path() / L"data";
}

}  // namespace

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Utf8(const wchar_t* text, size_t length) {
    if (!text || !length) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Utf8(const std::wstring& s) { return Utf8(s.data(), s.size()); }

const std::wstring& ExeName() {
    static const std::wstring name = [] {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s(path, n);
        const size_t slash = s.find_last_of(L'\\');
        if (slash != std::wstring::npos) s.erase(0, slash + 1);
        for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }();
    return name;
}

Preset DetectKeyboard() {
    // 7: a Japanese keyboard (106 or 109 keys). Windows can't tell an Apple JIS keyboard from a
    // PC's: that one is the harness setting "keyboard": "jis-mac".
    return GetKeyboardType(0) == 7 ? Preset::kJisPc : Preset::kNoLanguageKeys;
}

Process& GetProcess() {
    std::lock_guard<std::mutex> guard(g_guard);
    if (g_process) return *g_process;
    auto* process = new Process();
    for (const wchar_t* name : kOff) process->off |= ExeName() == name;
    Session::Options options;
    options.config_dir = SettingsDir();
    options.data_dir = DataDir();
    options.version = BADA_VERSION;
    options.detected = DetectKeyboard();
    options.sink = &StateSinkOfProcess();
    options.log = [](const std::string& line) { log::Write(L"%s", Wide(line).c_str()); };
    const ULONGLONG start = GetTickCount64();
    process->session = Session::Create(std::move(options), &process->error);
    if (process->session) {
        log::Enable(process->session->harness().log);
        log::Write(L"session started in %llu ms, settings in %s", GetTickCount64() - start,
                   process->session->folder().dir().c_str());
    } else {
        // Nothing to read the log setting from: this line is written only when the log is on by
        // its environment variable.
        log::Write(L"the engine can't be used: %s", Wide(process->error).c_str());
    }
    g_process = process;
    return *g_process;
}

Process* PeekProcess() {
    std::lock_guard<std::mutex> guard(g_guard);
    return g_process;
}

void ReleaseProcess() {
    std::lock_guard<std::mutex> guard(g_guard);
    delete g_process;
    g_process = nullptr;
}

}  // namespace bada::tip
