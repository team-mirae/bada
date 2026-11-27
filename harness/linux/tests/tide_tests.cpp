// bada-tide on Linux: the core (src/core/tide.h): the translators in the repository, templates, the
// deny list (§7), the lease (§3, §4, §5) on a clock of the test's own with reports made by hand, the
// report descriptor and the keys; then the hub (src/common/tide_hub.h) on a /sys/class/hidraw and a
// /dev of the test's own, where the keyboard is a pseudo-terminal the test answers on.
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "keys.h"
#include "tide.h"
#include "tide_hub.h"

using namespace bada;
using namespace bada::tide;
using test::Check;
using test::CheckEq;

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::uint8_t> Bytes(std::initializer_list<int> list, size_t size = 64) {
    std::vector<std::uint8_t> out;
    for (int b : list) out.push_back(static_cast<std::uint8_t>(b));
    out.resize(size, 0);
    return out;
}

// The ikarosu translator as its repository publishes it, with a Linux conflict.
const char* kIkarosu = R"({"format": "bada-tide-translator/1", "id": "ikarosu", "name": "ikarosu",
 "match": {"vendor_id": 1278, "product_id": 34, "usage_page": 65280, "usage": 1, "report_size": 64},
 "version": {"major": 1, "minor": 0},
 "hello": {"send": "AA AA F1 01 02 {major} {minor}",
           "reply": "55 55 F1 {status} 01 06 {major} {minor} {features} {lease_ms:le16}"},
 "state": {"send": "AA AA F1 02 04 {mode} {profile} {caps} {take}", "reply": "55 55 F1 {status} 02 06"},
 "key": {"recv": "55 55 F1 00 10 09 {usage} {edge} {mods} {seq} {time_ms:le32} {flags}"},
 "values": {"status": {"ok": 0, "unsupported": 1, "no-lease": 2, "bad-value": 3, "blocked": 4},
            "mode": {"muhenkan": 0, "henkan": 1}, "profile_none": 255, "features": {"keys": 1, "take": 16},
            "usage": {"Backquote": 53, "NonConvert": 139, "Convert": 138, "KanaMode": 136, "Lang2": 145, "Lang1": 144, "CapsLock": 57},
            "take": {"Backquote": 1, "NonConvert": 2, "Convert": 4, "KanaMode": 8, "Lang2": 16, "Lang1": 32, "CapsLock": 64},
            "edge": {"up": 0, "down": 1}, "mods": "hid", "flags": {"dropped": 1}},
 "retry_ms": 10000,
 "conflicts": [{"id": "pfu-hhkb-keymap-tool", "name": "HHKB Keymap Tool", "reason": "uses the same vendor channel",
                "match": {"macos": {"bundle_id": ["jp.co.pfu.hhkb-keymap-tool"]}, "linux": {"process": ["hhkb-keymap-tool"]}}}]})";

void Templates() {
    const auto t = Template::Parse("AA AA F1 {status} {lease_ms:le16} {x:be16}");
    Check(t.has_value(), "a template parses");
    CheckEq(t->LiteralPrefix(), "AA AA F1", "its literal prefix");
    const auto r = t->Encode({{"status", 4}, {"lease_ms", 3000}, {"x", 0x1234}}, 10);
    Check(r && *r == std::vector<std::uint8_t>({0xAA, 0xAA, 0xF1, 4, 0xB8, 0x0B, 0x12, 0x34, 0, 0}), "encode, padded");
    const auto f = t->Decode(*r);
    Check(f && f->at("lease_ms") == 3000 && f->at("x") == 0x1234 && f->at("status") == 4, "decode");
    Check(!t->Decode(Bytes({0xAA, 0xAB})).has_value(), "a literal byte that differs: no match");
    Check(!t->Encode({{"status", 1}}, 10).has_value(), "a missing field: nothing");
    Check(!t->Encode({{"status", 1}, {"lease_ms", 1}, {"x", 1}}, 4).has_value(), "too long: nothing");
    Check(!Template::Parse("AA {x:le24}").has_value(), "an unknown width is refused");
    Check(!Template::Parse("AAA").has_value(), "a byte of three digits is refused");
    Check(!Template::Parse("").has_value(), "an empty template is refused");
}

