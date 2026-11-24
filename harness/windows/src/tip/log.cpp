#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>

#include "shared_session.h"

namespace bada::tip::log {
namespace {

std::atomic<bool> g_enabled{false};

// The file named by the environment, or "". Read once.
const std::wstring& FromEnvironment() {
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"BADA_LOG_FILE", buffer, MAX_PATH);
        return n && n < MAX_PATH ? std::wstring(buffer, n) : std::wstring();
    }();
    return path;
}

std::wstring Path() {
    if (!FromEnvironment().empty()) return FromEnvironment();
    wchar_t folder[MAX_PATH];
    const DWORD n = ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\bada", folder, MAX_PATH);
    if (!n || n > MAX_PATH) return {};
    CreateDirectoryW(folder, nullptr);
    return std::wstring(folder) + L"\\bada.log";
}

}  // namespace

void Enable(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

void Write(const wchar_t* format, ...) {
    if (!g_enabled.load(std::memory_order_relaxed) && FromEnvironment().empty()) return;
    const std::wstring path = Path();
    if (path.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1024];
    const int n = swprintf_s(line, L"%02u:%02u:%02u.%03u %lu %lu %s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                             GetCurrentProcessId(), GetCurrentThreadId(), ExeName().c_str());
    if (n < 0) return;
    va_list args;
    va_start(args, format);
    // _TRUNCATE: a line too long is cut, never the process ended.
    int m = _vsnwprintf_s(line + n, std::size(line) - n - 1, _TRUNCATE, format, args);
    va_end(args);
    if (m < 0) m = static_cast<int>(wcsnlen(line + n, std::size(line) - n - 1));
    size_t length = static_cast<size_t>(n + m);
    line[length++] = L'\n';
    char utf8[3072];
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, line, static_cast<int>(length), utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0) return;
    // FILE_APPEND_DATA: every write lands at the end, so processes don't overwrite each other.
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    CloseHandle(file);
}

}  // namespace bada::tip::log
