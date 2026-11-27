import Foundation

// bada-tide (docs/bada-tide-v1.md): a translator file, its report templates and the deny list. Pure:
// no device access, so the tests can run it.

/// One template: a report's bytes, space-separated: a hex byte or a field (`{mode}`, `{lease_ms:le16}`).
struct TideTemplate: Equatable {
    enum Token: Equatable {
        case byte(UInt8)
        case field(String, width: Int, bigEndian: Bool)
    }
    let tokens: [Token]

    init?(_ text: String) {
        var tokens: [Token] = []
        for part in text.split(separator: " ") {
            if part.hasPrefix("{") && part.hasSuffix("}") {
                let inner = part.dropFirst().dropLast().split(separator: ":", omittingEmptySubsequences: false)
                guard let name = inner.first, !name.isEmpty, inner.count <= 2 else { return nil }
                var width = 1, big = false
                if inner.count == 2 {
                    switch inner[1] {
                    case "le16": width = 2
                    case "le32": width = 4
                    case "be16": width = 2; big = true
                    case "be32": width = 4; big = true
                    default: return nil
                    }
                }
                tokens.append(.field(String(name), width: width, bigEndian: big))
            } else {
                guard part.count == 2, let b = UInt8(part, radix: 16) else { return nil }
                tokens.append(.byte(b))
            }
        }
        guard !tokens.isEmpty else { return nil }
        self.tokens = tokens
    }

    /// The template's bytes, before any field, as upper-case hex ("AA AA F1"): for the deny list.
    var literalPrefix: String {
        var out: [String] = []
        for t in tokens {
            guard case .byte(let b) = t else { break }
            out.append(String(format: "%02X", b))
        }
        return out.joined(separator: " ")
    }

    /// A report of `size` bytes with the fields filled in; nil if a field is missing or the report too long.
    func encode(_ fields: [String: Int], size: Int) -> [UInt8]? {
        var out: [UInt8] = []
        for t in tokens {
            switch t {
            case .byte(let b): out.append(b)
            case .field(let name, let width, let big):
                guard let v = fields[name] else { return nil }
                var bytes = (0..<width).map { UInt8((v >> (8 * $0)) & 0xFF) }
                if big { bytes.reverse() }
                out += bytes
            }
        }
        guard out.count <= size else { return nil }
        return out + [UInt8](repeating: 0, count: size - out.count)
    }

    /// The fields of a report that matches the template (every literal byte equal); nil if it doesn't.
    func decode(_ report: [UInt8]) -> [String: Int]? {
        var fields: [String: Int] = [:]
        var i = 0
        for t in tokens {
            switch t {
            case .byte(let b):
                guard i < report.count, report[i] == b else { return nil }
                i += 1
            case .field(let name, let width, let big):
                guard i + width <= report.count else { return nil }
                var bytes = Array(report[i..<(i + width)])
                if big { bytes.reverse() }
                fields[name] = bytes.enumerated().reduce(0) { $0 | Int($1.element) << (8 * $1.offset) }
                i += width
            }
        }
        return fields
    }
}

/// A `bada-tide-translator/1` file (bada-tide §6), as a harness uses it.
struct TideTranslator {
    struct Match: Equatable { var vendorID, productID, usagePage, usage, reportSize: Int; var versionMin, versionMax: Int? }
    struct Conflict: Equatable { var id, name, reason: String; var bundleIDs: [String] }