void Translators() {
    std::string why;
    bool denied = false;
    for (const char* file : {"/tide/dmk-hhkb-hybrid.json", "/tide/ikarosu.json"}) {
        const auto t = Translator::Parse(ReadFile(std::string(BADA_ROOT) + file), &why, &denied);
        Check(t.has_value(), std::string("the repository's ") + file + " reads: " + why);
    }
    const auto t = Translator::Parse(kIkarosu, &why, &denied);
    Check(t.has_value(), "ikarosu's translator reads: " + why);
    Check(t->match.vendor_id == 0x04FE && t->match.product_id == 0x22 && t->match.usage_page == 0xFF00 &&
              t->match.usage == 1 && t->match.report_size == 64,
          "its match");
    Check(t->conflicts.size() == 1 && t->conflicts[0].processes == std::vector<std::string>{"hhkb-keymap-tool"},
          "its Linux conflict");
    Check(t->Running({"fcitx5", "hhkb-keymap-tool"}) == &t->conflicts[0], "a running conflict is found");
    Check(t->Running({"fcitx5", "HHKB-Keymap-Tool"}) == nullptr, "names are matched as they are");
    Check(t->Running({"fcitx5"}) == nullptr, "no conflict running");
    const auto repo = Translator::Parse(ReadFile(std::string(BADA_ROOT) + "/tide/ikarosu.json"), &why, &denied);
    Check(repo && repo->conflicts.size() == 1 && repo->conflicts[0].processes.empty(),
          "the repository's ikarosu has no Linux program to pause for (PFU's tool has no Linux version)");
    Check(t->Hello() == Bytes({0xAA, 0xAA, 0xF1, 0x01, 0x02, 1, 0}), "hello");
    Host host;
    host.mode = Mode::kHenkan;
    host.profile = 1;
    host.caps = true;
    Check(t->State(host, {"Convert", "NonConvert"}) == Bytes({0xAA, 0xAA, 0xF1, 0x02, 0x04, 1, 1, 1, 6}), "state");
    host.profile = -1;
    Check(t->State(host, {}) == Bytes({0xAA, 0xAA, 0xF1, 0x02, 0x04, 1, 255, 1, 0}), "state with no profile");

    // §7: a send template that begins like a firmware update or a settings write is refused.
    std::string bad = kIkarosu;
    bad.replace(bad.find("AA AA F1 01 02"), 14, "AA AA E0 01 02");
    Check(!Translator::Parse(bad, &why, &denied).has_value() && denied && why == "hello", "an E0 hello is refused");
    bad = kIkarosu;
    bad.replace(bad.find("AA AA F1 02 04"), 14, "AA AA 86 02 04");
    Check(!Translator::Parse(bad, &why, &denied).has_value() && denied && why == "state", "an 86 state is refused");
    bad = kIkarosu;
    bad.replace(bad.find("\"report_size\": 64"), 17, "\"report_size\": 32");
    bad.replace(bad.find("AA AA F1 01 02"), 14, "05 AA F1 01 02");
    Check(!Translator::Parse(bad, &why, &denied).has_value() && denied, "a VIA keymap write in 32 bytes is refused");
    bad = kIkarosu;
    bad.replace(bad.find("\"major\": 1"), 10, "\"major\": 2");
    Check(!Translator::Parse(bad, &why, &denied).has_value() && !denied, "another major is invalid, not denied");
    Check(!Translator::Parse("[]", &why, &denied).has_value() && why == "not a JSON object", "not an object");
}

