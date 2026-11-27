// bada-tide's device side on Linux (docs/bada-tide-v1.md): the keyboards the settings folder's
// `tide/*.json` match, found through /sys/class/hidraw and opened as /dev/hidrawN, each with its
// lease (src/core/tide.h decides everything; this is the devices, the clock and the programs).
//
// It runs in the frontend's own process (Fcitx5, or ibus-engine-bada): on Linux one process holds
// every text field, so the link needs no helper. The frontend adds fd() to its event loop, calls
// Dispatch() when it is readable, and tells the hub the harness's state with SetHost(). The hub
// hands it each key bada takes.
//
// hidraw is opened non-exclusively: other programs still reach the keyboard. Reads never block; a
// send goes on the device's own thread, so a keyboard that doesn't take it holds up nothing (§6: a
// renewal that can't go now goes at the next one).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "tide.h"

namespace bada {

class TideHub {
public:
    struct Options {
        std::filesystem::path settings_dir;  // the settings folder; translators are in its `tide/`
        // /sys/class/hidraw and /dev, for the tests. Empty: the system's.
        std::filesystem::path sys_dir, dev_dir;
        // /proc, for the tests. Empty: the system's.
        std::filesystem::path proc_dir;
        // One line for the log.
        std::function<void(const std::string&)> log;
        // A key bada takes: its press, or the release of a press that ran.
        std::function<void(const tide::Link::Key&)> key;
        // The status (Status()) changed.
        std::function<void()> changed;
    };
    explicit TideHub(Options options);
    ~TideHub();
    TideHub(const TideHub&) = delete;
    TideHub& operator=(const TideHub&) = delete;

    // Readable when the hub has something to do: a report, a tick.
    int fd() const { return epoll_; }
    void Dispatch();
    // The harness's state (bada-tide §2). A change is sent at once (§3).
    void SetHost(const tide::Host& host);
    // The settings folder changed: the translators are read again.
    void Reload();
    // bada-tide's status for the published state (§8): {"links": [...], "problems": [...]}.
    std::string Status() const { return status_; }

private:
    struct Device;
    struct Problem {
        std::string file, path, code, message;
    };
    void Log(const std::string& line) const;
    bool LoadTranslators();
    void Scan();
    void Close(Device& d);
    void CloseAll();
    void Tick();
    void ReadFrom(Device& d);
    void Send(Device& d, std::vector<std::uint8_t> report);
    void ArmTimer();
    void PublishStatus();
    std::set<std::string> RunningPrograms() const;
    std::uint64_t Now() const;

    Options options_;
    int epoll_ = -1, timer_ = -1;
    std::vector<tide::Translator> translators_;
    std::vector<Problem> problems_;
    std::string loaded_key_;
    std::vector<std::unique_ptr<Device>> devices_;
    tide::Host host_;
    std::uint64_t last_scan_ = 0;
    bool fast_ = false;  // the timer runs four times a second
    std::string status_ = R"({"links":[],"problems":[]})";
    // Problems with a device that isn't linked (no permission), by its node, logged once.
    std::set<std::string> unopened_;
};

}  // namespace bada
