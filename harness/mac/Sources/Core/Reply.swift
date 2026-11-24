import Foundation

// A reply (the standard, §6), read from the engine's JSON. A missing field is its default.

struct Preedit: Equatable {
    enum Kind: String { case live, fixed, picked }
    struct Segment: Equatable {
        var len: Int
        var kind: Kind
    }

    var text = ""
    /// The caret's place in `text`, in Unicode scalar values.
    var caret = 0
    var segments: [Segment] = []

    init() {}

    init(_ json: [String: Any]?) {
        text = json?["text"] as? String ?? ""
        let count = text.unicodeScalars.count
        caret = min(max(json?["caret"] as? Int ?? count, 0), count)
        for item in json?["segments"] as? [[String: Any]] ?? [] {
            guard let len = item["len"] as? Int, len > 0 else { continue }
            segments.append(Segment(len: len, kind: Kind(rawValue: item["kind"] as? String ?? "") ?? .live))
        }
    }

    /// A UTF-16 offset in `text` for an offset in Unicode scalar values.
    func utf16Offset(scalars: Int) -> Int {
        text.unicodeScalars.prefix(max(scalars, 0)).reduce(0) { $0 + UTF16.width($1) }
    }

    /// The text cut into runs to draw, as UTF-16 ranges. Segments that don't add up to the text are
    /// not trusted: the whole text is then one live run.
    var runs: [(range: NSRange, kind: Kind)] {
        let count = text.unicodeScalars.count
        guard count > 0 else { return [] }
        guard !segments.isEmpty, segments.reduce(0, { $0 + $1.len }) == count else {
            return [(NSRange(location: 0, length: text.utf16.count), .live)]
        }
        var runs: [(NSRange, Kind)] = []
        var at = 0
        for segment in segments {
            let start = utf16Offset(scalars: at)
            let end = utf16Offset(scalars: at + segment.len)
            runs.append((NSRange(location: start, length: end - start), segment.kind))
            at += segment.len
        }
        return runs
    }
}

struct Menu: Equatable {
    struct Row: Equatable {
        var text: String
        /// The signed index: 0 the top reading, 1 and up the others, −1 and −2 script forms.
        var entry: Int
        /// Romaji, a reading, a name.
        var note: String?
        /// BCP 47: the harness picks the font by it.
        var lang: String?
        /// The layout whose reading it is.
        var layout: String?
    }

    var rows: [Row] = []
    /// The highlighted row, or nil for none.
    var selected: Int?
    /// One row of single characters.
    var strip = false

    init() {}

    init(_ json: [String: Any]?) {
        for item in json?["rows"] as? [[String: Any]] ?? [] {
            rows.append(Row(text: item["text"] as? String ?? "", entry: item["entry"] as? Int ?? 0,
                            note: item["note"] as? String, lang: item["lang"] as? String, layout: item["layout"] as? String))
        }
        if let selected = json?["selected"] as? Int, rows.indices.contains(selected) { self.selected = selected }
        strip = json?["strip"] as? Bool ?? false
    }
}

/// What the engine needs from the harness to finish (§6.1 `ask`).
enum Ask: Equatable {
    /// The selected text; the string says what for.
    case selection(String)
    /// The text around the caret, at most this many characters.
    case around(before: Int, after: Int)

    init?(_ json: Any?) {
        guard let json = json as? [String: Any] else { return nil }
        if let what = json["selection"] as? String {
            self = .selection(what)
        } else if let around = json["around"] as? [String: Any] {
            self = .around(before: around["before"] as? Int ?? 0, after: around["after"] as? Int ?? 0)
        } else {
            return nil
        }
    }
}

struct ReplyError: Equatable {
    var code: String
    var message: String
    var path: String?
}

struct Reply {
    var ok = false
    var error: ReplyError?
    var commit = ""
    /// `commit` replaces the selection even when text is marked. false: the marked text, or the
    /// selection when nothing is marked.
    var replacesSelection = false
    var consumed = false
    /// With `consumed` false on a typing key: the text the key should type. nil: the engine has no say.
    var types: String?
    var preedit = Preedit()
    var menu = Menu()
    var mode: Mode?
    var profile: String?
    /// What `flush` would commit now.
    var staged = ""
    var ask: Ask?
    var rtl = false
    /// The whole reply, for the fields of the session's events (§6.6) and for the state (§6.7).
    var json: [String: Any] = [:]
    /// The reply as the engine wrote it.
    var text = ""

    /// The reply for when there is no engine to ask: nothing changed.
    static let none: Reply = {
        var reply = Reply()
        reply.error = ReplyError(code: "bad-event", message: "no engine", path: nil)
        return reply
    }()

    init() {}

    init(text: String) {
        self.text = text
        guard let object = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any] else {
            error = ReplyError(code: "bad-json", message: "the reply isn't a JSON object", path: nil)
            return
        }
        json = object
        ok = object["ok"] as? Bool ?? false
        if let e = object["error"] as? [String: Any] {
            error = ReplyError(code: e["code"] as? String ?? "bad-event", message: e["message"] as? String ?? "", path: e["path"] as? String)
        } else if !ok {
            error = ReplyError(code: "bad-event", message: "", path: nil)
        }
        commit = object["commit"] as? String ?? ""
        replacesSelection = object["replace"] as? String == "selection"
        consumed = object["consumed"] as? Bool ?? false
        types = object["types"] as? String
        preedit = Preedit(object["preedit"] as? [String: Any])
        menu = Menu(object["menu"] as? [String: Any])
        mode = (object["mode"] as? String).flatMap(Mode.init(rawValue:))
        profile = object["profile"] as? String
        staged = object["staged"] as? String ?? ""
        ask = Ask(object["ask"])
        rtl = object["dir"] as? String == "rtl"
    }

    /// The problems of a session event's reply (§6.6).
    var problems: [[String: Any]] { json["problems"] as? [[String: Any]] ?? [] }
}