void Lease() {
    std::string why;
    bool denied = false;
    Link link(*Translator::Parse(kIkarosu, &why, &denied));
    const Translator& t = link.translator();
    Host host;
    host.take = {"Convert", "NonConvert", "Lang1", "CapsLock"};
    std::uint64_t now = 1000;

    // hello at once, then once a second until answered.
    auto r = link.Tick(now, nullptr, host);
    Check(r && *r == t.Hello(), "hello after connect");
    Check(!link.Tick(now + 250, nullptr, host), "no second hello within a second");
    Check(link.Tick(now + 1000, nullptr, host).has_value(), "hello again after a second");

    // The reply: features keys and take, a 3 s lease.
    now += 1100;
    Check(!link.Received(now, Bytes({0x55, 0x55, 0xF1, 0, 1, 6, 1, 0, 0x11, 0xB8, 0x0B})), "the hello reply");
    Check(link.phase() == Link::Phase::kHeld, "the lease is held");
    r = link.Tick(now, nullptr, host);
    // CapsLock isn't the keyboard's to take here? It is: the translator lists it. The text service
    // never asks for it; this host does, and gets it.
    Check(r && *r == Bytes({0xAA, 0xAA, 0xF1, 0x02, 0x04, 0, 255, 0, 2 | 4 | 32 | 64}), "the state at once");
    Check(!link.Tick(now + 250, nullptr, host), "no renewal before a third of the lease");
    link.Received(now + 300, Bytes({0x55, 0x55, 0xF1, 0, 2, 6}));
    Check(link.Tick(now + 1000, nullptr, host).has_value(), "a renewal after a third of the lease");
    host.mode = Mode::kHenkan;
    host.profile = 0;
    r = link.Tick(now + 1010, nullptr, host);
    Check(r && (*r)[5] == 1 && (*r)[6] == 0, "a change goes at once");
    link.Received(now + 1020, Bytes({0x55, 0x55, 0xF1, 0, 2, 6}));

    // Keys: a taken key runs, with its modifiers (HID byte: left shift); one not taken doesn't.
    auto k = link.Received(now + 1100, Bytes({0x55, 0x55, 0xF1, 0, 0x10, 9, 138, 1, 0x02, 0, 0, 0, 0, 0, 0}));
    Check(k && k->code == "Convert" && k->down && k->mods.shift && !k->mods.ctrl, "変換 down, with Shift");
    k = link.Received(now + 1150, Bytes({0x55, 0x55, 0xF1, 0, 0x10, 9, 136, 1, 0, 1, 0, 0, 0, 0, 0}));
    Check(!k, "カタカナ isn't taken: the system has it");
    host.take = {};
    link.Tick(now + 1160, nullptr, host);
    k = link.Received(now + 1200, Bytes({0x55, 0x55, 0xF1, 0, 0x10, 9, 138, 0, 0, 2, 0, 0, 0, 0, 0}));
    Check(k && k->code == "Convert" && !k->down, "the release of a press that ran runs, take or not");
    k = link.Received(now + 1210, Bytes({0x55, 0x55, 0xF1, 0, 0x10, 9, 138, 1, 0, 3, 0, 0, 0, 0, 0}));
    Check(!k, "a key no longer taken doesn't run");
    host.take = {"Convert"};

    // §3: two seconds without a reply: the renewals stop, and hello starts again a second later.
    now += 1300;
    Check(!link.Tick(now + 2100, nullptr, host) && link.phase() == Link::Phase::kConnecting, "a quiet keyboard");
    Check(link.take().empty(), "takes nothing");
    Check(!link.Tick(now + 2600, nullptr, host), "no hello right away");
    Check(link.Tick(now + 3200, nullptr, host) == t.Hello(), "hello again");

    // §5: a conflicting program: nothing is sent, and the link says so.
    link.Received(now + 3300, Bytes({0x55, 0x55, 0xF1, 0, 1, 6, 1, 0, 0x11, 0xB8, 0x0B}));
    const Translator::Conflict* c = &t.conflicts[0];
    Check(!link.Tick(now + 3400, c, host) && link.phase() == Link::Phase::kConflict, "a conflict pauses the link");
    Check(json::Write(link.Published()).find("\"conflict\":\"pfu-hhkb-keymap-tool\"") != std::string::npos,
          "the conflict is published");
    Check(!link.Tick(now + 5000, c, host), "nothing while it runs");
    Check(link.Tick(now + 5250, nullptr, host) == t.Hello(), "hello once it quit");

    // blocked: wait retry_ms.
    link.Received(now + 5300, Bytes({0x55, 0x55, 0xF1, 4, 1, 6, 1, 0, 0x11, 0xB8, 0x0B}));
    Check(link.phase() == Link::Phase::kBlocked, "blocked");
    Check(!link.Tick(now + 9000, nullptr, host), "no hello while blocked");
    Check(link.Tick(now + 15400, nullptr, host) == t.Hello(), "hello after retry_ms");

    // no-lease: hello again at once.
    link.Received(now + 15500, Bytes({0x55, 0x55, 0xF1, 0, 1, 6, 1, 0, 0x11, 0xB8, 0x0B}));
    link.Tick(now + 15500, nullptr, host);
    link.Received(now + 15600, Bytes({0x55, 0x55, 0xF1, 2, 2, 6}));
    Check(link.phase() == Link::Phase::kConnecting, "no-lease: connecting");
    Check(link.Tick(now + 15600, nullptr, host) == t.Hello(), "hello at once after no-lease");

    // A keyboard that never answers is left alone for a minute after five hellos.
    Link silent(*Translator::Parse(kIkarosu, &why, &denied));
    std::uint64_t s = 0;
    int hellos = 0;
    for (int i = 0; i < 40; ++i, s += 250) {
        if (silent.Tick(s, nullptr, host)) ++hellos;
    }
    Check(hellos == 5 && silent.phase() == Link::Phase::kSilent, "five hellos, then silent");
}

