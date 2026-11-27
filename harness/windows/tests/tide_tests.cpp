// bada-tide's core (src/core/tide.h): the translators in the repository, templates, the deny list
// (§7), and the lease (§3, §4, §5) on a clock of the test's own, with reports made by hand.
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "tide.h"
#include "tide_ipc.h"

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

// The ikarosu translator as its repository publishes it, with the Windows conflict.
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
                "match": {"macos": {"bundle_id": ["jp.co.pfu.hhkb-keymap-tool"]}, "windows": {"process": ["HHKBKeymapTool.exe"]}}}]})";

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
    for (const char* file : {"/tide/dmk-hhkb-hybrid.json"}) {
        const auto t = Translator::Parse(ReadFile(std::string(BADA_ROOT) + file), &why, &denied);
        Check(t.has_value(), std::string("the repository's ") + file + " reads: " + why);
    }
    const auto t = Translator::Parse(kIkarosu, &why, &denied);
    Check(t.has_value(), "ikarosu's translator reads: " + why);
    Check(t->match.vendor_id == 0x04FE && t->match.product_id == 0x22 && t->match.usage_page == 0xFF00 &&
              t->match.usage == 1 && t->match.report_size == 64,
          "its match");
    Check(t->conflicts.size() == 1 && t->conflicts[0].processes == std::vector<std::string>{"hhkbkeymaptool.exe"},
          "its Windows conflict, in lower case");
    Check(t->Running({"explorer.exe", "hhkbkeymaptool.exe"}) == &t->conflicts[0], "a running conflict is found");
    Check(t->Running({"explorer.exe"}) == nullptr, "no conflict running");
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
    Check(!k, "カタカナ isn't taken: Windows has it");
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

void Ipc() {
    Host host;
    host.mode = Mode::kHenkan;
    host.profile = 3;
    host.caps = true;
    host.take = {"Convert", "Lang1"};
    Check(ipc::UnpackHost(ipc::PackHost(host)) == host, "the host word");
    host.profile = -1;
    host.take = {};
    Check(ipc::UnpackHost(ipc::PackHost(host)) == host, "the host word, no profile");
    std::uintptr_t w = 0;
    std::intptr_t l = 0;
    ipc::KeyEvent key{"Lang2", true, {}};
    key.mods.ctrl = true;
    ipc::PackKey(key, &w, &l);
    const auto back = ipc::UnpackKey(w, l);
    Check(back && back->code == "Lang2" && back->down && back->mods.ctrl && !back->mods.shift, "the key words");
    Check(!ipc::UnpackKey(0x0FF, 0).has_value(), "an unknown key index");
    std::vector<char> block(ipc::kStatusSize, 0);
    Check(!ipc::ReadStatus(block.data()).has_value(), "an empty status block");
    Check(ipc::WriteStatus(block.data(), R"({"links":[]})"), "a status written");
    Check(ipc::ReadStatus(block.data()) == std::optional<std::string>(R"({"links":[]})"), "and read");
}

}  // namespace

int main() {
    Templates();
    Translators();
    Lease();
    Ipc();
    return test::Finish("bada_tide_tests");
}
