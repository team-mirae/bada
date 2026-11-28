// bada-tide.exe: bada-tide's device side on Windows (docs/bada-tide-v1.md). One per logon session.
// It reads the settings folder's `tide/*.json`, finds the keyboards they match by their USB
// descriptors, holds each lease (`hello` after every connect, then `state` every third of the
// lease and at once on every change), and hands each key bada takes to the text service of the
// thread that has the keyboard focus. Everything it decides is src/core/tide.h; this is the
// devices, the clock and the messages (src/core/tide_ipc.h).
//
// A separate process, as on macOS: the text service lives in every app, and one keyboard has one
// link. The text service starts it (TextService::EnsureTideHelper) while the folder has a
// translator. It ends when the folder has none; its leases then run out by themselves.
//
// The keyboard is opened non-exclusively (FILE_SHARE_READ | FILE_SHARE_WRITE), so other programs
// (PFU's tool, a status script) still reach it. A send waits at most 250 ms.
//
//   bada-tide.exe             runs until the folder has no translator
//   bada-tide.exe --verbose   the same, and says what happens on stderr
#include <windows.h>

#include <dbt.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <shellapi.h>
#include <shlobj.h>

#include "json.h"
#include "state_block.h"
#include "tide.h"
#include "tide_ipc.h"

namespace fs = std::filesystem;
using namespace bada;
using namespace bada::tide;

namespace {

constexpr UINT_PTR kTimer = 1;
constexpr UINT kTickMs = 250;
constexpr UINT WM_TIDE_REPORT = WM_APP + 1;  // lParam: Report*, from a reader thread
constexpr UINT WM_TIDE_GONE = WM_APP + 2;    // wParam: the device's serial
constexpr wchar_t kStateMapping[] = L"Local\\com.teammirae.bada.state.v2.{5B6D78A1-D25C-40FC-B4FF-4AA02120BD84}";
constexpr wchar_t kStateMessage[] = L"com.teammirae.bada.state";
// What marks a key the text service injected itself (src/tip/guids.h): the helper's own
// fallback is not marked, so a text service reads it like the key it is.

bool g_verbose = false;
fs::path g_log_file;
HWND g_window = nullptr;
UINT g_host_message = 0, g_key_message = 0, g_ask_message = 0, g_status_message = 0, g_state_message = 0;

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

void Note(const char* format, ...) {
    if (!g_verbose && g_log_file.empty()) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (g_verbose) std::fprintf(stderr, "%s%s\n", stamp, line);
    if (!g_log_file.empty()) {
        std::ofstream out(g_log_file, std::ios::app | std::ios::binary);
        out << stamp << GetCurrentProcessId() << " bada-tide.exe " << line << "\n";
    }
}

// The settings folder (the standard, §10.1), as the text service finds it.
fs::path SettingsDir() {
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"BADA_CONFIG_DIR", buffer, MAX_PATH);
    if (n && n < MAX_PATH) return fs::path(std::wstring(buffer, n));
    PWSTR roaming = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &roaming)) && roaming) {
        dir = fs::path(roaming) / L"bada";
    }
    CoTaskMemFree(roaming);
    return dir;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// The diagnostic log, as the text service has it: on with the harness setting "log", in
// %LOCALAPPDATA%\bada\bada.log; %BADA_LOG_FILE% names another file and turns it on by itself.
void FindLog(const fs::path& settings) {
    g_log_file.clear();
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"BADA_LOG_FILE", buffer, MAX_PATH);
    if (n && n < MAX_PATH) {
        g_log_file = fs::path(std::wstring(buffer, n));
        return;
    }
    const auto parsed = json::Parse(ReadFile(settings / L"settings.json"));
    const json::Value* harness = parsed ? parsed->Get("harness") : nullptr;
    const json::Value* ours = harness ? harness->Get("com.teammirae.bada.windows") : nullptr;
    if (!ours || !ours->GetBool("log")) return;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)) && local) {
        g_log_file = fs::path(local) / L"bada" / L"bada.log";
    }
    CoTaskMemFree(local);
}

// --- translators

struct Problem {
    std::string file, path, code, message;
};

std::vector<Translator> g_translators;
std::vector<Problem> g_problems;

bool HasTranslators(const fs::path& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == L".json") return true;
    }
    return false;
}

