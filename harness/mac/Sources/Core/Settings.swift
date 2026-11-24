import Foundation

// The settings folder (the standard, §10.1): settings.json, layouts/*.json and dictionary.json, read
// and fed to the session. A file that can't be used changes nothing: what was in effect stays in
// effect, and the problems are kept for the published state (§6.7).

/// What was wrong with a file of the settings folder.
struct Problem: Equatable {
    /// The file, from the folder: `settings.json`, `layouts/mine.json`, `dictionary.json`.
    var file: String
    /// Where in the file (`engine.henkan.profiles[1].layouts[0]`); "" for the whole file.
    var path: String
    var code: String
    var message: String
    /// `error`: nothing was changed. `warning`: it was taken, with this noted.
    var level: String

    var json: [String: Any] { ["file": file, "path": path, "code": code, "message": message, "level": level] }
    var isError: Bool { level == "error" }
}

/// This harness's own settings: `harness["com.teammirae.bada.mac"]` of the settings file.
struct HarnessSettings: Equatable {
    /// Which keyboard's preset a file with no `keys.bindings` gets (§9.5). nil (`"auto"`): by the
    /// keyboard macOS reports.
    var keyboard: KeyTable.Preset?
    /// Diagnostics in ~/Library/Logs/Bada.log.
    var log = false

    static func read(_ value: Any?, path: String) -> (settings: HarnessSettings, flaws: [KeyTable.Flaw], warnings: [KeyTable.Flaw]) {
        var settings = HarnessSettings()
        guard let value, !(value is NSNull) else { return (settings, [], []) }
        guard let object = value as? [String: Any] else {
            return (settings, [KeyTable.Flaw(path: path, code: "bad-value", message: "the harness's settings must be an object")], [])
        }
        var flaws: [KeyTable.Flaw] = []
        var warnings: [KeyTable.Flaw] = []
        for (name, raw) in object.sorted(by: { $0.key < $1.key }) {
            switch name {
            case "keyboard":
                if raw as? String == "auto" {
                    settings.keyboard = nil
                } else if let text = raw as? String, let preset = KeyTable.Preset(rawValue: text) {
                    settings.keyboard = preset
                } else {
                    flaws.append(KeyTable.Flaw(path: path + ".keyboard", code: "bad-value", message: "keyboard must be auto, jis-mac, jis-pc or none"))
                }
            case "log":
                if let on = raw as? Bool { settings.log = on } else {
                    flaws.append(KeyTable.Flaw(path: path + ".log", code: "bad-value", message: "log must be true or false"))
                }
            default:
                warnings.append(KeyTable.Flaw(path: path + "." + name, code: "unknown-field", message: "this harness has no setting \(name)"))
            }
        }
        return (settings, flaws, warnings)
    }
}

final class SettingsStore {
    static let format = "bada-settings/2"

    let folder: URL
    let harnessID: String
    private let session: EngineSession
    /// The `engine` part of a new install: no muhenkan layout, no profile (§8.3).
    private let defaults: [String: Any]
    /// The preset by the keyboard macOS reports, for a file that doesn't name one.
    var autoPreset: KeyTable.Preset

    // What is in effect.
    private(set) var table = KeyTable()
    private(set) var profiles: [Profile] = []
    private(set) var muhenkanLayout: String?
    private(set) var harness = HarnessSettings()
    /// The session's layouts, stock and custom.
    private(set) var layouts: [LayoutInfo] = []
    /// What the user dictionary holds: `{"positive": n, "negative": n}`.
    private(set) var dictionary: [String: Int] = [:]

    /// The bytes last read of each file, by its path from the folder.
    private var seen: [String: Data] = [:]
    /// The id each layout file fed.
    private var fed: [String: String] = [:]
    private var found: [String: [Problem]] = [:]
    private var usedPreset: KeyTable.Preset?
    private var loaded = false

