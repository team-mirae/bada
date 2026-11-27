import Cocoa

/// bada-tide (docs/bada-tide-v1.md) in the input method: keyboards that report their language keys to
/// bada directly. The devices are the helper's (TideHelper, `Bada --tide-helper`), started while the
/// settings folder has a translator in `tide/`; this end tells it bada's state and runs the key events
/// it passes up through the key table.
///
/// Nothing is saved. If bada quits, the helper's stdin closes and it exits; if the helper dies, the
/// leases run out by themselves and bada starts it again.
enum Tide {
    private static var helper: Process?
    private static var input: FileHandle?
    private static var pending = Data()
    private static var stopping = false
    private static var lastState: String?

    /// The keys tide reads now (for `harness.keys.available`).
    private(set) static var availableKeys: Set<String> = []
    /// What `harness.tide` publishes (bada-tide §8).
    private(set) static var published: [[String: Any]] = []
    private(set) static var problems: [Problem] = []

    static var folder: URL { Bada.folder.appendingPathComponent("tide") }

    private static var hasTranslators: Bool {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return files.contains { $0.hasSuffix(".json") }
    }

    /// The settings folder changed (or bada started): start, tell or stop the helper.
    static func reload() {
        if hasTranslators {
            if helper == nil { start() } else { send(["reload": true]) }
        } else {
            stop()
        }
    }

    private static func start() {
        guard let path = Bundle.main.executablePath else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = ["--tide-helper"]
        let toHelper = Pipe(), fromHelper = Pipe()
        p.standardInput = toHelper
        p.standardOutput = fromHelper
        p.standardError = FileHandle.nullDevice
        fromHelper.fileHandleForReading.readabilityHandler = { handle in
            let chunk = handle.availableData
            DispatchQueue.main.async { received(chunk) }
        }
        p.terminationHandler = { _ in
            DispatchQueue.main.async {
                helper = nil
                input = nil
                pending = Data()
                lastState = nil
                clear()
                if !stopping {
                    DiagnosticLog.note("tide: the helper stopped; starting it again")
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { if helper == nil && hasTranslators { start() } }
                }
            }
        }
        do {
            stopping = false
            try p.run()
            helper = p
            input = toHelper.fileHandleForWriting
            DiagnosticLog.note("tide: helper started (pid \(p.processIdentifier))")
            changed()
        } catch {
            DiagnosticLog.note("tide: couldn't start the helper (\(error))")
        }
    }

    private static func stop() {
        guard let p = helper else { return }
        stopping = true
        try? input?.close()   // the helper exits on end of input; its leases run out by themselves
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { if p.isRunning { p.terminate() } }
        helper = nil
        input = nil
        clear()
    }

    private static func clear() {
        let had = !published.isEmpty || !availableKeys.isEmpty
        published = []
        availableKeys = []
        problems = []
        if had {
            KeyRouter.tableChanged()
            StatePublisher.refresh()
        }
    }

    private static func send(_ object: [String: Any]) {
        guard let input, let line = JSON.text(object) else { return }
        do { try input.write(contentsOf: Data((line + "\n").utf8)) } catch {}
    }

    /// Mode, profile, Caps Lock or the key table may have changed: tell the helper when it did.
    /// It takes the language keys the key table binds, while one of bada's modes is the selected
    /// input source; never Caps Lock, which bada reads with macOS's own state (CapsLock).
    static func changed() {
        guard helper != nil else { return }
        let take = Modes.isOurs ? Bada.table.boundKeys.intersection(TideTranslator.keys).subtracting(["CapsLock"]) : []
        let state: [String: Any] = ["mode": Modes.current.rawValue,
                                    "profile": Modes.profile.flatMap { Bada.table.cycle.firstIndex(of: $0) } ?? NSNull(),
                                    "caps": CapsLock.isOn(NSEvent.modifierFlags), "take": take.sorted()]
        guard let text = JSON.text(state), text != lastState else { return }
        lastState = text
        send(["state": state])
    }

    private static func received(_ chunk: Data) {
        pending.append(chunk)
        while let nl = pending.firstIndex(of: 10) {
            let line = pending[pending.startIndex..<nl]
            pending.removeSubrange(pending.startIndex...nl)
            guard let o = (try? JSONSerialization.jsonObject(with: Data(line))) as? [String: Any] else { continue }
            if let k = o["key"] as? [String: Any], let code = k["code"] as? String {
                KeyRouter.tide(code, down: k["down"] as? Bool ?? false, mods: Mods(names: k["mods"] as? [String] ?? []))
            }
            if let s = o["status"] as? [String: Any] {
                published = s["links"] as? [[String: Any]] ?? []
                availableKeys = Set(s["available"] as? [String] ?? [])
                problems = (s["problems"] as? [[String: Any]] ?? []).map {
                    Problem(file: $0["file"] as? String ?? "", path: $0["path"] as? String ?? "", code: $0["code"] as? String ?? "",
                            message: $0["message"] as? String ?? "", level: $0["level"] as? String ?? "error")
                }
                DiagnosticLog.note("tide: \(published.map { "\($0["translator"] ?? "?")=\($0["state"] ?? "?")" }.joined(separator: ", "))")
                KeyRouter.tableChanged()
                StatePublisher.refresh()
            }
        }
    }
}