// Reads tide/*.json again. True when the set of translators changed.
bool LoadTranslators(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == L".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<Translator> found;
    std::vector<Problem> problems;
    for (const fs::path& path : files) {
        const std::string file = "tide/" + Utf8(path.filename().wstring());
        std::string why;
        bool denied = false;
        std::optional<Translator> t = Translator::Parse(ReadFile(path), &why, &denied);
        if (!t) {
            if (denied) {
                problems.push_back({file, why + ".send", "denied",
                                    "the " + why + " template begins like a request bada-tide never sends (§7)"});
            } else {
                problems.push_back({file, "", "bad-value", why});
            }
            Note("%s: not used (%s)", file.c_str(), why.c_str());
            continue;
        }
        if (std::any_of(found.begin(), found.end(), [&](const Translator& o) { return o.id == t->id; })) {
            problems.push_back({file, "id", "duplicate-translator", "another translator has the id " + t->id});
            continue;
        }
        found.push_back(std::move(*t));
    }
    // The same files read the same way: nothing to restart. (A translator's content is compared by
    // what it was read from.)
    static std::string last;
    std::string key;
    for (const fs::path& p : files) key += ReadFile(p) + "\n\x1f";
    const bool changed = key != last;
    last = key;
    g_translators = std::move(found);
    g_problems = std::move(problems);
    return changed;
}

// --- devices

struct Report {
    std::uint64_t serial;
    std::vector<std::uint8_t> bytes;
};

struct Device {
    std::uint64_t serial = 0;
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    size_t in_length = 0, out_length = 0;  // with the report id byte
    std::unique_ptr<Link> link;
    std::thread reader;
    // A taken key whose press went to this window: its release goes there too.
    std::map<std::string, HWND> pressed_to;
};

std::vector<std::unique_ptr<Device>> g_devices;
std::uint64_t g_next_serial = 1;

void ReadLoop(Device* d) {
    std::vector<std::uint8_t> buffer(d->in_length);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const std::uint64_t serial = d->serial;
    const HANDLE handles[2] = {ov.hEvent, d->stop};
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!::ReadFile(d->handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            const DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(d->handle, &ov);
                GetOverlappedResult(d->handle, &ov, &got, TRUE);
                CloseHandle(ov.hEvent);
                return;  // stopped
            }
            if (!GetOverlappedResult(d->handle, &ov, &got, FALSE)) break;
        }
        if (got > 1) {
            // The first byte is the report id (0: the device has none).
            auto* r = new Report{serial, std::vector<std::uint8_t>(buffer.begin() + 1, buffer.begin() + got)};
            if (!PostMessageW(g_window, WM_TIDE_REPORT, 0, reinterpret_cast<LPARAM>(r))) delete r;
        }
    }
    CloseHandle(ov.hEvent);
    PostMessageW(g_window, WM_TIDE_GONE, static_cast<WPARAM>(serial), 0);
}

void Close(Device& d) {
    if (d.stop) SetEvent(d.stop);
    if (d.reader.joinable()) d.reader.join();
    if (d.handle != INVALID_HANDLE_VALUE) CloseHandle(d.handle);
    if (d.stop) CloseHandle(d.stop);
    d.handle = INVALID_HANDLE_VALUE;
    d.stop = nullptr;
}

void CloseAll() {
    for (auto& d : g_devices) Close(*d);
    g_devices.clear();
}

// Sends one report: never longer than 250 ms (bada-tide §6: a send that can't go now goes at the
// next renewal).
bool Send(Device& d, const std::vector<std::uint8_t>& report) {
    std::vector<std::uint8_t> out(d.out_length, 0);
    std::copy_n(report.begin(), std::min(report.size(), out.size() - 1), out.begin() + 1);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD wrote = 0;
    bool ok = WriteFile(d.handle, out.data(), static_cast<DWORD>(out.size()), &wrote, &ov) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 250) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(d.handle, &ov, &wrote, FALSE) != FALSE;
        } else {
            CancelIoEx(d.handle, &ov);
            GetOverlappedResult(d.handle, &ov, &wrote, TRUE);
            ok = false;
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok) Note("%s: send failed", d.link->translator().id.c_str());
    return ok;
}

// What a HID interface is, read without opening it for reading or writing.
struct Info {
    int vendor = 0, product = 0, version = 0, usage_page = 0, usage = 0;
    size_t in_length = 0, out_length = 0;
};

