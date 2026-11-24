import Foundation

// The key table (the standard, §9): what language keys, Caps Lock and bound chords do.
// A pure type: no AppKit, no engine, no clock. Tests/KeyTableCases replays the standard's cases on it.

/// The two modes (the standard, §2).
enum Mode: String, Equatable {
    case muhenkan, henkan
    var other: Mode { self == .muhenkan ? .henkan : .muhenkan }
}

/// A henkan profile, as the harness needs it: its id, its name for menus, its layouts.
struct Profile: Equatable {
    var id: String
    var name: String
    var layouts: [String]
}

/// The modifiers of a press, in the table's order: ctrl, alt, shift, meta.
struct Mods: OptionSet, Hashable {
    let rawValue: Int
    static let ctrl = Mods(rawValue: 1)
    static let alt = Mods(rawValue: 2)
    static let shift = Mods(rawValue: 4)
    static let meta = Mods(rawValue: 8)
    static let named: [(name: String, mod: Mods)] = [("ctrl", .ctrl), ("alt", .alt), ("shift", .shift), ("meta", .meta)]
}

/// What a binding is on: one key with its modifiers, or a pair of language keys (§9.3).
enum Chord: Hashable {
    case key(String, Mods)
    /// Two language keys held together, in alphabetical order.
    case pair(String, String)

    static func pair(of a: String, _ b: String) -> Chord { a < b ? .pair(a, b) : .pair(b, a) }

    /// Reads `on`: the modifiers in the order ctrl, alt, shift, meta, then the key's code; or two
    /// language keys joined by +. nil: not a chord.
    init?(_ text: String) {
        let parts = text.split(separator: "+", omittingEmptySubsequences: false).map(String.init)
        guard let code = parts.last, Chord.isCode(code) else { return nil }
        if parts.count == 2, KeyTable.languageKeys.contains(parts[0]), KeyTable.languageKeys.contains(code) {
            guard parts[0] != code else { return nil }
            self = Chord.pair(of: parts[0], code)
            return
        }
        var mods: Mods = []
        var last = -1
        for name in parts.dropLast() {
            guard let at = Mods.named.firstIndex(where: { $0.name == name }), at > last else { return nil }
            last = at
            mods.insert(Mods.named[at].mod)
        }
        self = .key(code, mods)
    }

    /// As `on` writes it.
    var text: String {
        switch self {
        case .key(let code, let mods): return (Mods.named.filter { mods.contains($0.mod) }.map(\.name) + [code]).joined(separator: "+")
        case .pair(let a, let b): return a + "+" + b
        }
    }

    /// A W3C code: a capital, then letters and digits.
    private static func isCode(_ text: String) -> Bool {
        guard let first = text.unicodeScalars.first, ("A"..."Z").contains(first) else { return false }
        return text.unicodeScalars.allSatisfy { ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) }
    }
}

/// One action of a binding's `do` list (§9.2).
enum KeyAction: Equatable {
    case modeToggle
    case mode(Mode)
    case profile(String)
    case profileFirst, profileNext, profilePrev, profileLast
    case menuNext, menuPrev, fix, widthFull, widthHalf
    /// `<layout id>.<action id>`.
    case layout(String, action: String)
    case pass, none

    init?(_ text: String) {
        switch text {
        case "mode.toggle": self = .modeToggle
        case "mode:muhenkan": self = .mode(.muhenkan)
        case "mode:henkan": self = .mode(.henkan)
        case "profile.first": self = .profileFirst
        case "profile.next": self = .profileNext
        case "profile.prev": self = .profilePrev
        case "profile.last": self = .profileLast
        case "menu.next": self = .menuNext
        case "menu.prev": self = .menuPrev
        case "fix": self = .fix
        case "width.full": self = .widthFull
        case "width.half": self = .widthHalf
        case "pass": self = .pass
        case "none": self = .none
        default:
            if text.hasPrefix("profile:") {
                let id = String(text.dropFirst("profile:".count))
                guard KeyTable.isProfileID(id) else { return nil }
                self = .profile(id)
                return
            }
            // A layout's action: a slug, a dot, the action's id.
            let parts = text.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
            guard parts.count == 2, KeyTable.isLayoutID(parts[0]), KeyTable.isSlug(parts[1]) else { return nil }
            self = .layout(parts[0], action: parts[1])
        }
    }