    static let format = "bada-tide-translator/1"
    /// The tide version this harness speaks.
    static let major = 1, minor = 0
    /// The seven keys tide knows (the standard, §4).
    static let keys = ["Backquote", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1", "CapsLock"]

    var id: String
    var name: String
    var match: Match
    var helloSend, helloReply, stateSend, stateReply, keyRecv: TideTemplate
    var status: [String: Int]
    var modeValue: [String: Int]
    var profileNone: Int
    var features: [String: Int]
    var usage: [String: Int]
    var take: [String: Int]
    var edge: [String: Int]
    var dropped: Int
    var retryMs: Int
    var conflicts: [Conflict]

    enum Problem: Error, Equatable { case invalid(String), denied(String) }

    /// Reads a translator; throws `invalid` (with what's wrong) or `denied` (bada-tide §7).
    init(json: Data) throws {
        guard let o = (try? JSONSerialization.jsonObject(with: json)) as? [String: Any] else { throw Problem.invalid("not a JSON object") }
        guard o["format"] as? String == Self.format else { throw Problem.invalid("format isn't \(Self.format)") }
        func obj(_ v: Any?, _ what: String) throws -> [String: Any] {
            guard let d = v as? [String: Any] else { throw Problem.invalid("\(what) is missing") }
            return d
        }
        func int(_ d: [String: Any], _ k: String, _ what: String) throws -> Int {
            guard let n = d[k] as? Int else { throw Problem.invalid("\(what).\(k) is missing") }
            return n
        }
        func ints(_ v: Any?, _ what: String) throws -> [String: Int] {
            guard let d = v as? [String: Any] else { throw Problem.invalid("\(what) is missing") }
            var out: [String: Int] = [:]
            for (k, v) in d {
                guard let n = v as? Int else { throw Problem.invalid("\(what).\(k) isn't a number") }
                out[k] = n
            }
            return out
        }
        func template(_ d: [String: Any], _ k: String, _ what: String) throws -> TideTemplate {
            guard let s = d[k] as? String, let t = TideTemplate(s) else { throw Problem.invalid("\(what).\(k) isn't a template") }
            return t
        }
        guard let id = o["id"] as? String, !id.isEmpty else { throw Problem.invalid("id is missing") }
        self.id = id
        name = o["name"] as? String ?? id
        let m = try obj(o["match"], "match")
        match = Match(vendorID: try int(m, "vendor_id", "match"), productID: try int(m, "product_id", "match"),
                      usagePage: try int(m, "usage_page", "match"), usage: try int(m, "usage", "match"),
                      reportSize: try int(m, "report_size", "match"),
                      versionMin: m["version_min"] as? Int, versionMax: m["version_max"] as? Int)
        let version = try obj(o["version"], "version")
        guard version["major"] as? Int == Self.major else { throw Problem.invalid("version.major isn't \(Self.major)") }
        let hello = try obj(o["hello"], "hello"), state = try obj(o["state"], "state"), key = try obj(o["key"], "key")
        helloSend = try template(hello, "send", "hello")
        helloReply = try template(hello, "reply", "hello")
        stateSend = try template(state, "send", "state")
        stateReply = try template(state, "reply", "state")
        keyRecv = try template(key, "recv", "key")
        let values = try obj(o["values"], "values")
        status = try ints(values["status"], "values.status")
        for s in ["ok", "unsupported", "no-lease", "bad-value", "blocked"] where status[s] == nil {
            throw Problem.invalid("values.status.\(s) is missing")
        }
        modeValue = try ints(values["mode"], "values.mode")
        guard modeValue["muhenkan"] != nil, modeValue["henkan"] != nil else { throw Problem.invalid("values.mode needs muhenkan and henkan") }
        profileNone = try int(values, "profile_none", "values")
        features = try ints(values["features"], "values.features")
        usage = try ints(values["usage"], "values.usage")
        take = try ints(values["take"], "values.take")
        edge = try ints(values["edge"], "values.edge")
        guard edge["up"] != nil, edge["down"] != nil else { throw Problem.invalid("values.edge needs up and down") }
        guard values["mods"] as? String == "hid" else { throw Problem.invalid("values.mods isn't \"hid\"") }
        for k in usage.keys where !Self.keys.contains(k) { throw Problem.invalid("values.usage.\(k) is no language key") }
        for k in take.keys where !Self.keys.contains(k) { throw Problem.invalid("values.take.\(k) is no language key") }
        dropped = (values["flags"] as? [String: Any])?["dropped"] as? Int ?? 0
        retryMs = o["retry_ms"] as? Int ?? 10000
        conflicts = ((o["conflicts"] as? [[String: Any]]) ?? []).compactMap { c in
            guard let id = c["id"] as? String else { return nil }
            let bundles = ((c["match"] as? [String: Any])?["macos"] as? [String: Any])?["bundle_id"] as? [String] ?? []
            return Conflict(id: id, name: c["name"] as? String ?? id, reason: c["reason"] as? String ?? "", bundleIDs: bundles)
        }
        if let refused = Self.refused(hello: helloSend, state: stateSend, reportSize: match.reportSize) {
            throw Problem.denied(refused)
        }
    }

    // MARK: bada-tide §7

    static let deniedPFU = (0..<8).map { String(format: "AA AA E%X", $0) } + ["AA AA 86", "AA AA 04", "AA AA 07", "AA AA 08", "AA AA D0"]
    static let deniedVIA = ["05", "07", "0B", "0C", "12", "13"]

    /// The message whose send template begins like a known dangerous request, or nil.
    static func refused(hello: TideTemplate, state: TideTemplate, reportSize: Int) -> String? {
        for (name, t) in [("hello", hello), ("state", state)] {
            let prefix = t.literalPrefix
            if deniedPFU.contains(where: { prefix.hasPrefix($0) }) { return name }
            if reportSize == 32, let first = prefix.split(separator: " ").first, deniedVIA.contains(String(first)) { return name }
        }
        return nil
    }

    // MARK: Messages

    func hello(size: Int) -> [UInt8]? {
        helloSend.encode(["major": Self.major, "minor": Self.minor], size: size)
    }

    /// `profile`: index in the key table's cycle, or nil. `take`: codes of §4.
    func state(mode: Mode, profile: Int?, caps: Bool, take keys: Set<String>, size: Int) -> [UInt8]? {
        let mask = keys.reduce(0) { $0 | (take[$1] ?? 0) }
        return stateSend.encode(["mode": modeValue[mode.rawValue] ?? 0, "profile": profile.map { min($0, 254) } ?? profileNone,
                                 "caps": caps ? 1 : 0, "take": mask], size: size)
    }

    /// What a report from the keyboard is.
    enum Incoming: Equatable {
        case hello(status: String, features: Set<String>, leaseMs: Int)
        case state(status: String)
        case key(code: String, down: Bool, mods: Mods, seq: Int, dropped: Bool)
        case other
    }

    func read(_ report: [UInt8]) -> Incoming {
        if let f = keyRecv.decode(report) {
            guard let u = f["usage"], let code = usage.first(where: { $0.value == u })?.key else { return .other }
            let hid = f["mods"] ?? 0
            var mods: Mods = []
            if hid & 0x11 != 0 { mods.insert(.ctrl) }
            if hid & 0x22 != 0 { mods.insert(.shift) }
            if hid & 0x44 != 0 { mods.insert(.alt) }
            if hid & 0x88 != 0 { mods.insert(.meta) }
            return .key(code: code, down: f["edge"] == edge["down"], mods: mods, seq: f["seq"] ?? 0,
                        dropped: dropped != 0 && (f["flags"] ?? 0) & dropped != 0)
        }
        if let f = helloReply.decode(report) {
            let have = f["features"] ?? 0
            return .hello(status: statusName(f["status"]), features: Set(features.filter { have & $0.value != 0 }.keys),
                          leaseMs: f["lease_ms"] ?? 3000)
        }
        if let f = stateReply.decode(report) { return .state(status: statusName(f["status"])) }
        return .other
    }

    private func statusName(_ v: Int?) -> String { status.first(where: { $0.value == v })?.key ?? "unknown" }
}

extension Mods {
    /// The modifiers' names, for the helper's pipe (`ctrl`, `alt`, `shift`, `meta`).
    var names: [String] { Mods.named.filter { contains($0.mod) }.map(\.name) }
    init(names: [String]) {
        self = []
        for (name, mod) in Mods.named where names.contains(name) { insert(mod) }
    }
}