std::optional<Info> Describe(const std::wstring& path) {
    const HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    Info info;
    HIDD_ATTRIBUTES attributes = {sizeof(attributes)};
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS caps = {};
    const bool ok = HidD_GetAttributes(h, &attributes) && HidD_GetPreparsedData(h, &preparsed) &&
                    HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
    if (preparsed) HidD_FreePreparsedData(preparsed);
    CloseHandle(h);
    if (!ok) return std::nullopt;
    info.vendor = attributes.VendorID;
    info.product = attributes.ProductID;
    info.version = attributes.VersionNumber;
    info.usage_page = caps.UsagePage;
    info.usage = caps.Usage;
    info.in_length = caps.InputReportByteLength;
    info.out_length = caps.OutputReportByteLength;
    return info;
}

std::vector<std::wstring> HidPaths() {
    std::vector<std::wstring> paths;
    GUID hid;
    HidD_GetHidGuid(&hid);
    const HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;
    SP_DEVICE_INTERFACE_DATA item = {sizeof(item)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &item); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<std::uint8_t> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, nullptr)) {
            paths.push_back(Lower(detail->DevicePath));
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

void PublishStatus();

// Finds the keyboards that match a translator and opens the new ones; forgets the ones gone.
void Scan() {
    const std::vector<std::wstring> paths = HidPaths();
    bool changed = false;
    // Gone: a keyboard that comes back is a new one (bada-tide §3).
    for (auto it = g_devices.begin(); it != g_devices.end();) {
        if (std::find(paths.begin(), paths.end(), (*it)->path) == paths.end()) {
            Note("%s: keyboard gone", (*it)->link->translator().id.c_str());
            Close(**it);
            it = g_devices.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (const std::wstring& path : paths) {
        if (std::any_of(g_devices.begin(), g_devices.end(), [&](const auto& d) { return d->path == path; })) continue;
        const std::optional<Info> info = Describe(path);
        if (!info) continue;
        for (const Translator& t : g_translators) {
            const Translator::Match& m = t.match;
            if (info->vendor != m.vendor_id || info->product != m.product_id || info->usage_page != m.usage_page ||
                info->usage != m.usage || !m.Accepts(info->version)) {
                continue;
            }
            // The report size, without the report id byte.
            if (info->in_length != static_cast<size_t>(m.report_size) + 1 ||
                info->out_length != static_cast<size_t>(m.report_size) + 1) {
                continue;
            }
            const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                Note("%s: couldn't open the keyboard (%lu)", t.id.c_str(), GetLastError());
                break;
            }
            auto d = std::make_unique<Device>();
            d->serial = g_next_serial++;
            d->path = path;
            d->handle = h;
            d->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            d->in_length = info->in_length;
            d->out_length = info->out_length;
            d->link = std::make_unique<Link>(t);
            d->reader = std::thread(ReadLoop, d.get());
            Note("%s: keyboard connected (%04x:%04x rev %04x)", t.id.c_str(), info->vendor, info->product, info->version);
            g_devices.push_back(std::move(d));
            changed = true;
            break;
        }
    }
    if (changed) PublishStatus();
}

// --- the text services

std::map<DWORD, Host> g_hosts;  // by thread: what each text service last said
DWORD g_focus_thread = 0;      // the thread whose text service has the keyboard focus; 0 none
Host g_last_host;              // the last host known, for when no text service has focus

// The thread whose text service has the keyboard's focus, from the published state: the field
// with focus is the one that shows (src/tip/state_shared.h), and its program is in the foreground.
DWORD FocusThread(Host* from_state) {
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kStateMapping);
    if (!mapping) return 0;
    DWORD thread = 0;
    if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) {
        if (const std::optional<state::Snapshot> s = state::Read(view, state::kBlockSize)) {
            from_state->mode = (s->flags & state::kFlagHenkan) ? Mode::kHenkan : Mode::kMuhenkan;
            from_state->caps = (s->flags & state::kFlagCapsLock) != 0;
            DWORD foreground = 0;
            if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &foreground);
            if ((s->flags & state::kFlagActive) && s->process_id == foreground) thread = s->thread_id;
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return thread;
}

HWND ThreadWindow(DWORD thread) {
    return thread ? FindWindowExW(HWND_MESSAGE, nullptr, ipc::kThreadClass, std::to_wstring(thread).c_str()) : nullptr;
}

// The state every link sends now (bada-tide §2): the focused text service's. With no text service
// of bada in focus, bada takes nothing, so the keys are Windows' (as macOS takes them only while
// one of bada's modes is the input source).
Host CurrentHost() {
    Host from_state = g_last_host;
    const DWORD focus = FocusThread(&from_state);
    if (focus != g_focus_thread) {
        g_focus_thread = focus;
        if (focus && !g_hosts.count(focus)) {
            if (HWND w = ThreadWindow(focus)) PostMessageW(w, g_ask_message, 0, 0);
        }
    }
    if (focus) {
        const auto it = g_hosts.find(focus);
        if (it != g_hosts.end()) {
            g_last_host = it->second;
            return it->second;
        }
    }
    Host host = from_state;
    host.take.clear();
    return host;
}

// Hands a taken key to the text service in focus; a release goes where its press went. If there
// is none (focus moved to a program without bada in between), the key goes to Windows as a key.
void Deliver(Device& d, const Link::Key& key) {
    HWND target = nullptr;
    if (key.down) {
        target = ThreadWindow(g_focus_thread);
        d.pressed_to[key.code] = target;
    } else {
        const auto it = d.pressed_to.find(key.code);
        if (it != d.pressed_to.end()) {
            target = it->second;
            d.pressed_to.erase(it);
        }
    }
    WPARAM wparam = 0;
    LPARAM lparam = 0;
    ipc::PackKey({key.code, key.down, key.mods}, reinterpret_cast<std::uintptr_t*>(&wparam),
                 reinterpret_cast<std::intptr_t*>(&lparam));
    if (target && PostMessageW(target, g_key_message, wparam, lparam)) {
        Note("%s: %s %s -> thread window %p", d.link->translator().id.c_str(), key.code.c_str(),
             key.down ? "down" : "up", static_cast<void*>(target));
        return;
    }
    // By scan code, as the keyboard would have sent it (src/core/keys.cpp).
    static const std::map<std::string, WORD> scans = {{"Backquote", 0x29}, {"NonConvert", 0x7B}, {"Convert", 0x79},
                                                      {"KanaMode", 0x70},  {"Lang2", 0x71},      {"Lang1", 0x72},
                                                      {"CapsLock", 0x3A}};
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scans.at(key.code);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (key.down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
    Note("%s: %s %s -> Windows (no text service of bada in focus)", d.link->translator().id.c_str(), key.code.c_str(),
         key.down ? "down" : "up");
}

// --- status

HANDLE g_status_mapping = nullptr;
void* g_status_view = nullptr;
std::string g_last_status;

void PublishStatus() {
    json::Value links = json::Array();
    for (const auto& d : g_devices) links.Push(d->link->Published());
    json::Value problems = json::Array();
    for (const Problem& p : g_problems) {
        json::Value item = json::Object();
        item.Set("file", json::Str(p.file)).Set("path", json::Str(p.path)).Set("code", json::Str(p.code));
        item.Set("message", json::Str(p.message)).Set("level", json::Str("error"));
        problems.Push(std::move(item));
    }
    json::Value status = json::Object();
    status.Set("links", std::move(links)).Set("problems", std::move(problems));
    const std::string text = json::Write(status);
    if (text == g_last_status) return;
    g_last_status = text;
    if (!g_status_view) {
        g_status_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(ipc::kStatusSize), ipc::kStatusMapping);
        if (g_status_mapping) g_status_view = MapViewOfFile(g_status_mapping, FILE_MAP_WRITE, 0, 0, 0);
    }
    ipc::WriteStatus(g_status_view, text);
    std::string summary;
    for (const auto& d : g_devices) {
        summary += (summary.empty() ? "" : ", ") + d->link->translator().id + "=" + Link::PhaseName(d->link->phase());
    }
    Note("status: %s", summary.empty() ? "no keyboard" : summary.c_str());
    if (HWND w = ThreadWindow(g_focus_thread)) PostMessageW(w, g_status_message, 0, 0);
}

// --- the clock

std::set<std::string> RunningPrograms() {
    std::set<std::string> names;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W entry = {sizeof(entry)};
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        names.insert(Utf8(Lower(entry.szExeFile)));
    }
    CloseHandle(snap);
    return names;
}