    /// As the `do` list writes it.
    var name: String {
        switch self {
        case .modeToggle: return "mode.toggle"
        case .mode(let mode): return "mode:" + mode.rawValue
        case .profile(let id): return "profile:" + id
        case .profileFirst: return "profile.first"
        case .profileNext: return "profile.next"
        case .profilePrev: return "profile.prev"
        case .profileLast: return "profile.last"
        case .menuNext: return "menu.next"
        case .menuPrev: return "menu.prev"
        case .fix: return "fix"
        case .widthFull: return "width.full"
        case .widthHalf: return "width.half"
        case .layout(let layout, let action): return layout + "." + action
        case .pass: return "pass"
        case .none: return "none"
        }
    }
}

/// What the harness knows when a key is pressed (§9.1): its own mode and profile, and the last reply.
struct KeyState: Equatable {
    var mode: Mode
    /// The profile last selected; nil before the first.
    var profile: String?
    /// The profile in use before that one.
    var previous: String?
    /// Rows in the last reply's menu.
    var menuRows = 0
    /// The last reply's `staged`.
    var staged = ""
    /// Whether the lock is on.
    var capsLock = false
    /// Another input source than the harness's own is selected, so no mode is in use: no `when`
    /// holds, and an action that enters a mode or a profile applies even if it was the last one.
    /// Only a harness that sees keys while it isn't selected (the Mac's key tap) ever sets it.
    var away = false

    /// The state after a decision: a `select` changes the mode and the profile, and the lock may change.
    mutating func apply(_ decision: Decision) {
        if case .select(let mode, let profile) = decision.then {
            let changes = mode != self.mode || (mode == .henkan && profile != self.profile)
            if mode == .henkan, let profile, profile != self.profile {
                if let before = self.profile { previous = before }
                self.profile = profile
            }
            self.mode = mode
            away = false
            if changes {
                // A select that changes the mode or the profile commits what was staged.
                menuRows = 0
                staged = ""
            }
        }
        if let lock = decision.capsLock { capsLock = lock }
    }
}

/// What must happen for a press.
enum Outcome: Equatable {
    /// The harness sends `select` (and switches the OS's input source if the mode changes).
    case select(Mode, String?)
    /// The harness sends `action` with this name.
    case action(String)
    /// The key isn't the table's: it is fed to the engine as a `key` event.
    case key
    /// The harness flushes and types this text itself.
    case type(String)
    /// The app or the OS has the key.
    case pass
    /// Nothing.
    case swallow
    /// The settings window opens on its onboarding (§8.3).
    case onboarding
}

struct Decision: Equatable {
    var then: Outcome
    /// The index of the binding that was chosen; nil when none matched.
    var binding: Int?
    /// The action of its `do` list that ran; nil when none applied.
    var ran: String?
    /// The lock afterwards, when the press sets it; nil: the lock stays as it is.
    var capsLock: Bool?
}

struct KeyTable: Equatable {
    enum When: Equatable {
        case muhenkan, henkan
        case profile(String)

        init?(_ text: String) {
            switch text {
            case "muhenkan": self = .muhenkan
            case "henkan": self = .henkan
            default:
                guard text.hasPrefix("profile:") else { return nil }
                let id = String(text.dropFirst("profile:".count))
                guard KeyTable.isProfileID(id) else { return nil }
                self = .profile(id)
            }
        }
    }

    struct Binding: Equatable {
        var on: Chord
        var actions: [KeyAction]
        var when: When?
    }

    enum CapsLockSetting: String { case respect, transient }
    enum CapsRule: String { case allcaps, upperSnake = "upper_snake" }

    var bindings: [Binding] = []
    /// The profiles `profile.next` and `profile.prev` go through, in order.
    var cycle: [String] = []
    /// The profile that `mode:henkan` and the toggle enter: "last", or a profile's id.
    var henkanEnters = "last"
    var capsLock = CapsLockSetting.respect
    var capsRule = CapsRule.allcaps

    /// The language keys of §9.3 that can make a pair: every one of the table but Caps Lock.
    static let languageKeys: Set<String> = ["Backquote", "NonConvert", "Convert", "KanaMode", "Lang2", "Lang1"]
    static let capsLockKey = "CapsLock"

