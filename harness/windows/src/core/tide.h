// bada-tide (docs/bada-tide-v1.md) without a device: a translator file, its report templates, the
// deny list, and one keyboard's lease as a state machine. The helper (src/tide/) moves reports in
// and out of the keyboard and runs the clock; everything it decides is here. No Windows headers,
// so the tests run anywhere. A port of the macOS harness's TideTranslator.swift and the lease logic
// of its TideHelper.swift.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "keys.h"
#include "keytable.h"

namespace bada::tide {

// The tide version this harness speaks.
inline constexpr int kMajor = 1, kMinor = 0;
inline constexpr char kFormat[] = "bada-tide-translator/1";

// The seven keys tide knows (the standard, §4), in this order everywhere a set of them is packed
// into bits (the text service's messages to the helper).
const std::vector<std::string>& Keys();
std::uint32_t KeyBits(const std::set<std::string>& keys);
std::set<std::string> KeysOf(std::uint32_t bits);

// One template: a report's bytes, space-separated: a hex byte or a field (`{mode}`, `{lease_ms:le16}`).
class Template {
public:
    struct Token {
        bool field = false;
        std::uint8_t byte = 0;
        std::string name;
        int width = 1;
        bool big_endian = false;
    };
    static std::optional<Template> Parse(std::string_view text);

    // The template's bytes before any field, as upper-case hex ("AA AA F1"): for the deny list.
    std::string LiteralPrefix() const;
    // A report of `size` bytes with the fields filled in; nullopt if a field is missing or the
    // report is too long.
    std::optional<std::vector<std::uint8_t>> Encode(const std::map<std::string, int>& fields, size_t size) const;
    // The fields of a report that matches the template (every literal byte equal); nullopt if it
    // doesn't.
    std::optional<std::map<std::string, int>> Decode(const std::vector<std::uint8_t>& report) const;

private:
    std::vector<Token> tokens_;
};

// The harness's state, as `state` carries it (bada-tide §2).
struct Host {
    Mode mode = Mode::kMuhenkan;
    int profile = -1;  // the index in the key table's cycle; -1 none
    bool caps = false;
    std::set<std::string> take;  // the keys the harness asks to take (§4)
    bool operator==(const Host& o) const {
        return mode == o.mode && profile == o.profile && caps == o.caps && take == o.take;
    }
};

// What a report from the keyboard is.
struct Incoming {
    enum class Kind { kOther, kHello, kState, kKey } kind = Kind::kOther;
    std::string status;               // kHello, kState
    std::set<std::string> features;   // kHello
    int lease_ms = 3000;              // kHello
    std::string code;                 // kKey
    bool down = false;                // kKey
    Mods mods;                        // kKey
    int seq = 0;                      // kKey
    bool dropped = false;             // kKey
};

// A `bada-tide-translator/1` file (bada-tide §6), as the harness uses it.
struct Translator {
    struct Match {
        int vendor_id = 0, product_id = 0, usage_page = 0, usage = 0, report_size = 0;
        std::optional<int> version_min, version_max;
        bool Accepts(int version) const;
    };
    struct Conflict {
        std::string id, name, reason;
        std::vector<std::string> processes;  // windows.process, in lower case
    };

    // Reads a translator. nullopt, and `problem` says why; `denied` is set when it was refused by
    // bada-tide §7 rather than being malformed.
    static std::optional<Translator> Parse(std::string_view text, std::string* problem, bool* denied);

    std::string id, name;
    Match match;
    Template hello_send, hello_reply, state_send, state_reply, key_recv;
    std::map<std::string, int> status, mode, features, usage, take, edge;
    int profile_none = 255;
    int dropped = 0;
    int retry_ms = 10000;
    std::vector<Conflict> conflicts;

    std::vector<std::uint8_t> Hello() const;
    // `take`: the keys taken (§4), already narrowed to what this keyboard can take.
    std::vector<std::uint8_t> State(const Host& host, const std::set<std::string>& take) const;
    Incoming Read(const std::vector<std::uint8_t>& report) const;
    // The conflict whose program is running (`running`: file names in lower case), or nullptr.
    const Conflict* Running(const std::set<std::string>& running) const;

private:
    std::string StatusName(std::optional<int> value) const;
};

// The send templates bada-tide never lets through (§7): the message whose template begins like a
// known dangerous request ("hello" or "state"), or "".
std::string Refused(const Template& hello, const Template& state, int report_size);

// One matched keyboard's link: the lease, with no device (bada-tide §3). The helper calls Tick four
// times a second and after every report, sends what it returns, and hands Received every report.
class Link {
public:
    enum class Phase { kConnecting, kHeld, kConflict, kBlocked, kUnsupported, kSilent, kError };
    static const char* PhaseName(Phase phase);

    explicit Link(Translator translator) : translator_(std::move(translator)) {}

    // `now`: a steady clock, in ms. `conflict`: the program of the translator's conflicts that runs
    // now, or nullptr. Returns the report to send now, if any.
    std::optional<std::vector<std::uint8_t>> Tick(std::uint64_t now, const Translator::Conflict* conflict,
                                                  const Host& host);
    // A send didn't go out: the next tick tries again.
    void SendFailed() { last_sent_at_.reset(); }

    // A key event to run: a taken key's press, or the release of one whose press ran.
    struct Key {
        std::string code;
        bool down = false;
        Mods mods;
    };
    // A report from the keyboard. Returns the key event it carries, if bada runs it.
    std::optional<Key> Received(std::uint64_t now, const std::vector<std::uint8_t>& report);

    const Translator& translator() const { return translator_; }
    Phase phase() const { return phase_; }
    const std::set<std::string>& take() const { return take_; }
    const std::set<std::string>& features() const { return features_; }
    // What `harness.tide` publishes for this keyboard (bada-tide §8).
    json::Value Published() const;
    // A line for the log when something worth one happened since the last call; "" otherwise.
    std::string TakeNote();

private:
    std::set<std::string> WantedTake(const Host& host) const;
    void Note(std::string line);

    Translator translator_;
    Phase phase_ = Phase::kConnecting;
    std::set<std::string> features_;
    int lease_ms_ = 3000;
    std::string conflict_;
    std::uint64_t retry_at_ = 0;
    std::optional<std::vector<std::uint8_t>> last_sent_;
    std::optional<std::uint64_t> last_sent_at_;
    std::set<std::string> take_;
    // Taken keys whose press ran: their release runs too, whatever take is by then.
    std::set<std::string> down_;
    std::uint64_t last_heard_ = 0;
    int unanswered_ = 0;  // hellos sent with no answer, since the last answer
    std::string note_;
};

}  // namespace bada::tide