    init(folder: URL, session: EngineSession, defaults: [String: Any], harnessID: String, autoPreset: KeyTable.Preset) {
        self.folder = folder
        self.session = session
        self.defaults = defaults
        self.harnessID = harnessID
        self.autoPreset = autoPreset
        table.bindings = KeyTable.preset(autoPreset)
    }

    /// What was wrong with the files last read, in the order they are fed: layouts, settings, dictionary.
    var problems: [Problem] {
        let files = found.keys.sorted { a, b in
            func rank(_ file: String) -> Int { file.hasPrefix("layouts/") ? 0 : file == "settings.json" ? 1 : 2 }
            return (rank(a), a) < (rank(b), b)
        }
        return files.flatMap { found[$0] ?? [] }
    }

    private func read(_ file: String) -> Data? {
        FileManager.default.contents(atPath: folder.appendingPathComponent(file).path)
    }

    /// Reads the folder and feeds the session what changed since the last time. Returns whether
    /// anything in effect, or any problem, changed.
    @discardableResult
    func reload() -> Bool {
        let before = snapshot()
        let layoutsChanged = reloadLayouts()
        // The settings may name a layout that has just come or gone: they are read again with it.
        let settings = read("settings.json")
        let preset = harness.keyboard ?? autoPreset
        if !loaded || settings != seen["settings.json"] || layoutsChanged || preset != usedPreset {
            seen["settings.json"] = settings
            apply(settings: settings)
        }
        let dictionary = read("dictionary.json")
        if !loaded || dictionary != seen["dictionary.json"] {
            seen["dictionary.json"] = dictionary
            apply(dictionary: dictionary)
        }
        if !loaded || layoutsChanged {
            layouts = (session.send(["type": "layout"]).json["layouts"] as? [[String: Any]] ?? []).compactMap(LayoutInfo.init)
        }
        loaded = true
        return snapshot() != before
    }

    private func snapshot() -> String {
        let state: [String: Any] = [
            "table": String(describing: table), "profiles": profiles.map { [$0.id, $0.name] + $0.layouts },
            "muhenkan": muhenkanLayout ?? "", "harness": String(describing: harness),
            "layouts": layouts.map { "\($0.id) \($0.stock) \($0.modified)" }, "problems": problems.map(\.json),
            "dictionary": dictionary,
        ]
        return JSON.text(state) ?? ""
    }

    // MARK: layouts/*.json