    /// A key that never goes to the engine (§4).
    static func isLanguageKey(_ code: String) -> Bool { languageKeys.contains(code) || code == capsLockKey }

    // MARK: Deciding

    /// The binding for a press: the most specific one that holds (a profile's, then a mode's, then
    /// one with no `when`); among equals, the later one in the list. `chords`: whether the harness
    /// can see chords on other keys than the language keys now (§9.3); false leaves those bindings out.
    func binding(for press: Chord, state: KeyState, chords: Bool = true) -> Int? {
        if !chords, case .key(let code, _) = press, !KeyTable.isLanguageKey(code) { return nil }
        var best: (index: Int, rank: Int)?
        for (index, binding) in bindings.enumerated() where binding.on == press {
            let rank: Int
            switch binding.when {
            case nil: rank = 0
            case .muhenkan: if state.away || state.mode != .muhenkan { continue }; rank = 1
            case .henkan: if state.away || state.mode != .henkan { continue }; rank = 1
            case .profile(let id): if state.away || state.mode != .henkan || state.profile != id { continue }; rank = 2
            }
            if best == nil || rank >= best!.rank { best = (index, rank) }
        }
        return best?.index
    }

    /// What a press does (§9.1, §9.2, §9.4).
    func decide(_ press: Chord, state: KeyState, profiles: [Profile], chords: Bool = true) -> Decision {
        if let index = binding(for: press, state: state, chords: chords) {
            for action in bindings[index].actions {
                guard let then = run(action, state: state, profiles: profiles) else { continue }
                var decision = Decision(then: then, binding: index, ran: action.name)
                // transient: the lock is turned off when muhenkan is left.
                if case .select(.henkan, _) = then, state.mode == .muhenkan, capsLock == .transient { decision.capsLock = false }
                return decision
            }
            return Decision(then: .swallow, binding: index, ran: nil)
        }
        guard case .key(let code, let mods) = press else {
            // A pair with no binding that holds: both keys are up, and nothing runs.
            return Decision(then: .swallow)
        }
        if code == KeyTable.capsLockKey {
            // Unbound, the key is the lock.
            if capsLock == .transient && state.mode == .henkan { return Decision(then: .swallow, capsLock: false) }
            return Decision(then: .pass, capsLock: !state.capsLock)
        }
        // A language key the table has no binding for goes to the OS as it is, never to the engine.
        if KeyTable.languageKeys.contains(code) { return Decision(then: .pass) }
        // upper_snake: with the lock on in muhenkan, Space types _ and ⌥Space a space.
        if code == "Space", capsRule == .upperSnake, state.mode == .muhenkan, state.capsLock,
           !mods.contains(.ctrl), !mods.contains(.meta) {
            return Decision(then: .type(mods.contains(.alt) ? " " : "_"))
        }
        return Decision(then: .key)
    }

    /// The profile `mode:henkan` and the toggle enter (`henkan_enters`): the one last used, or the
    /// one named; the first of the list when that one isn't there. nil: there is no profile at all.
    func entering(last: String?, profiles: [Profile]) -> String? {
        guard let first = profiles.first else { return nil }
        let wanted = henkanEnters == "last" ? last : henkanEnters
        if let wanted, profiles.contains(where: { $0.id == wanted }) { return wanted }
        return first.id
    }

