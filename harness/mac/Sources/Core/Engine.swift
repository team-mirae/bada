import Foundation

// The engine's C ABI (engine/include/bada.h; the standard, §3), wrapped: what it describes, its
// data, one session a process, one engine a text field. Everything else is an event through `send`.

/// A layout as `bada_describe` and the reply to a `layout` event list it.
struct LayoutInfo: Equatable {
    var id: String
    /// What lists show, by language. `en` is always there.
    var names: [String: String]
    /// "muhenkan" (it types directly) or "henkan" (it composes).
    var kind: String
    var read: String
    var lang: String
    var stock: Bool
    var modified: Bool

    init?(_ json: [String: Any]) {
        guard let id = json["id"] as? String else { return nil }
        self.id = id
        names = json["name"] as? [String: String] ?? [:]
        kind = json["kind"] as? String ?? ""
        read = json["read"] as? String ?? ""
        lang = json["lang"] as? String ?? "und"
        stock = json["stock"] as? Bool ?? false
        modified = json["modified"] as? Bool ?? false
    }

    /// The name in the first of these languages it has, else in English, else its id.
    func name(in languages: [String]) -> String {
        for language in languages {
            if let name = names[String(language.prefix(2))] { return name }
        }
        return names["en"] ?? id
    }
}

/// What the engine is and has (§3.2).
struct EngineDescription {
    /// The major version of the standard this harness is written for (§1).
    static let major = 2

    var json: [String: Any]
    var abiName: String
    var abiMajor: Int
    var abiMinor: Int
    var id: String
    var version: String
    var events: [String]
    var features: [String]
    var actions: [String]
    var layouts: [LayoutInfo]
    /// The names of the buffers it knows, and whether it can use each in place.
    var data: [(name: String, required: Bool, borrow: Bool)]
    /// The `engine` part of a new install's settings file.
    var defaultSettings: [String: Any]

    enum Refusal: Error, Equatable {
        case unreadable
        /// Another standard, or another major version of it.
        case abi(name: String, major: Int)
    }

    /// Reads a description. Throws when the harness must refuse the engine (§1): it isn't
    /// bada-compatible, or its major version isn't 2.
    init(text: String) throws {
        guard let json = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any],
              let abi = json["abi"] as? [String: Any] else { throw Refusal.unreadable }
        self.json = json
        abiName = abi["name"] as? String ?? ""
        abiMajor = abi["major"] as? Int ?? 0
        abiMinor = abi["minor"] as? Int ?? 0
        guard abiName == "bada-compatible", abiMajor == EngineDescription.major else {
            throw Refusal.abi(name: abiName, major: abiMajor)
        }
        let engine = json["engine"] as? [String: Any] ?? [:]
        id = engine["id"] as? String ?? ""
        version = engine["version"] as? String ?? ""
        events = json["events"] as? [String] ?? []
        features = json["features"] as? [String] ?? []
        actions = json["actions"] as? [String] ?? []
        layouts = (json["layouts"] as? [[String: Any]] ?? []).compactMap(LayoutInfo.init)
        data = (json["data"] as? [[String: Any]] ?? []).compactMap { item in
            guard let name = item["name"] as? String else { return nil }
            return (name, item["required"] as? Bool ?? false, item["borrow"] as? Bool ?? false)
        }
        defaultSettings = (json["settings"] as? [String: Any])?["defaults"] as? [String: Any] ?? [:]
    }

    /// What the linked engine says of itself. No engine is needed for it.
    static func linked() throws -> EngineDescription {
        guard let text = bada_describe() else { throw Refusal.unreadable }
        return try EngineDescription(text: String(cString: text))
    }
}

/// The engine's data (§11): named buffers, built once and shared by every session on it.
final class EngineData {
    /// One named buffer. `borrowed`: the engine may use the bytes in place, for the buffers its
    /// description marks `borrow`; they are kept alive as long as this object.
    struct Buffer {
        var name: String
        var bytes: Data
        var borrowed = false
    }

    let handle: OpaquePointer?
    /// Borrowed buffers must outlive the data (§3.4).
    private var kept: [NSData] = []