    private func reloadLayouts() -> Bool {
        let directory = folder.appendingPathComponent("layouts", isDirectory: true)
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? [])
            .filter { $0.hasSuffix(".json") && !$0.hasPrefix(".") }.sorted()
        let files = names.map { "layouts/" + $0 }
        var changed = false
        // A file that is gone takes its layout with it; a stock one it replaced comes back (§7.6).
        for file in seen.keys.sorted() where file.hasPrefix("layouts/") && !files.contains(file) {
            seen[file] = nil
            found[file] = nil
            if let id = fed.removeValue(forKey: file), !fed.values.contains(id) {
                _ = session.send(["type": "layout", "id": id, "value": NSNull()])
            }
            changed = true
        }
        for file in files {
            guard let bytes = read(file), bytes != seen[file] else { continue }
            seen[file] = bytes
            changed = true
            guard let layout = JSON.object(bytes) else {
                found[file] = [Problem(file: file, path: "", code: "bad-json", message: "the file isn't a JSON object", level: "error")]
                continue
            }
            let reply = session.send(["type": "layout", "value": layout])
            var problems = reply.problems.map { problem(file, $0, prefix: "") }
            if !reply.ok && !problems.contains(where: \.isError) {
                problems.append(Problem(file: file, path: reply.error?.path ?? "", code: reply.error?.code ?? "invalid",
                                        message: reply.error?.message ?? "the layout was refused", level: "error"))
            }
            if reply.ok, let id = layout["id"] as? String {
                if let other = fed.first(where: { $0.value == id && $0.key != file })?.key {
                    problems.append(Problem(file: file, path: "id", code: "duplicate-layout",
                                            message: "\(other) has the same id: the file read last is the one in use", level: "warning"))
                }
                // The file used to feed another id: that one goes, unless another file feeds it too.
                if let old = fed[file], old != id, !fed.contains(where: { $0.value == old && $0.key != file }) {
                    _ = session.send(["type": "layout", "id": old, "value": NSNull()])
                }
                fed[file] = id
            }
            found[file] = problems.isEmpty ? nil : problems
        }
        return changed
    }

    private func problem(_ file: String, _ json: [String: Any], prefix: String) -> Problem {
        let inner = json["path"] as? String ?? ""
        let path = prefix.isEmpty ? inner : inner.isEmpty ? prefix : prefix + "." + inner
        return Problem(file: file, path: path, code: json["code"] as? String ?? "invalid",
                       message: json["message"] as? String ?? "", level: json["level"] as? String ?? "error")
    }

    // MARK: settings.json

    private func apply(settings data: Data?) {
        let file = "settings.json"
        usedPreset = harness.keyboard ?? autoPreset
        guard let data else {
            // No file: a new install. The engine's defaults, and the keyboard's preset.
            let reply = session.send(["type": "settings", "set": defaults])
            take(engine: reply)
            harness = HarnessSettings()
            usedPreset = autoPreset
            table = KeyTable()
            table.bindings = KeyTable.preset(autoPreset)
            found[file] = nil
            return
        }
        func refuse(_ problems: [Problem]) { found[file] = problems }
        guard let object = JSON.object(data) else {
            return refuse([Problem(file: file, path: "", code: "bad-json", message: "the file isn't a JSON object", level: "error")])
        }
        guard object["format"] as? String == SettingsStore.format else {
            return refuse([Problem(file: file, path: "format", code: "bad-format", message: "the file isn't \(SettingsStore.format)", level: "error")])
        }
        var problems: [Problem] = []
        func add(_ flaws: [KeyTable.Flaw], level: String) {
            problems += flaws.map { Problem(file: file, path: $0.path, code: $0.code, message: $0.message, level: level) }
        }
        for name in object.keys.sorted() where !["format", "engine", "keys", "harness"].contains(name) {
            add([KeyTable.Flaw(path: name, code: "unknown-field", message: "a settings file has no part \(name)")], level: "error")
        }
        // The harness's own part first: it may name the keyboard whose preset the table gets.
        var own = HarnessSettings()
        if let raw = object["harness"], !(raw is NSNull) {
            if let parts = raw as? [String: Any] {
                let read = HarnessSettings.read(parts[harnessID], path: "harness.\(harnessID)")
                own = read.settings
                add(read.flaws, level: "error")
                add(read.warnings, level: "warning")
            } else {
                add([KeyTable.Flaw(path: "harness", code: "bad-value", message: "harness must be an object")], level: "error")
            }
        }
        let preset = own.keyboard ?? autoPreset
        let keys = KeyTable.read(object["keys"], preset: preset)
        add(keys.flaws, level: "error")
        // The engine checks its own part: first only checked, so a file with any error changes nothing.
        var engine = defaults
        if let raw = object["engine"], !(raw is NSNull) {
            if let part = raw as? [String: Any] { engine = part } else {
                add([KeyTable.Flaw(path: "engine", code: "bad-value", message: "engine must be an object")], level: "error")
            }
        }
        let checked = session.send(["type": "settings", "set": engine, "dry": true])
        var theirs = checked.problems.map { problem(file, $0, prefix: "engine") }
        if !checked.ok && !theirs.contains(where: \.isError) {
            theirs.append(Problem(file: file, path: "engine", code: checked.error?.code ?? "invalid",
                                  message: checked.error?.message ?? "the engine refused its settings", level: "error"))
        }
        problems += theirs
        guard !problems.contains(where: \.isError) else { return refuse(problems) }
        let reply = session.send(["type": "settings", "set": engine])
        guard reply.ok else {
            return refuse(problems + [Problem(file: file, path: "engine", code: reply.error?.code ?? "invalid",
                                              message: reply.error?.message ?? "the engine refused its settings", level: "error")])
        }
        take(engine: reply)
        table = keys.table
        harness = own
        usedPreset = preset
        found[file] = problems.isEmpty ? nil : problems
    }

    /// The profiles and the muhenkan layout, from the settings the engine says are in effect.
    private func take(engine reply: Reply) {
        guard reply.ok, let settings = reply.json["settings"] as? [String: Any] else { return }
        muhenkanLayout = (settings["muhenkan"] as? [String: Any])?["layout"] as? String
        let list = (settings["henkan"] as? [String: Any])?["profiles"] as? [[String: Any]] ?? []
        profiles = list.compactMap { item in
            guard let id = item["id"] as? String else { return nil }
            return Profile(id: id, name: item["name"] as? String ?? id, layouts: item["layouts"] as? [String] ?? [])
        }
    }

    // MARK: dictionary.json

    private func apply(dictionary data: Data?) {
        let file = "dictionary.json"
        var value: Any = NSNull()   // no file: no user dictionary
        if let data {
            guard let object = JSON.object(data) else {
                found[file] = [Problem(file: file, path: "", code: "bad-json", message: "the file isn't a JSON object", level: "error")]
                return
            }
            value = object
        }
        let reply = session.send(["type": "dictionary", "value": value])
        var problems = reply.problems.map { problem(file, $0, prefix: "") }
        if !reply.ok && !problems.contains(where: \.isError) {
            problems.append(Problem(file: file, path: reply.error?.path ?? "", code: reply.error?.code ?? "invalid",
                                    message: reply.error?.message ?? "the dictionary was refused", level: "error"))
        }
        if reply.ok { dictionary = reply.json["dictionary"] as? [String: Int] ?? [:] }
        found[file] = problems.isEmpty ? nil : problems
    }
}