    /// What an action does now; nil when it doesn't apply (it would change nothing).
    private func run(_ action: KeyAction, state: KeyState, profiles: [Profile]) -> Outcome? {
        func has(_ id: String) -> Bool { profiles.contains { $0.id == id } }
        let inUse = state.mode == .henkan && !state.away ? state.profile : nil
        /// Into henkan with this profile, unless it is the one in use or isn't there.
        func enter(_ id: String?) -> Outcome? {
            guard let id, has(id), id != inUse else { return nil }
            return .select(.henkan, id)
        }
        func entering() -> String? { self.entering(last: state.profile, profiles: profiles) }
        switch action {
        case .modeToggle:
            if state.mode == .henkan { return .select(.muhenkan, state.profile) }
            guard let id = entering() else { return .onboarding }
            return .select(.henkan, id)
        case .mode(.muhenkan):
            return state.mode == .muhenkan && !state.away ? nil : .select(.muhenkan, state.profile)
        case .mode(.henkan):
            if state.mode == .henkan && !state.away { return nil }
            guard let id = entering() else { return .onboarding }
            return .select(.henkan, id)
        case .profile(let id):
            // With no profile at all, any action that would enter henkan opens the onboarding.
            if profiles.isEmpty { return .onboarding }
            return enter(id)
        case .profileFirst:
            if profiles.isEmpty { return .onboarding }
            return enter(profiles[0].id)
        case .profileNext, .profilePrev:
            if profiles.isEmpty { return .onboarding }
            let cycle = self.cycle.filter(has)
            guard !cycle.isEmpty else { return nil }
            let next = action == .profileNext
            if let last = state.profile, let at = cycle.firstIndex(of: last) {
                return enter(cycle[(at + (next ? 1 : cycle.count - 1)) % cycle.count])
            }
            return enter(next ? cycle.first : cycle.last)
        case .profileLast:
            if profiles.isEmpty { return .onboarding }
            return enter(state.mode == .henkan ? state.previous : state.profile)
        case .menuNext, .menuPrev: return state.menuRows > 1 && !state.away ? .action(action.name) : nil
        case .fix: return state.staged.isEmpty || state.away ? nil : .action(action.name)
        case .widthFull, .widthHalf: return state.away ? nil : .action(action.name)
        case .layout(let layout, _):
            guard let id = inUse, let profile = profiles.first(where: { $0.id == id }), profile.layouts.contains(layout) else { return nil }
            return .action(action.name)
        case .pass: return .pass
        case .none: return .swallow
        }
    }

    // MARK: Pairs

    /// The pairs the table binds, whatever their `when`.
    var pairs: Set<Chord> {
        Set(bindings.map(\.on).filter { if case .pair = $0 { return true } else { return false } })
    }

    /// The pairs with a binding that holds now (its `when`): the only ones a key waits for (§9.3).
    func pairs(holding state: KeyState) -> Set<Chord> {
        pairs.filter { binding(for: $0, state: state) != nil }
    }

    /// Every key some binding is on, alone or in a pair: what the harness has to watch for.
    var boundKeys: Set<String> {
        var keys: Set<String> = []
        for binding in bindings {
            switch binding.on {
            case .key(let code, _): keys.insert(code)
            case .pair(let a, let b): keys.insert(a); keys.insert(b)
            }
        }
        return keys
    }

    // MARK: Presets (§9.5)

    enum Preset: String {
        case jisMac = "jis-mac", jisPC = "jis-pc", noLanguageKeys = "none"
    }

    /// The bindings a settings file with no `keys.bindings` gets, by keyboard.
    static func preset(_ preset: Preset) -> [Binding] {
        func jis(leave: String, enter: String) -> [Binding] {
            [Binding(on: .key(leave, []), actions: [.mode(.muhenkan)], when: nil),
             Binding(on: .key(enter, []), actions: [.profileFirst], when: nil),
             Binding(on: .key(enter, []), actions: [.menuNext, .profileFirst], when: .henkan),
             Binding(on: .key(enter, .shift), actions: [.menuPrev], when: .henkan),
             Binding(on: .key(enter, .ctrl), actions: [.fix], when: .henkan),
             Binding(on: .key("Backquote", []), actions: [.widthFull], when: nil),
             Binding(on: .key("Backquote", .shift), actions: [.widthHalf], when: nil)]
        }
        switch preset {
        case .jisMac: return jis(leave: "Lang2", enter: "Lang1")
        case .jisPC: return jis(leave: "NonConvert", enter: "Convert")
        case .noLanguageKeys: return [Binding(on: .key(capsLockKey, []), actions: [.modeToggle], when: nil)]
        }
    }

    // MARK: Reading the `keys` part of a settings file

    /// A problem in `keys`, with a path into the settings file.
    struct Flaw: Equatable {
        var path: String
        var code: String
        var message: String
    }