    /// nil when a buffer the engine requires is missing or can't be read. With no buffers at all the
    /// engine gets `bada_data_new(NULL, 0)`: every optional feature that needs data is off.
    init?(_ buffers: [Buffer] = []) {
        if buffers.isEmpty {
            handle = bada_data_new(nil, 0)
        } else {
            // NSData keeps its bytes in one place for as long as it lives; a mapped file isn't copied.
            let blobs = buffers.map { $0.bytes as NSData }
            let names = buffers.map { strdup($0.name)! }
            var list: [bada_buf] = []
            for (index, buffer) in buffers.enumerated() {
                list.append(bada_buf(name: UnsafePointer(names[index]), ptr: blobs[index].bytes.assumingMemoryBound(to: UInt8.self),
                                     len: blobs[index].length, flags: buffer.borrowed ? UInt32(BADA_BORROWED) : 0))
                if buffer.borrowed { kept.append(blobs[index]) }
            }
            handle = withExtendedLifetime(blobs) { list.withUnsafeBufferPointer { bada_data_new($0.baseAddress, $0.count) } }
            // Arguments are valid for the call only: the engine copied what it keeps, borrowed buffers aside.
            names.forEach { free($0) }
        }
        if handle == nil { return nil }
    }

    deinit {
        // The data first, then the buffers it may point into.
        bada_data_free(handle)
        kept = []
    }
}

/// A session (§3.1): the settings, the custom layouts and the user dictionary every text field
/// shares. One per process.
final class EngineSession {
    fileprivate let handle: OpaquePointer
    /// The data must be freed after every session on it.
    private let data: EngineData?

    /// `harness`: its id, version and features (§3.3). nil when the engine refuses it.
    init?(data: EngineData?, harness id: String, version: String, features: [String], extra: [String: Any] = [:]) {
        var object = extra
        object["harness"] = ["id": id, "version": version, "features": features] as [String: Any]
        guard let json = JSON.text(object), let handle = bada_session_new(data?.handle, json) else { return nil }
        self.handle = handle
        self.data = data
    }

    /// A session from a whole `init_json`, as a transcript's `init` line has it.
    init?(data: EngineData?, initJSON: String) {
        guard let handle = bada_session_new(data?.handle, initJSON) else { return nil }
        self.handle = handle
        self.data = data
    }

    deinit { bada_session_free(handle) }

    /// One of the session's own events: `settings`, `layout`, `dictionary` (§6.6).
    func send(_ event: [String: Any]) -> Reply {
        send(json: JSON.text(event) ?? "{}")
    }

    func send(json: String) -> Reply {
        Reply(text: String(cString: bada_send(handle, nil, json)))
    }
}

/// One text field (§3.1): its mode, its profile, its staged keys.
final class Engine {
    /// The session must be freed after every engine of it.
    let session: EngineSession
    private let handle: OpaquePointer

    init?(_ session: EngineSession) {
        guard let handle = bada_new(session.handle) else { return nil }
        self.session = session
        self.handle = handle
    }

    deinit { bada_free(handle) }

    /// One event in, one reply out. The reply's text is copied: the engine's own string only lives
    /// until the next call (§3.4).
    func send(_ event: [String: Any]) -> Reply {
        send(json: JSON.text(event) ?? "{}")
    }

    func send(json: String) -> Reply {
        Reply(text: String(cString: bada_send(session.handle, handle, json)))
    }

    func key(_ code: String, mods: Mods = [], isRepeat: Bool = false) -> Reply {
        var event: [String: Any] = ["type": "key", "key": code, "shift": mods.contains(.shift), "ctrl": mods.contains(.ctrl),
                                    "alt": mods.contains(.alt), "meta": mods.contains(.meta)]
        if isRepeat { event["repeat"] = true }
        return send(event)
    }

    func action(_ name: String) -> Reply { send(["type": "action", "name": name]) }

    func select(_ mode: Mode, profile: String?) -> Reply {
        send(["type": "select", "mode": mode.rawValue, "profile": profile ?? NSNull()])
    }

    func pick(row: Int) -> Reply { send(["type": "pick", "row": row]) }

    /// `cause`: blur, click, edit or harness (§5).
    func flush(cause: String = "harness") -> Reply { send(["type": "flush", "cause": cause]) }

    func reset() -> Reply { send(["type": "reset"]) }

    func state() -> Reply { send(["type": "state"]) }
}

enum JSON {
    /// An object as JSON text, with its keys in order so the same object is always the same text.
    static func text(_ object: Any, pretty: Bool = false) -> String? {
        guard JSONSerialization.isValidJSONObject(object) else { return nil }
        var options: JSONSerialization.WritingOptions = [.sortedKeys, .withoutEscapingSlashes]
        if pretty { options.insert(.prettyPrinted) }
        return (try? JSONSerialization.data(withJSONObject: object, options: options)).flatMap { String(data: $0, encoding: .utf8) }
    }

    static func object(_ data: Data) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    }
}
