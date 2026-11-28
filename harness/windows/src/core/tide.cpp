#include "tide.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace bada::tide {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A whole number in a JSON value, or nullopt.
std::optional<int> Int(const json::Value* v) {
    if (!v || !v->IsNumber() || std::floor(v->number) != v->number || std::fabs(v->number) > 2147483647.0) {
        return std::nullopt;
    }
    return static_cast<int>(v->number);
}

struct Bad {
    std::string why;
};

const json::Value& Obj(const json::Value& parent, const std::string& key, const std::string& what) {
    const json::Value* v = parent.Get(key);
    if (!v || !v->IsObject()) throw Bad{what + " is missing"};
    return *v;
}

int IntOf(const json::Value& parent, const std::string& key, const std::string& what) {
    const std::optional<int> n = Int(parent.Get(key));
    if (!n) throw Bad{what + "." + key + " is missing"};
    return *n;
}

std::map<std::string, int> Ints(const json::Value* v, const std::string& what) {
    if (!v || !v->IsObject()) throw Bad{what + " is missing"};
    std::map<std::string, int> out;
    for (const auto& [k, item] : v->object) {
        const std::optional<int> n = Int(&item);
        if (!n) throw Bad{what + "." + k + " isn't a number"};
        out[k] = *n;
    }
    return out;
}

Template TemplateOf(const json::Value& parent, const std::string& key, const std::string& what) {
    const json::Value* v = parent.Get(key);
    std::optional<Template> t = v && v->IsString() ? Template::Parse(v->string) : std::nullopt;
    if (!t) throw Bad{what + "." + key + " isn't a template"};
    return *t;
}

bool Known(const std::string& key) {
    const std::vector<std::string>& keys = Keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

}  // namespace

const std::vector<std::string>& Keys() {
    static const std::vector<std::string> keys = {"Backquote", "NonConvert", "Convert", "KanaMode",
                                                  "Lang2",     "Lang1",      "CapsLock"};
    return keys;
}

std::uint32_t KeyBits(const std::set<std::string>& keys) {
    std::uint32_t bits = 0;
    const std::vector<std::string>& all = Keys();
    for (size_t i = 0; i < all.size(); ++i) {
        if (keys.count(all[i])) bits |= 1u << i;
    }
    return bits;
}

std::set<std::string> KeysOf(std::uint32_t bits) {
    std::set<std::string> out;
    const std::vector<std::string>& all = Keys();
    for (size_t i = 0; i < all.size(); ++i) {
        if (bits & (1u << i)) out.insert(all[i]);
    }
    return out;
}

// --- templates

std::optional<Template> Template::Parse(std::string_view text) {
    Template t;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size()) break;
        size_t end = text.find(' ', i);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view part = text.substr(i, end - i);
        i = end;
        Token token;
        if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
            const std::string_view inner = part.substr(1, part.size() - 2);
            const size_t colon = inner.find(':');
            const std::string_view name = inner.substr(0, colon);
            if (name.empty() || name.find(':') != std::string_view::npos) return std::nullopt;
            token.field = true;
            token.name = std::string(name);
            if (colon != std::string_view::npos) {
                const std::string_view w = inner.substr(colon + 1);
                if (w == "le16") token.width = 2;
                else if (w == "le32") token.width = 4;
                else if (w == "be16") token.width = 2, token.big_endian = true;
                else if (w == "be32") token.width = 4, token.big_endian = true;
                else return std::nullopt;
            }
        } else {
            if (part.size() != 2 || !std::isxdigit(static_cast<unsigned char>(part[0])) ||
                !std::isxdigit(static_cast<unsigned char>(part[1]))) {
                return std::nullopt;
            }
            token.byte = static_cast<std::uint8_t>(std::stoi(std::string(part), nullptr, 16));
        }
        t.tokens_.push_back(std::move(token));
    }
    if (t.tokens_.empty()) return std::nullopt;
    return t;
}