    /// Reads the `keys` part. A table with no `bindings` gets the preset's; one that has them is used
    /// as it is. Any flaw means the part can't be used: the caller keeps the table it had.
    static func read(_ value: Any?, preset: Preset) -> (table: KeyTable, flaws: [Flaw]) {
        var table = KeyTable()
        table.bindings = KeyTable.preset(preset)
        guard let value, !(value is NSNull) else { return (table, []) }
        guard let keys = value as? [String: Any] else {
            return (table, [Flaw(path: "keys", code: "bad-value", message: "keys must be an object")])
        }
        var flaws: [Flaw] = []
        for name in keys.keys.sorted() where !["bindings", "cycle", "henkan_enters", "caps_lock", "caps_rule"].contains(name) {
            flaws.append(Flaw(path: "keys." + name, code: "unknown-field", message: "keys has no field \(name)"))
        }
        if let raw = keys["bindings"] {
            if let list = raw as? [Any] {
                table.bindings = []
                for (index, item) in list.enumerated() {
                    let path = "keys.bindings[\(index)]"
                    guard let item = item as? [String: Any] else {
                        flaws.append(Flaw(path: path, code: "bad-value", message: "a binding must be an object"))
                        continue
                    }
                    for name in item.keys.sorted() where !["on", "do", "when"].contains(name) {
                        flaws.append(Flaw(path: path + "." + name, code: "unknown-field", message: "a binding has no field \(name)"))
                    }
                    var on: Chord?
                    if let text = item["on"] as? String, let chord = Chord(text) {
                        on = chord
                    } else {
                        flaws.append(Flaw(path: path + ".on", code: "bad-value",
                                          message: "on must be a key with its modifiers (ctrl, alt, shift, meta, in that order), or two language keys"))
                    }
                    var actions: [KeyAction] = []
                    if let names = item["do"] as? [Any], !names.isEmpty {
                        for (at, name) in names.enumerated() {
                            if let name = name as? String, let action = KeyAction(name) {
                                actions.append(action)
                            } else {
                                flaws.append(Flaw(path: path + ".do[\(at)]", code: "unknown-action", message: "no action by that name: \(name)"))
                            }
                        }
                    } else {
                        flaws.append(Flaw(path: path + ".do", code: "bad-value", message: "do must be a list of at least one action"))
                    }
                    var when: When?
                    if let raw = item["when"] {
                        if let text = raw as? String, let parsed = When(text) {
                            when = parsed
                        } else {
                            flaws.append(Flaw(path: path + ".when", code: "bad-value", message: "when must be muhenkan, henkan or profile:<id>"))
                        }
                    }
                    if let on { table.bindings.append(Binding(on: on, actions: actions, when: when)) }
                }
            } else {
                flaws.append(Flaw(path: "keys.bindings", code: "bad-value", message: "bindings must be a list"))
            }
        }
        if let raw = keys["cycle"] {
            if let list = raw as? [String], list.allSatisfy(isProfileID), Set(list).count == list.count {
                table.cycle = list
            } else {
                flaws.append(Flaw(path: "keys.cycle", code: "bad-value", message: "cycle must be a list of different profile ids"))
            }
        }
        if let raw = keys["henkan_enters"] {
            if let text = raw as? String, isProfileID(text) {
                table.henkanEnters = text
            } else {
                flaws.append(Flaw(path: "keys.henkan_enters", code: "bad-value", message: "henkan_enters must be last or a profile id"))
            }
        }
        if let raw = keys["caps_lock"] {
            if let text = raw as? String, let setting = CapsLockSetting(rawValue: text) {
                table.capsLock = setting
            } else {
                flaws.append(Flaw(path: "keys.caps_lock", code: "bad-value", message: "caps_lock must be respect or transient"))
            }
        }
        if let raw = keys["caps_rule"] {
            if let text = raw as? String, let rule = CapsRule(rawValue: text) {
                table.capsRule = rule
            } else {
                flaws.append(Flaw(path: "keys.caps_rule", code: "bad-value", message: "caps_rule must be allcaps or upper_snake"))
            }
        }
        return (table, flaws)
    }

    // MARK: Ids

    /// `[a-z0-9-]{1,32}` (§8.1).
    static func isProfileID(_ text: String) -> Bool {
        (1...32).contains(text.unicodeScalars.count) && text.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }

    /// Lowercase letters, digits and hyphens, with no empty part.
    static func isSlug(_ text: String) -> Bool {
        let parts = text.split(separator: "-", omittingEmptySubsequences: false)
        return !parts.isEmpty && parts.allSatisfy { part in
            !part.isEmpty && part.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) }
        }
    }

    /// `<group>-<name>` (§7.1): a group of two or three letters, then at least one more part.
    static func isLayoutID(_ text: String) -> Bool {
        let parts = text.split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count >= 2, isSlug(text), text.count <= 48 else { return false }
        return (2...3).contains(parts[0].count) && parts[0].unicodeScalars.allSatisfy { ("a"..."z").contains($0) }
    }
}