fs::path g_tide_dir;

// Four times a second, after every report and every change: conflicts, hellos, renewals. The
// running programs are read every tick, so before every hello too (bada-tide §5).
void Tick() {
    if (g_devices.empty()) return;
    const Host host = CurrentHost();
    const std::set<std::string> running = RunningPrograms();
    const std::uint64_t now = GetTickCount64();
    for (auto& d : g_devices) {
        Link& link = *d->link;
        if (std::optional<std::vector<std::uint8_t>> r = link.Tick(now, link.translator().Running(running), host)) {
            if (!Send(*d, *r)) link.SendFailed();
        }
        const std::string note = link.TakeNote();
        if (!note.empty()) Note("%s: %s", link.translator().id.c_str(), note.c_str());
    }
    PublishStatus();
}

void Received(const Report& r) {
    for (auto& d : g_devices) {
        if (d->serial != r.serial) continue;
        const std::optional<Link::Key> key = d->link->Received(GetTickCount64(), r.bytes);
        const std::string note = d->link->TakeNote();
        if (!note.empty()) Note("%s: %s", d->link->translator().id.c_str(), note.c_str());
        if (key) Deliver(*d, *key);
        Tick();  // a hello answered: the state goes now
        return;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_host_message && message) {
        const DWORD thread = static_cast<DWORD>(wparam);
        const Host host = ipc::UnpackHost(static_cast<std::uint32_t>(lparam));
        const auto it = g_hosts.find(thread);
        if (it == g_hosts.end() || !(it->second == host)) {
            g_hosts[thread] = host;
            Tick();  // renew at once with the new state (bada-tide §3)
        }
        return 0;
    }
    if (message == g_state_message && message) {
        Tick();  // focus, mode or the lock changed somewhere
        return 0;
    }
    switch (message) {
        case WM_TIMER: {
            Tick();
            static ULONGLONG checked = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - checked >= 2000) {
                checked = now;
                // The folder decides: no translator, no helper. A changed one starts over.
                if (!HasTranslators(g_tide_dir)) {
                    Note("no translator in %s: stopping", Utf8(g_tide_dir.wstring()).c_str());
                    DestroyWindow(window);
                } else if (LoadTranslators(g_tide_dir)) {
                    Note("the translators changed: opening the keyboards again");
                    CloseAll();
                    Scan();
                }
                // Threads that are gone no longer hold a host.
                for (auto it = g_hosts.begin(); it != g_hosts.end();) {
                    it = ThreadWindow(it->first) ? std::next(it) : g_hosts.erase(it);
                }
            }
            return 0;
        }
        case WM_TIDE_REPORT: {
            std::unique_ptr<Report> r(reinterpret_cast<Report*>(lparam));
            Received(*r);
            return 0;
        }
        case WM_TIDE_GONE:
            Scan();
            return 0;
        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) Scan();
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) CloseAll();
            return 0;
        case WM_DESTROY:
            CloseAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--verbose") g_verbose = true;
    }
    LocalFree(argv);
    if (g_verbose && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    // One helper per logon session.
    const HANDLE single = CreateMutexW(nullptr, TRUE, ipc::kMutex);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    const fs::path settings = SettingsDir();
    g_tide_dir = settings / L"tide";
    FindLog(settings);
    if (!HasTranslators(g_tide_dir)) {
        Note("no translator in %s", Utf8(g_tide_dir.wstring()).c_str());
        return 0;
    }
    LoadTranslators(g_tide_dir);

    g_host_message = RegisterWindowMessageW(ipc::kHostMessage);
    g_key_message = RegisterWindowMessageW(ipc::kKeyMessage);
    g_ask_message = RegisterWindowMessageW(ipc::kAskMessage);
    g_status_message = RegisterWindowMessageW(ipc::kStatusMessage);
    g_state_message = RegisterWindowMessageW(kStateMessage);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = ipc::kHelperClass;
    if (!RegisterClassW(&wc)) return 1;
    // A top-level window that is never shown: the state message is posted to every top-level window.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, ipc::kHelperClass, L"bada-tide", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, instance, nullptr);
    if (!g_window) return 1;
    // Text services in elevated programs may tell it their state.
    ChangeWindowMessageFilterEx(g_window, g_host_message, MSGFLT_ALLOW, nullptr);

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    const HDEVNOTIFY notify = RegisterDeviceNotificationW(g_window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    Note("started; translators: %zu, settings in %s", g_translators.size(), Utf8(settings.wstring()).c_str());
    Scan();
    PublishStatus();
    SetTimer(g_window, kTimer, kTickMs, nullptr);
    // Every text service says what it wants: the ones running from before this helper started too.
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, ipc::kThreadClass, nullptr)) != nullptr;) {
        PostMessageW(w, g_ask_message, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (notify) UnregisterDeviceNotification(notify);
    // An empty status: no keyboard is linked now.
    if (g_status_view) ipc::WriteStatus(g_status_view, "");
    Note("stopped");
    return 0;
}
