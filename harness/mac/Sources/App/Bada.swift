import Cocoa

/// What every text field of the process shares: the engine's description, its data, the one
/// session, and the settings folder.
enum Bada {
    /// The harness's id (the standard, §12).
    static let harnessID = "com.teammirae.bada.mac"
    static let bundleID = Bundle.main.bundleIdentifier ?? "com.teammirae.inputmethod.Bada"
    /// bada's one version, from the repository's VERSION file by way of Info.plist.
    static let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0.0"
    /// What this harness can do (§3.3): it draws segments, sends `pick`, and answers a request for
    /// the text around the caret. It doesn't draw right to left.
    static let features = ["segments", "pick", "around"]

    private(set) static var description: EngineDescription?
    private(set) static var session: EngineSession?
    private(set) static var store: SettingsStore?
    /// Why there is no engine, if there is none.
    private(set) static var refusal: String?
    private static var watcher: FolderWatcher?

    /// The settings folder (§10.1). `BADA_HOME` moves it, for a test run.
    static let folder: URL = {
        if let home = ProcessInfo.processInfo.environment["BADA_HOME"], !home.isEmpty {
            return URL(fileURLWithPath: home, isDirectory: true)
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("bada", isDirectory: true)
    }()

    static var table: KeyTable { store?.table ?? KeyTable() }
    static var profiles: [Profile] { store?.profiles ?? [] }

    /// The names of the data buffers the engine was given.
    private(set) static var dataNames: [String] = []

    /// The engine's data (§11): named buffers for `bada_data_new`.
    ///
    /// Each buffer the engine knows is the file of that name in the bundle's `data` folder, mapped
    /// and not read. The engine uses in place the ones its description marks `borrow`, and copies
    /// what it keeps of the others. A buffer with no file is left out: its feature is off. Once the
    /// pack's layout is in the standard, map the pack and pass its blobs the same way.
    static func dataBuffers(for description: EngineDescription) -> [EngineData.Buffer] {
        guard let folder = Bundle.main.resourceURL?.appendingPathComponent("data", isDirectory: true),
              let files = try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
        else { return [] }
        var buffers: [EngineData.Buffer] = []
        for entry in description.data {
            guard let file = files.first(where: { $0.deletingPathExtension().lastPathComponent == entry.name }),
                  let bytes = try? Data(contentsOf: file, options: .alwaysMapped) else { continue }
            buffers.append(EngineData.Buffer(name: entry.name, bytes: bytes, borrowed: entry.borrow))
        }
        return buffers
    }

    /// Reads the engine's description, starts the session and loads the settings folder.
    /// `watch`: reload when a file of the folder changes.
    static func start(watch: Bool = true) {
        do {
            let description = try EngineDescription.linked()
            self.description = description
            let buffers = dataBuffers(for: description)
            dataNames = buffers.map(\.name)
            guard let data = EngineData(buffers) else {
                refusal = "the engine's data can't be loaded"
                return
            }
            guard let session = EngineSession(data: data, harness: harnessID, version: version, features: features) else {
                refusal = "the engine refused the session"
                return
            }
            self.session = session
            let store = SettingsStore(folder: folder, session: session, defaults: description.defaultSettings,
                                      harnessID: harnessID, autoPreset: Keyboards.preset())
            self.store = store
            store.reload()
            DiagnosticLog.note("engine \(description.id) \(description.version), bada-compatible \(description.abiMajor).\(description.abiMinor); \(store.profiles.count) profiles, \(store.problems.count) problems")
            if watch {
                // The folder is there from the first run on: other programs write into it.
                try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                watcher = FolderWatcher(folder, ignoring: FolderWatcher.isNotSettings) { reload() }
                Tide.reload()
            }
        } catch EngineDescription.Refusal.abi(let name, let major) {
            // The standard, §1: a harness must refuse an engine with another major version.
            refusal = "the engine is \(name) \(major), and this input method is for bada-compatible \(EngineDescription.major)"
        } catch {
            refusal = "the engine's description can't be read"
        }
        if let refusal { NSLog("bada: %@", refusal) }
    }

    /// The folder changed: read it again, and let everything that depends on the settings follow.
    static func reload() {
        Tide.reload()   // tide/*.json (bada-tide §9)
        guard let store else { return }
        store.autoPreset = Keyboards.preset()
        guard store.reload() else { return }
        DiagnosticLog.note("settings reloaded: \(store.profiles.count) profiles, \(store.problems.count) problems")
        Modes.settingsChanged()
    }
}