extension Decision {
    init(then: Outcome, capsLock: Bool? = nil) {
        self.init(then: then, binding: nil, ran: nil, capsLock: capsLock)
    }
}

/// Presses and releases in order (§9.3): a pair of language keys runs when both are up, and a key
/// that is part of a bound pair runs late: when it is released alone, or when another key that makes
/// no bound pair with it is pressed. Every other key runs when it is pressed. "Another key" is any
/// key, a modifier included; the repeats of a held key are not presses.
struct KeySequencer {
    /// The pairs a key pressed now waits for: those with a binding that holds now
    /// (`KeyTable.pairs(holding:)`), and whose two keys the harness sees go down and up. The caller
    /// sets it before each press.
    var pairs: Set<Chord> = []

    /// Keys of bound pairs that are down and haven't run: one alone, or two that make a pair.
    private var waiting: [String] = []
    /// Of a pair that is forming: the keys already released.
    private var released: Set<String> = []
    /// Keys still down that have nothing left to run.
    private var spent: Set<String> = []

    /// What a press sets off. `late`: keys that were waiting and run now, before this one.
    /// `now`: this key's own press, or nil when it waits (or is a repeat of a key that waits).
    struct Down: Equatable {
        var late: [Chord] = []
        var now: Chord?
        var all: [Chord] { late + (now.map { [$0] } ?? []) }
    }

    /// Whether the key is one the sequencer is holding: its repeats and its release are its own.
    func holds(_ code: String) -> Bool { waiting.contains(code) || spent.contains(code) }

    /// Whether a key waits for its release.
    var isWaiting: Bool { !waiting.isEmpty }

    private func inPair(_ code: String) -> Bool {
        pairs.contains { if case .pair(let a, let b) = $0 { return a == code || b == code } else { return false } }
    }

    /// A key went down. `mayWait` false: the key runs now even if it is part of a bound pair.
    mutating func down(_ code: String, mods: Mods = [], isRepeat: Bool = false, mayWait: Bool = true) -> Down {
        // A repeat of a held key is no new press: nothing that waits runs for it.
        if isRepeat { return Down(late: [], now: holds(code) ? nil : .key(code, mods)) }
        var result = Down()
        let waits = mayWait && mods.isEmpty && inPair(code)
        if waiting.count == 1 {
            let first = waiting[0]
            if waits, first != code, pairs.contains(Chord.pair(of: first, code)) {
                waiting.append(code)
                return result
            }
            // Another key that makes no bound pair with it: it runs its own binding, before that key.
            result.late.append(.key(first, []))
            spent.insert(first)
            waiting = []
        } else if waiting.count == 2 {
            // Another key before both were released: not a pair, and nothing runs for either.
            for key in waiting where !released.contains(key) { spent.insert(key) }
            waiting = []
            released = []
        }
        if waits {
            spent.remove(code)
            waiting = [code]
        } else {
            result.now = .key(code, mods)
        }
        return result
    }

    /// A key went up. Returns what runs now: a key released alone, or a pair with both keys up.
    mutating func up(_ code: String) -> [Chord] {
        if spent.remove(code) != nil { return [] }
        guard waiting.contains(code) else { return [] }
        if waiting.count == 1 {
            waiting = []
            return [.key(code, [])]
        }
        released.insert(code)
        guard waiting.allSatisfy(released.contains) else { return [] }
        let pair = Chord.pair(of: waiting[0], waiting[1])
        waiting = []
        released = []
        return [pair]
    }

    /// A key that waited too long, or whose release was lost: one alone runs now; a pair is dropped.
    mutating func expire() -> [Chord] {
        defer { waiting = []; released = [] }
        if waiting.count == 1 {
            spent.insert(waiting[0])
            return [.key(waiting[0], [])]
        }
        for key in waiting where !released.contains(key) { spent.insert(key) }
        return []
    }

    /// Forgets every key: the releases of what was down may never come.
    mutating func reset() {
        waiting = []
        released = []
        spent = []
    }
}
