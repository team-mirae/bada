#include "tide_hub.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "json.h"

namespace fs = std::filesystem;

namespace bada {
namespace {

constexpr int kTickMs = 250;      // with a keyboard linked
constexpr int kIdleMs = 2000;     // without: the translators and the devices are looked at
constexpr int kScanMs = 2000;     // devices that came or went
constexpr std::uint64_t kTimerTag = 0;

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::uint8_t> ReadBytes(const fs::path& path) {
    const std::string s = ReadFile(path);
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// HID_ID=0003:000004FE:00000022 in the HID device's uevent: bus, vendor, product.
bool ReadIds(const fs::path& uevent, int* vendor, int* product) {
    std::istringstream in(ReadFile(uevent));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("HID_ID=", 0) != 0) continue;
        unsigned bus = 0, v = 0, p = 0;
        if (std::sscanf(line.c_str() + 7, "%x:%x:%x", &bus, &v, &p) != 3) return false;
        *vendor = static_cast<int>(v);
        *product = static_cast<int>(p);
        return true;
    }
    return false;
}

// The release number of the USB device the HID device belongs to (its bcdDevice), if it is one.
std::optional<int> ReadVersion(const fs::path& hid_device) {
    std::error_code ec;
    fs::path p = fs::canonical(hid_device, ec);
    if (ec) return std::nullopt;
    for (int up = 0; up < 4 && !p.empty() && p != p.root_path(); ++up, p = p.parent_path()) {
        const fs::path file = p / "bcdDevice";
        if (fs::exists(file, ec)) {
            const std::string text = ReadFile(file);
            try {
                return std::stoi(text, nullptr, 16);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

struct TideHub::Device {
    std::uint64_t serial = 0;
    std::string node;  // hidraw2
    fs::path path;     // /dev/hidraw2
    int fd = -1;
    size_t report_size = 0;
    std::unique_ptr<tide::Link> link;

    // The sender: one report waits at most; a newer one takes its place.
    std::thread sender;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<std::vector<std::uint8_t>> pending;
    bool stop = false;
    std::atomic<bool> failed{false};
    bool gone = false;
};

TideHub::TideHub(Options options) : options_(std::move(options)) {
    if (options_.sys_dir.empty()) options_.sys_dir = "/sys/class/hidraw";
    if (options_.dev_dir.empty()) options_.dev_dir = "/dev";
    if (options_.proc_dir.empty()) options_.proc_dir = "/proc";
    epoll_ = epoll_create1(EPOLL_CLOEXEC);
    timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (epoll_ >= 0 && timer_ >= 0) {
        epoll_event e = {};
        e.events = EPOLLIN;
        e.data.u64 = kTimerTag;
        epoll_ctl(epoll_, EPOLL_CTL_ADD, timer_, &e);
    }
    LoadTranslators();
    Scan();
    ArmTimer();
    PublishStatus();
}

TideHub::~TideHub() {
    CloseAll();
    if (timer_ >= 0) close(timer_);
    if (epoll_ >= 0) close(epoll_);
}

void TideHub::Log(const std::string& line) const {
    if (options_.log) options_.log("tide: " + line);
}

std::uint64_t TideHub::Now() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Reads tide/*.json again. True when the files changed.
bool TideHub::LoadTranslators() {
    const fs::path dir = options_.settings_dir / "tide";
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::string key;
    for (const fs::path& p : files) key += p.filename().string() + "\n" + ReadFile(p) + "\n\x1f";
    if (key == loaded_key_) return false;
    loaded_key_ = key;
    std::vector<tide::Translator> found;
    std::vector<Problem> problems;
    for (const fs::path& path : files) {
        const std::string file = "tide/" + path.filename().string();
        std::string why;
        bool denied = false;
        std::optional<tide::Translator> t = tide::Translator::Parse(ReadFile(path), &why, &denied);
        if (!t) {
            if (denied) {
                problems.push_back({file, why + ".send", "denied",
                                    "the " + why + " template begins like a request bada-tide never sends (§7)"});
            } else {
                problems.push_back({file, "", "bad-value", why});
            }
            Log(file + ": not used (" + why + ")");
            continue;
        }
        if (std::any_of(found.begin(), found.end(), [&](const tide::Translator& o) { return o.id == t->id; })) {
            problems.push_back({file, "id", "duplicate-translator", "another translator has the id " + t->id});
            continue;
        }
        Log(file + ": " + t->name);
        found.push_back(std::move(*t));
    }
    translators_ = std::move(found);
    problems_ = std::move(problems);
    return true;
}

void TideHub::Reload() {
    if (!LoadTranslators()) return;
    Log("the translators changed: opening the keyboards again");
    CloseAll();
    unopened_.clear();
    Scan();
    ArmTimer();
    PublishStatus();
}

// Finds the keyboards that match a translator and opens the new ones; forgets the ones gone.
void TideHub::Scan() {
    last_scan_ = Now();
    std::set<std::string> present;
    std::error_code ec;
    if (!translators_.empty()) {
        for (const auto& entry : fs::directory_iterator(options_.sys_dir, ec)) {
            const std::string node = entry.path().filename().string();
            if (node.rfind("hidraw", 0) == 0) present.insert(node);
        }
    }
    // Gone: a keyboard that comes back is a new one (bada-tide §3).
    for (auto it = devices_.begin(); it != devices_.end();) {
        Device& d = **it;
        if (d.gone || !present.count(d.node)) {
            Log(d.link->translator().id + ": keyboard gone (" + d.node + ")");
            Close(d);
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = unopened_.begin(); it != unopened_.end();) {
        it = present.count(*it) ? std::next(it) : unopened_.erase(it);
    }
    static std::uint64_t serial = 0;
    for (const std::string& node : present) {
        if (std::any_of(devices_.begin(), devices_.end(), [&](const auto& d) { return d->node == node; })) continue;
        const fs::path hid = options_.sys_dir / node / "device";
        tide::HidDevice device;
        if (!ReadIds(hid / "uevent", &device.vendor_id, &device.product_id)) continue;
        const std::optional<tide::HidInfo> info = tide::ParseDescriptor(ReadBytes(hid / "report_descriptor"));
        if (!info) continue;
        device.info = *info;
        device.version = ReadVersion(hid);
        for (const tide::Translator& t : translators_) {
            if (!tide::Matches(t.match, device)) continue;
            const fs::path path = options_.dev_dir / node;
            // Non-exclusively, as hidraw always is: other programs reach the keyboard too.
            const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                const int error = errno;
                if (unopened_.insert(node).second) {
                    Log(t.id + ": " + path.string() + " can't be opened: " + std::strerror(error));
                }
                break;
            }
            auto d = std::make_unique<Device>();
            d->serial = ++serial;
            d->node = node;
            d->path = path;
            d->fd = fd;
            d->report_size = static_cast<size_t>(t.match.report_size);
            d->link = std::make_unique<tide::Link>(t);
            Device* raw = d.get();
            d->sender = std::thread([raw] {
                std::vector<std::uint8_t> out;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(raw->mutex);
                        raw->wake.wait(lock, [raw] { return raw->stop || raw->pending; });
                        if (raw->stop) return;
                        out = std::move(*raw->pending);
                        raw->pending.reset();
                    }
                    // The report id first: 0, the device has none (Matches).
                    std::vector<std::uint8_t> buffer(raw->report_size + 1, 0);
                    std::copy_n(out.begin(), std::min(out.size(), raw->report_size), buffer.begin() + 1);
                    if (write(raw->fd, buffer.data(), buffer.size()) != static_cast<ssize_t>(buffer.size())) {
                        raw->failed = true;
                    }
                }
            });
            epoll_event e = {};
            e.events = EPOLLIN;
            e.data.u64 = d->serial;
            epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e);
            unopened_.erase(node);
            Log(t.id + ": keyboard found (" + path.string() + ")");
            devices_.push_back(std::move(d));
            break;
        }
    }
}

void TideHub::Close(Device& d) {
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.stop = true;
    }
    d.wake.notify_all();
    if (d.sender.joinable()) d.sender.join();
    if (d.fd >= 0) {
        epoll_ctl(epoll_, EPOLL_CTL_DEL, d.fd, nullptr);
        close(d.fd);
    }
    d.fd = -1;
}

void TideHub::CloseAll() {
    for (auto& d : devices_) Close(*d);
    devices_.clear();
}

void TideHub::Send(Device& d, std::vector<std::uint8_t> report) {
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.pending = std::move(report);
    }
    d.wake.notify_one();
}

void TideHub::ArmTimer() {
    if (timer_ < 0) return;
    const bool fast = !devices_.empty();
    const int ms = fast ? kTickMs : kIdleMs;
    itimerspec spec = {};
    spec.it_interval.tv_sec = ms / 1000;
    spec.it_interval.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    spec.it_value = spec.it_interval;
    timerfd_settime(timer_, 0, &spec, nullptr);
    fast_ = fast;
}

std::set<std::string> TideHub::RunningPrograms() const {
    std::set<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(options_.proc_dir, ec)) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        // The kernel's name (15 bytes at most), the first argument's file name, and the
        // executable's, so that a conflict names the program as its users know it.
        const std::string comm = Trim(ReadFile(entry.path() / "comm"));
        if (!comm.empty()) names.insert(comm);
        const std::string cmdline = ReadFile(entry.path() / "cmdline");
        const std::string argv0 = cmdline.substr(0, cmdline.find('\0'));
        if (!argv0.empty()) names.insert(fs::path(argv0).filename().string());
        const fs::path exe = fs::read_symlink(entry.path() / "exe", ec);
        if (!ec && !exe.empty()) names.insert(exe.filename().string());
    }
    return names;
}

// Four times a second while a keyboard is linked, after every report and every change: conflicts,
// hellos, renewals. The running programs are read every tick, so before every hello too (§5).
void TideHub::Tick() {
    const std::uint64_t now = Now();
    if (now - last_scan_ >= kScanMs) {
        const size_t before = devices_.size();
        Scan();
        if ((devices_.size() > 0) != fast_ || devices_.size() != before) ArmTimer();
    }
    if (devices_.empty()) {
        PublishStatus();
        return;
    }
    const bool conflicts = std::any_of(translators_.begin(), translators_.end(), [](const tide::Translator& t) {
        return std::any_of(t.conflicts.begin(), t.conflicts.end(), [](const auto& c) { return !c.processes.empty(); });
    });
    const std::set<std::string> running = conflicts ? RunningPrograms() : std::set<std::string>{};
    for (auto& d : devices_) {
        tide::Link& link = *d->link;
        if (d->failed.exchange(false)) {
            Log(link.translator().id + ": send failed");
            link.SendFailed();
        }
        if (std::optional<std::vector<std::uint8_t>> r = link.Tick(now, link.translator().Running(running), host_)) {
            Send(*d, std::move(*r));
        }
        const std::string note = link.TakeNote();
        if (!note.empty()) Log(link.translator().id + ": " + note);
    }
    PublishStatus();
}

void TideHub::ReadFrom(Device& d) {
    std::vector<std::uint8_t> buffer(std::max<size_t>(d.report_size, 64) + 1);
    for (;;) {
        const ssize_t n = read(d.fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            d.gone = true;  // unplugged: ENODEV or EIO
            return;
        }
        if (n == 0) return;
        const std::vector<std::uint8_t> report(buffer.begin(), buffer.begin() + n);
        const std::optional<tide::Link::Key> key = d.link->Received(Now(), report);
        const std::string note = d.link->TakeNote();
        if (!note.empty()) Log(d.link->translator().id + ": " + note);
        if (key && options_.key) options_.key(*key);
    }
}

void TideHub::Dispatch() {
    epoll_event events[8];
    const int n = epoll_wait(epoll_, events, 8, 0);
    for (int i = 0; i < n; ++i) {
        if (events[i].data.u64 == kTimerTag) {
            std::uint64_t expirations = 0;
            if (read(timer_, &expirations, sizeof(expirations)) < 0) {
                // nothing: the timer fires again
            }
            continue;
        }
        for (auto& d : devices_) {
            if (d->serial != events[i].data.u64) continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) d->gone = true;
            else ReadFrom(*d);
        }
    }
    if (std::any_of(devices_.begin(), devices_.end(), [](const auto& d) { return d->gone; })) {
        Scan();
        ArmTimer();
    }
    Tick();  // a hello answered: the state goes now
}

void TideHub::SetHost(const tide::Host& host) {
    if (host == host_) return;
    host_ = host;
    if (!devices_.empty()) Tick();  // renew at once with the new state (§3)
}

void TideHub::PublishStatus() {
    json::Value links = json::Array();
    for (const auto& d : devices_) links.Push(d->link->Published());
    json::Value problems = json::Array();
    for (const Problem& p : problems_) {
        json::Value item = json::Object();
        item.Set("file", json::Str(p.file)).Set("path", json::Str(p.path)).Set("code", json::Str(p.code));
        item.Set("message", json::Str(p.message)).Set("level", json::Str("error"));
        problems.Push(std::move(item));
    }
    // A keyboard that matched and couldn't be opened: hidraw needs the udev rule (the README).
    for (const std::string& node : unopened_) {
        json::Value item = json::Object();
        item.Set("file", json::Str("")).Set("path", json::Str("")).Set("code", json::Str("tide-no-access"));
        item.Set("message", json::Str((options_.dev_dir / node).string() +
                                      " matches a translator and can't be opened: install bada's udev rule"));
        item.Set("level", json::Str("warning"));
        problems.Push(std::move(item));
    }
    json::Value status = json::Object();
    status.Set("links", std::move(links)).Set("problems", std::move(problems));
    std::string text = json::Write(status);
    if (text == status_) return;
    status_ = std::move(text);
    if (options_.changed) options_.changed();
}

}  // namespace bada