std::string Template::LiteralPrefix() const {
    std::string out;
    for (const Token& t : tokens_) {
        if (t.field) break;
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X", t.byte);
        if (!out.empty()) out += ' ';
        out += hex;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Template::Encode(const std::map<std::string, int>& fields, size_t size) const {
    std::vector<std::uint8_t> out;
    for (const Token& t : tokens_) {
        if (!t.field) {
            out.push_back(t.byte);
            continue;
        }
        const auto it = fields.find(t.name);
        if (it == fields.end()) return std::nullopt;
        std::vector<std::uint8_t> bytes;
        for (int b = 0; b < t.width; ++b) bytes.push_back(static_cast<std::uint8_t>((it->second >> (8 * b)) & 0xFF));
        if (t.big_endian) std::reverse(bytes.begin(), bytes.end());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    if (out.size() > size) return std::nullopt;
    out.resize(size, 0);
    return out;
}

std::optional<std::map<std::string, int>> Template::Decode(const std::vector<std::uint8_t>& report) const {
    std::map<std::string, int> fields;
    size_t i = 0;
    for (const Token& t : tokens_) {
        if (!t.field) {
            if (i >= report.size() || report[i] != t.byte) return std::nullopt;
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(t.width) > report.size()) return std::nullopt;
        std::vector<std::uint8_t> bytes(report.begin() + static_cast<std::ptrdiff_t>(i),
                                        report.begin() + static_cast<std::ptrdiff_t>(i) + t.width);
        if (t.big_endian) std::reverse(bytes.begin(), bytes.end());
        std::uint32_t v = 0;
        for (size_t b = 0; b < bytes.size(); ++b) v |= static_cast<std::uint32_t>(bytes[b]) << (8 * b);
        fields[t.name] = static_cast<int>(v);
        i += static_cast<size_t>(t.width);
    }
    return fields;
}

// --- §7

std::string Refused(const Template& hello, const Template& state, int report_size) {
    static const std::vector<std::string> pfu = {"AA AA E0", "AA AA E1", "AA AA E2", "AA AA E3", "AA AA E4",
                                                 "AA AA E5", "AA AA E6", "AA AA E7", "AA AA 86", "AA AA 04",
                                                 "AA AA 07", "AA AA 08", "AA AA D0"};
    static const std::vector<std::string> via = {"05", "07", "0B", "0C", "12", "13"};
    for (const auto& [name, t] : {std::pair<const char*, const Template*>{"hello", &hello}, {"state", &state}}) {
        const std::string prefix = t->LiteralPrefix();
        for (const std::string& p : pfu) {
            if (prefix.rfind(p, 0) == 0) return name;
        }
        if (report_size == 32) {
            const std::string first = prefix.substr(0, prefix.find(' '));
            if (std::find(via.begin(), via.end(), first) != via.end()) return name;
        }
    }
    return {};
}

// --- translators

bool Translator::Match::Accepts(int version) const {
    if (version_min && version < *version_min) return false;
    if (version_max && version > *version_max) return false;
    return true;
}

std::optional<Translator> Translator::Parse(std::string_view text, std::string* problem, bool* denied) {
    if (denied) *denied = false;
    const std::optional<json::Value> parsed = json::Parse(text);
    try {
        if (!parsed || !parsed->IsObject()) throw Bad{"not a JSON object"};
        const json::Value& o = *parsed;
        if (o.GetString("format") != kFormat) throw Bad{std::string("format isn't ") + kFormat};
        Translator t;
        t.id = o.GetString("id");
        if (t.id.empty()) throw Bad{"id is missing"};
        t.name = o.GetString("name", t.id);
        const json::Value& m = Obj(o, "match", "match");
        t.match.vendor_id = IntOf(m, "vendor_id", "match");
        t.match.product_id = IntOf(m, "product_id", "match");
        t.match.usage_page = IntOf(m, "usage_page", "match");
        t.match.usage = IntOf(m, "usage", "match");
        t.match.report_size = IntOf(m, "report_size", "match");
        if (t.match.report_size <= 0 || t.match.report_size > 1024) throw Bad{"match.report_size is out of range"};
        t.match.version_min = Int(m.Get("version_min"));
        t.match.version_max = Int(m.Get("version_max"));
        const json::Value& version = Obj(o, "version", "version");
        if (Int(version.Get("major")) != kMajor) throw Bad{"version.major isn't " + std::to_string(kMajor)};
        const json::Value& hello = Obj(o, "hello", "hello");
        const json::Value& state = Obj(o, "state", "state");
        const json::Value& key = Obj(o, "key", "key");
        t.hello_send = TemplateOf(hello, "send", "hello");
        t.hello_reply = TemplateOf(hello, "reply", "hello");
        t.state_send = TemplateOf(state, "send", "state");
        t.state_reply = TemplateOf(state, "reply", "state");
        t.key_recv = TemplateOf(key, "recv", "key");
        const json::Value& values = Obj(o, "values", "values");
        t.status = Ints(values.Get("status"), "values.status");
        for (const char* s : {"ok", "unsupported", "no-lease", "bad-value", "blocked"}) {
            if (!t.status.count(s)) throw Bad{std::string("values.status.") + s + " is missing"};
        }
        t.mode = Ints(values.Get("mode"), "values.mode");
        if (!t.mode.count("muhenkan") || !t.mode.count("henkan")) throw Bad{"values.mode needs muhenkan and henkan"};
        t.profile_none = IntOf(values, "profile_none", "values");
        t.features = Ints(values.Get("features"), "values.features");
        t.usage = Ints(values.Get("usage"), "values.usage");
        t.take = Ints(values.Get("take"), "values.take");
        t.edge = Ints(values.Get("edge"), "values.edge");
        if (!t.edge.count("up") || !t.edge.count("down")) throw Bad{"values.edge needs up and down"};
        if (values.GetString("mods") != "hid") throw Bad{"values.mods isn't \"hid\""};
        for (const auto& [k, v] : t.usage) {
            if (!Known(k)) throw Bad{"values.usage." + k + " is no language key"};
        }
        for (const auto& [k, v] : t.take) {
            if (!Known(k)) throw Bad{"values.take." + k + " is no language key"};
        }
        if (const json::Value* flags = values.Get("flags")) t.dropped = Int(flags->Get("dropped")).value_or(0);
        t.retry_ms = Int(o.Get("retry_ms")).value_or(10000);
        if (const json::Value* list = o.Get("conflicts"); list && list->IsArray()) {
            for (const json::Value& c : list->array) {
                Conflict conflict;
                conflict.id = c.GetString("id");
                if (conflict.id.empty()) continue;
                conflict.name = c.GetString("name", conflict.id);
                conflict.reason = c.GetString("reason");
                const json::Value* match = c.Get("match");
                const json::Value* windows = match ? match->Get("windows") : nullptr;
                const json::Value* process = windows ? windows->Get("process") : nullptr;
                if (process && process->IsArray()) {
                    for (const json::Value& p : process->array) {
                        if (p.IsString() && !p.string.empty()) conflict.processes.push_back(Lower(p.string));
                    }
                }
                t.conflicts.push_back(std::move(conflict));
            }
        }
        const std::string refused = Refused(t.hello_send, t.state_send, t.match.report_size);
        if (!refused.empty()) {
            if (denied) *denied = true;
            throw Bad{refused};
        }
        return t;
    } catch (const Bad& bad) {
        if (problem) *problem = bad.why;
        return std::nullopt;
    }
}

std::vector<std::uint8_t> Translator::Hello() const {
    return hello_send.Encode({{"major", kMajor}, {"minor", kMinor}}, static_cast<size_t>(match.report_size))
        .value_or(std::vector<std::uint8_t>{});
}

std::vector<std::uint8_t> Translator::State(const Host& host, const std::set<std::string>& keys) const {
    int mask = 0;
    for (const std::string& k : keys) {
        const auto it = take.find(k);
        if (it != take.end()) mask |= it->second;
    }
    const auto m = mode.find(host.mode == Mode::kHenkan ? "henkan" : "muhenkan");
    const int profile = host.profile < 0 ? profile_none : std::min(host.profile, 254);
    return state_send
        .Encode({{"mode", m == mode.end() ? 0 : m->second}, {"profile", profile}, {"caps", host.caps ? 1 : 0},
                 {"take", mask}},
                static_cast<size_t>(match.report_size))
        .value_or(std::vector<std::uint8_t>{});
}

std::string Translator::StatusName(std::optional<int> value) const {
    for (const auto& [word, v] : status) {
        if (value == v) return word;
    }
    return "unknown";
}

Incoming Translator::Read(const std::vector<std::uint8_t>& report) const {
    Incoming in;
    if (const auto f = key_recv.Decode(report)) {
        const auto u = f->find("usage");
        if (u == f->end()) return in;
        for (const auto& [code, value] : usage) {
            if (value != u->second) continue;
            const auto get = [&](const char* name) {
                const auto it = f->find(name);
                return it == f->end() ? 0 : it->second;
            };
            const int hid = get("mods");
            in.kind = Incoming::Kind::kKey;
            in.code = code;
            in.down = get("edge") == edge.at("down");
            in.mods.ctrl = (hid & 0x11) != 0;
            in.mods.shift = (hid & 0x22) != 0;
            in.mods.alt = (hid & 0x44) != 0;
            in.mods.meta = (hid & 0x88) != 0;
            in.seq = get("seq");
            in.dropped = dropped != 0 && (get("flags") & dropped) != 0;
            return in;
        }
        return in;
    }
    if (const auto f = hello_reply.Decode(report)) {
        in.kind = Incoming::Kind::kHello;
        const auto s = f->find("status");
        in.status = StatusName(s == f->end() ? std::nullopt : std::optional<int>(s->second));
        const auto have = f->find("features");
        for (const auto& [feature, bit] : features) {
            if (have != f->end() && (have->second & bit)) in.features.insert(feature);
        }
        const auto lease = f->find("lease_ms");
        in.lease_ms = lease == f->end() ? 3000 : lease->second;
        return in;
    }
    if (const auto f = state_reply.Decode(report)) {
        in.kind = Incoming::Kind::kState;
        const auto s = f->find("status");
        in.status = StatusName(s == f->end() ? std::nullopt : std::optional<int>(s->second));
        return in;
    }
    return in;
}

const Translator::Conflict* Translator::Running(const std::set<std::string>& running) const {
    for (const Conflict& c : conflicts) {
        for (const std::string& p : c.processes) {
            if (running.count(p)) return &c;
        }
    }
    return nullptr;
}

// --- the lease

const char* Link::PhaseName(Phase phase) {
    switch (phase) {
        case Phase::kConnecting: return "connecting";
        case Phase::kHeld: return "held";
        case Phase::kConflict: return "conflict";
        case Phase::kBlocked: return "blocked";
        case Phase::kUnsupported: return "unsupported";
        case Phase::kSilent: return "silent";
        case Phase::kError: return "error";
    }
    return "error";
}

void Link::Note(std::string line) {
    if (!note_.empty()) note_ += "; ";
    note_ += line;
}

std::string Link::TakeNote() {
    std::string out;
    out.swap(note_);
    return out;
}

std::set<std::string> Link::WantedTake(const Host& host) const {
    std::set<std::string> out;
    if (!features_.count("take")) return out;
    for (const std::string& k : host.take) {
        if (translator_.take.count(k)) out.insert(k);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Link::Tick(std::uint64_t now, const Translator::Conflict* conflict,
                                                     const Host& host) {
    const auto sent_since = [&](std::uint64_t ms) { return !last_sent_at_ || now - *last_sent_at_ >= ms; };
    // §5: a conflicting program runs: no hello, no renewal (the lease runs out by itself).
    if (conflict) {
        if (phase_ != Phase::kConflict || conflict_ != conflict->id) {
            phase_ = Phase::kConflict;
            conflict_ = conflict->id;
            take_.clear();
            down_.clear();
            Note(conflict->name + " is running; link paused");
        }
        return std::nullopt;
    }
    if (phase_ == Phase::kConflict) {
        phase_ = Phase::kConnecting;
        conflict_.clear();
        last_sent_at_.reset();
        unanswered_ = 0;
        Note("the conflicting program quit; connecting again");
    }
    switch (phase_) {
        case Phase::kUnsupported:
        case Phase::kError:
        case Phase::kConflict:
            return std::nullopt;
        case Phase::kBlocked:
        case Phase::kSilent:
            if (now < retry_at_) return std::nullopt;
            [[fallthrough]];
        case Phase::kConnecting: {
            // hello, again every second until it is answered; a keyboard that never answers (a
            // stock firmware with the same ids) is left alone for a minute after five tries.
            if (unanswered_ >= 5) {
                if (phase_ != Phase::kSilent) Note("no answer to hello; trying again in a minute");
                phase_ = Phase::kSilent;
                unanswered_ = 0;
                retry_at_ = now + 60000;
                return std::nullopt;
            }
            if (!sent_since(1000)) return std::nullopt;
            std::vector<std::uint8_t> r = translator_.Hello();
            if (r.empty()) return std::nullopt;
            ++unanswered_;
            last_sent_ = r;
            last_sent_at_ = now;
            return r;
        }
        case Phase::kHeld: {
            // §3: a link that hears nothing from the keyboard takes nothing. Its replies come every
            // renewal; two seconds of silence and the renewals stop, so the lease runs out and the
            // keyboard gives the keys back to Windows.
            if (now - last_heard_ > 2000) {
                Note("the keyboard went quiet; letting the lease run out");
                phase_ = Phase::kConnecting;
                take_.clear();
                down_.clear();
                last_sent_at_ = now;  // the next hello after a second
                unanswered_ = 0;
                return std::nullopt;
            }
            take_ = WantedTake(host);
            std::vector<std::uint8_t> r = translator_.State(host, take_);
            if (r.empty()) return std::nullopt;
            // Every third of the lease, and at once when the state changed (§3).
            if (last_sent_ != r || sent_since(static_cast<std::uint64_t>(lease_ms_ / 3))) {
                last_sent_ = r;
                last_sent_at_ = now;
                return r;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<Link::Key> Link::Received(std::uint64_t now, const std::vector<std::uint8_t>& report) {
    const Incoming in = translator_.Read(report);
    if (in.kind != Incoming::Kind::kOther) {
        last_heard_ = now;
        unanswered_ = 0;
    }
    switch (in.kind) {
        case Incoming::Kind::kHello:
            if (phase_ == Phase::kConflict) return std::nullopt;
            if (in.status == "ok") {
                if (phase_ != Phase::kHeld) {
                    std::string list;
                    for (const std::string& f : in.features) list += (list.empty() ? "" : ", ") + f;
                    Note("lease held (" + list + ", " + std::to_string(in.lease_ms) + " ms)");
                }
                phase_ = Phase::kHeld;
                features_ = in.features;
                lease_ms_ = in.lease_ms > 0 ? in.lease_ms : 3000;
                last_sent_.reset();  // the state goes at the next tick
            } else if (in.status == "blocked") {
                phase_ = Phase::kBlocked;
                retry_at_ = now + static_cast<std::uint64_t>(translator_.retry_ms);
                take_.clear();
                Note("blocked (another program used the channel)");
            } else if (in.status == "unsupported") {
                phase_ = Phase::kUnsupported;
                Note("the keyboard speaks another tide major");
            } else {
                phase_ = Phase::kError;
                Note("hello answered " + in.status);
            }
            return std::nullopt;
        case Incoming::Kind::kState:
            if (in.status == "no-lease" && phase_ == Phase::kHeld) {
                // The lease ran out (the keyboard was busy, or the PC slept): start again.
                phase_ = Phase::kConnecting;
                take_.clear();
                down_.clear();
                last_sent_at_.reset();
                Note("the lease ran out; connecting again");
            }
            return std::nullopt;
        case Incoming::Kind::kKey:
            if (in.dropped) Note("events were dropped");
            // A key bada doesn't take reaches Windows too, and is read there: only taken keys run.
            if (in.down) {
                if (phase_ != Phase::kHeld || !take_.count(in.code)) return std::nullopt;
                down_.insert(in.code);
                return Key{in.code, true, in.mods};
            }
            if (down_.erase(in.code)) return Key{in.code, false, in.mods};
            return std::nullopt;
        case Incoming::Kind::kOther:
            return std::nullopt;
    }
    return std::nullopt;
}

json::Value Link::Published() const {
    json::Value v = json::Object();
    v.Set("translator", json::Str(translator_.id));
    v.Set("state", json::Str(PhaseName(phase_)));
    v.Set("version", json::Str(std::to_string(kMajor) + "." + std::to_string(kMinor)));
    json::Value features = json::Array();
    for (const std::string& f : features_) features.Push(json::Str(f));
    v.Set("features", std::move(features));
    json::Value take = json::Array();
    for (const std::string& k : take_) take.Push(json::Str(k));
    v.Set("take", std::move(take));
    v.Set("conflict", conflict_.empty() ? json::Null() : json::Str(conflict_));
    return v;
}

}  // namespace bada::tide