// The HHKB Professional HYBRID's four interfaces, as /sys/class/hidraw/hidraw*/device/report_descriptor
// has them (ikarosu): the keyboard, consumer and system keys with report ids, the PFU vendor channel
// (tide), and the VIA channel.
const std::vector<std::uint8_t> kKeyboard = {0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x03,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,0xc0};
const std::vector<std::uint8_t> kConsumer = {0x05,0x01,0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x01,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x09,0x6f,0x09,0x70,0x09,0x30,0x09,0xb7,0x09,0xe2,0x09,0xe9,0x09,0xea,0x09,0xb8,0x81,0x02,0xc0,0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x02,0x05,0x07,0x95,0x01,0x75,0x08,0x81,0x03,0x95,0xe8,0x75,0x01,0x15,0x00,0x25,0x01,0x05,0x07,0x19,0x00,0x29,0xe7,0x81,0x02,0xc0};
const std::vector<std::uint8_t> kVendor = {0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x09,0x02,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x40,0x81,0x02,0x09,0x03,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x40,0x91,0x02,0xc0};
const std::vector<std::uint8_t> kVia = {0x06,0x60,0xff,0x09,0x61,0xa1,0x01,0x09,0x62,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x20,0x81,0x02,0x09,0x63,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x20,0x91,0x02,0xc0};

void Descriptors() {
    auto v = ParseDescriptor(kVendor);
    Check(v && v->usage_page == 0xFF00 && v->usage == 1 && v->in_size == 64 && v->out_size == 64 && !v->numbered,
          "the vendor channel: 0xFF00/1, 64 bytes each way, no report id");
    auto k = ParseDescriptor(kKeyboard);
    Check(k && k->usage_page == 1 && k->usage == 6 && k->in_size == 8 && k->out_size == 1, "the keyboard");
    auto c = ParseDescriptor(kConsumer);
    Check(c && c->usage_page == 0x0C && c->usage == 1 && c->numbered, "the consumer keys: numbered");
    auto via = ParseDescriptor(kVia);
    Check(via && via->usage_page == 0xFF60 && via->usage == 0x61 && via->in_size == 32, "VIA");
    Check(!ParseDescriptor({0x06, 0x00}).has_value(), "a cut descriptor");
    Check(!ParseDescriptor({}).has_value(), "an empty one");

    std::string why;
    bool denied = false;
    const Translator t = *Translator::Parse(kIkarosu, &why, &denied);
    HidDevice d{0x04FE, 0x22, 1, *v};
    Check(Matches(t.match, d), "ikarosu's vendor channel matches");
    for (const auto* other : {&kKeyboard, &kConsumer, &kVia}) {
        HidDevice o = d;
        o.info = *ParseDescriptor(*other);
        Check(!Matches(t.match, o), "its other interfaces don't");
    }
    HidDevice wrong = d;
    wrong.product_id = 0x21;
    Check(!Matches(t.match, wrong), "another product doesn't");
    Translator limited = t;
    limited.match.version_min = 2;
    Check(!Matches(limited.match, d), "below version_min");
    d.version.reset();
    Check(!Matches(limited.match, d), "version limits and no release number: no match");
    Check(Matches(t.match, d), "no limits: the release number isn't needed (Bluetooth)");
}