/// The first run (§8.3): the easy mode picks the muhenkan layout and one henkan layout.
enum Onboarding {
    enum Failure: Error {
        /// There is a settings.json already, and it isn't a `bada-settings/2` object: it is left alone.
        case unreadable
    }

    /// The settings file after the easy mode: `muhenkan.layout`, and one more profile with that one
    /// layout. Everything else of an existing file is kept.
    static func settings(muhenkan: String, henkan: String, name: String, harnessID: String, existing: Data?) throws -> Data {
        var file: [String: Any] = ["format": SettingsStore.format, "keys": [String: Any](), "harness": [harnessID: [String: Any]()]]
        if let existing {
            guard let object = JSON.object(existing), object["format"] as? String == SettingsStore.format else { throw Failure.unreadable }
            file = object
        }
        var engine = file["engine"] as? [String: Any] ?? [:]
        engine["muhenkan"] = ["layout": muhenkan]
        var part = engine["henkan"] as? [String: Any] ?? [:]
        var profiles = part["profiles"] as? [[String: Any]] ?? []
        let taken = Set(profiles.compactMap { $0["id"] as? String })
        var id = "main"
        var number = 2
        while taken.contains(id) {
            id = "main-\(number)"
            number += 1
        }
        profiles.append(["id": id, "name": name, "layouts": [henkan]])
        part["profiles"] = profiles
        engine["henkan"] = part
        file["engine"] = engine
        guard let text = JSON.text(file, pretty: true) else { throw Failure.unreadable }
        return Data((text + "\n").utf8)
    }

}

enum AtomicFile {
    /// Writes a file of the settings folder as the standard asks: a temporary file, then a rename.
    static func write(_ data: Data, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let temporary = url.deletingLastPathComponent().appendingPathComponent(".\(url.lastPathComponent).\(ProcessInfo.processInfo.processIdentifier).tmp")
        try data.write(to: temporary)
        if rename(temporary.path, url.path) != 0 {
            try? FileManager.default.removeItem(at: temporary)
            throw CocoaError(.fileWriteUnknown)
        }
    }
}