void KeyCodes() {
    for (const std::string& code : tide::Keys()) {
        unsigned evdev = 0, sym = 0;
        Check(EvdevOf(code, &evdev, &sym), code + " has an evdev code");
        CheckEq(CodeOf(evdev, sym), code, code + " reads back as itself");
    }
    unsigned evdev = 0, sym = 0;
    Check(!EvdevOf("KeyA", &evdev, &sym), "KeyA is no tide key");
}

// --- the hub, on a keyboard of the test's own

namespace fs = std::filesystem;

struct Fake {
    fs::path root;
    int master = -1;

    Fake() {
        root = fs::temp_directory_path() / ("bada-tide-test-" + std::to_string(getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "sys/hidraw7/device");
        fs::create_directories(root / "sys/hidraw6/device");
        fs::create_directories(root / "dev");
        fs::create_directories(root / "config/tide");
        fs::create_directories(root / "proc/4242");
        Write(root / "sys/hidraw7/device/uevent", "DRIVER=hid-generic\nHID_ID=0003:000004FE:00000022\nHID_NAME=PFU\n");
        Write(root / "sys/hidraw7/device/report_descriptor", std::string(kVendor.begin(), kVendor.end()));
        Write(root / "sys/hidraw6/device/uevent", "HID_ID=0003:000004FE:00000022\n");
        Write(root / "sys/hidraw6/device/report_descriptor", std::string(kKeyboard.begin(), kKeyboard.end()));
        Write(root / "proc/4242/comm", "fcitx5\n");
        // The keyboard: a pseudo-terminal in raw mode, its other end where /dev/hidraw7 would be.
        master = posix_openpt(O_RDWR | O_NOCTTY);
        grantpt(master);
        unlockpt(master);
        const std::string slave = ptsname(master);
        termios raw;
        const int s = open(slave.c_str(), O_RDWR | O_NOCTTY);
        tcgetattr(s, &raw);
        cfmakeraw(&raw);
        tcsetattr(s, TCSANOW, &raw);
        close(s);
        fs::create_symlink(slave, root / "dev/hidraw7");
    }
    ~Fake() {
        if (master >= 0) close(master);
        fs::remove_all(root);
    }
    static void Write(const fs::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    // A report the hub sent: 65 bytes, the report id first.
    std::optional<std::vector<std::uint8_t>> Sent(int ms = 1000) {
        std::vector<std::uint8_t> out;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (out.size() < 65 && std::chrono::steady_clock::now() < end) {
            pollfd p{master, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            std::uint8_t b[65];
            const ssize_t n = read(master, b, 65 - out.size());
            if (n > 0) out.insert(out.end(), b, b + n);
        }
        if (out.size() != 65) return std::nullopt;
        return out;
    }
    // One report from the keyboard. A pseudo-terminal keeps no report apart from the next, as
    // hidraw does: the hub reads each before the next goes (Settle).
    void Answer(const std::vector<std::uint8_t>& report) {
        if (write(master, report.data(), report.size()) != static_cast<ssize_t>(report.size())) {
            Check(false, "the keyboard answered");
        }
    }
};

// Runs the hub's loop until `done` or `ms` passed.
template <typename F>
void Run(TideHub& hub, int ms, F done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < end) {
        pollfd p{hub.fd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0) hub.Dispatch();
    }
}

// The keyboard answers, and the hub reads it.
void Answer(Fake& fake, TideHub& hub, const std::vector<std::uint8_t>& report) {
    fake.Answer(report);
    Run(hub, 100, [] { return false; });
}

void Hub() {
    Fake fake;
    std::string translator = kIkarosu;
    Fake::Write(fake.root / "config/tide/ikarosu.json", translator);
    Fake::Write(fake.root / "config/tide/broken.json", "{\"format\": \"nope\"}");
    std::vector<Link::Key> keys;
    int changes = 0;
    TideHub::Options options;
    options.settings_dir = fake.root / "config";
    options.sys_dir = fake.root / "sys";
    options.dev_dir = fake.root / "dev";
    options.proc_dir = fake.root / "proc";
    options.key = [&](const Link::Key& k) { keys.push_back(k); };
    options.changed = [&] { ++changes; };
    TideHub hub(std::move(options));
    Check(hub.fd() >= 0, "the hub has a descriptor to wait on");
    const std::string status = hub.Status();
    Check(status.find("\"translator\":\"ikarosu\"") != std::string::npos, "the vendor channel is linked: " + status);
    Check(status.find("tide/broken.json") != std::string::npos, "a broken translator is a problem");
    Check(status.find("hidraw6") == std::string::npos, "the keyboard interface isn't opened");

    // hello, with the report id 0 first.
    Host host;
    host.take = {"Convert", "Lang1"};
    hub.SetHost(host);
    Run(hub, 300, [] { return false; });
    auto sent = fake.Sent();
    Check(sent && (*sent)[0] == 0 && std::vector<std::uint8_t>(sent->begin() + 1, sent->end()) ==
                                         Bytes({0xAA, 0xAA, 0xF1, 0x01, 0x02, 1, 0}),
          "hello went out");
    fake.Answer(Bytes({0x55, 0x55, 0xF1, 0, 1, 6, 1, 0, 0x11, 0xB8, 0x0B}));
    Run(hub, 500, [&] { return hub.Status().find("\"state\":\"held\"") != std::string::npos; });
    Check(hub.Status().find("\"state\":\"held\"") != std::string::npos, "the lease is held: " + hub.Status());
    sent = fake.Sent();
    Check(sent && (*sent)[1] == 0xAA && (*sent)[4] == 0x02 && (*sent)[9] == (4 | 32), "the state, taking 変換 and Lang1");
    Answer(fake, hub, Bytes({0x55, 0x55, 0xF1, 0, 2, 6}));

    // A change goes at once.
    host.mode = Mode::kHenkan;
    hub.SetHost(host);
    sent = fake.Sent(300);
    Check(sent && (*sent)[6] == 1, "henkan, at once");
    Answer(fake, hub, Bytes({0x55, 0x55, 0xF1, 0, 2, 6}));

    // A taken key comes to the frontend.
    fake.Answer(Bytes({0x55, 0x55, 0xF1, 0, 0x10, 9, 138, 1, 0x01, 0, 0, 0, 0, 0, 0}));
    Run(hub, 500, [&] { return !keys.empty(); });
    Check(keys.size() == 1 && keys[0].code == "Convert" && keys[0].down && keys[0].mods.ctrl, "変換 down, with Control");

    // A conflicting program pauses the link.
    std::string conflicted = translator;
    conflicted.replace(conflicted.find("[\"hhkb-keymap-tool\"]"), 20, "[\"keymap-thing\"]");
    Fake::Write(fake.root / "config/tide/ikarosu.json", conflicted);
    Fake::Write(fake.root / "proc/4242/comm", "keymap-thing\n");
    hub.Reload();
    Run(hub, 600, [&] { return hub.Status().find("\"state\":\"conflict\"") != std::string::npos; });
    Check(hub.Status().find("\"conflict\":\"pfu-hhkb-keymap-tool\"") != std::string::npos,
          "paused while the program runs: " + hub.Status());

    // Unplugged: the link goes.
    fs::remove_all(fake.root / "sys/hidraw7");
    Run(hub, 2600, [&] { return hub.Status().find("ikarosu") == std::string::npos; });
    Check(hub.Status().find("\"links\":[]") != std::string::npos, "unplugged: no link");
    Check(changes > 0, "the frontend heard of the changes");
}

}  // namespace

int main() {
    Templates();
    Translators();
    Lease();
    Descriptors();
    KeyCodes();
    Hub();
    return test::Finish("bada_tide_tests");
}
