import Carbon.HIToolbox
import Cocoa
import InputMethodKit

extension Mods {
    init(_ flags: NSEvent.ModifierFlags) {
        self = []
        if flags.contains(.control) { insert(.ctrl) }
        if flags.contains(.option) { insert(.alt) }
        if flags.contains(.shift) { insert(.shift) }
        if flags.contains(.command) { insert(.meta) }
    }

    init(_ flags: CGEventFlags) {
        self = []
        if flags.contains(.maskControl) { insert(.ctrl) }
        if flags.contains(.maskAlternate) { insert(.alt) }
        if flags.contains(.maskShift) { insert(.shift) }
        if flags.contains(.maskCommand) { insert(.meta) }
    }
}

/// Presses and releases from three places, put in order and run through the key table (the
/// standard, §9): the key tap (every key, once Accessibility is allowed), the HID reader (the PC
/// boards' language keys and Caps Lock, with Input Monitoring) and the input method itself (the
/// keys macOS hands it). The table decides (KeyTable, a pure type); this does what it decided.
enum KeyRouter {
    private static var sequencer = KeySequencer()
    /// A press whose key isn't known yet. カタカナ arrives as かな, and the HID report that says so
    /// may come just before the key event or just after.
    private enum Undecided {
        /// A かな or 英数 key event, already kept from the app: maybe the echo of a HID key.
        case event(code: String, keyCode: Int, mods: Mods, at: Date)
        /// A HID key: maybe a かな or 英数 event follows, maybe it arrives as nothing (変換).
        case hid(code: String, at: Date)
    }
    private static var undecided: Undecided?
    /// Key codes whose release belongs to a HID key: nothing runs for it.
    private static var echoes: Set<Int> = []
    /// Key codes whose press was kept from the app: their release is kept too.
    private static var kept: Set<Int> = []
    /// When a key began to wait for its release (§9.3), to give up on a release that never comes.
    private static var waitingSince = Date.distantPast

    /// How long a key event waits for the HID report that says which key it was.
    private static let eventWindow = 0.03
    /// How long a HID key waits for the key event macOS makes of it.
    private static let hidWindow = 0.08
    /// The keys read at the HID level, by their code.
    static let hidKeys: Set<String> = ["NonConvert", "Convert", "KanaMode"]
    /// The language keys that come as key events, which the tap can keep and hand back.
    private static let tapKeys: Set<String> = ["Lang1", "Lang2", "Backquote"]

    /// What the table decides from (§9.1): the harness's mode and profile, and the last reply.
    static func state() -> KeyState {
        let field = BadaInputController.active
        return KeyState(mode: Modes.current, profile: Modes.profile, previous: Modes.previous,
                        menuRows: field?.last.menu.rows.count ?? 0, staged: field?.last.staged ?? "",
                        capsLock: CapsLock.isOn(NSEvent.modifierFlags), away: !Modes.isOurs)
    }

    /// The pairs a key pressed now waits for (§9.3): bound, with a binding that holds now, and with
    /// both keys' presses and releases seen: the tap's for 英数, かな and 半角/全角, the HID
    /// reader's for the rest.
    private static func pairs(_ state: KeyState) -> Set<Chord> {
        func seen(_ code: String) -> Bool { hidKeys.contains(code) ? HIDKeys.isListening : LanguageKeyTap.isOn }
        return Bada.table.pairs(holding: state).filter {
            if case .pair(let a, let b) = $0 { return seen(a) && seen(b) } else { return false }
        }
    }

    /// A key went down, to the sequencer: which keys run now, late ones first.
    private static func down(_ code: String, mods: Mods = [], isRepeat: Bool = false, mayWait: Bool = true) -> KeySequencer.Down {
        sequencer.pairs = pairs(state())
        return sequencer.down(code, mods: mods, isRepeat: isRepeat, mayWait: mayWait)
    }

    /// The table or the permissions changed: with no pair left to wait for, nothing waits.
    static func tableChanged() {
        if Bada.table.pairs.isEmpty { sequencer.reset() }
    }

    /// The tap went off and on again: releases may have been missed.
    static func forget() {
        sequencer.reset()
        undecided = nil
        echoes = []
        kept = []
    }

    // MARK: From the key tap

    /// A key event from the tap. Returns true to keep it from the app and from macOS.
    static func tapped(keyCode: Int, down isDown: Bool, mods: Mods, isRepeat: Bool) -> Bool {
        let code = keyName(keyCode)
        if !isDown {
            settle()
            if echoes.remove(keyCode) != nil { return true }
            let wasKept = kept.remove(keyCode) != nil
            for press in sequencer.up(code) { run(press) }
            return wasKept
        }
        // The repeats of a held key are not presses: one whose press was kept is kept, and one
        // that has a binding is swallowed, for a binding runs once for each press (§9.1).
        if isRepeat && (kept.contains(keyCode) || sequencer.holds(code)) { return true }
        // 英数 and かな are macOS's own keys for switching input source: bada takes them wherever one
        // of its modes is among the user's input sources. Every other key is the table's only while
        // bada is the selected input source.
        if code == "Lang1" || code == "Lang2" {
            guard Modes.isAdded else { return false }
            if isRepeat { return Modes.isOurs && Bada.table.binding(for: .key(code, mods), state: state()) != nil }
            if case .hid(let hidCode, _) = undecided {
                // The HID report came first: this event is that key's.
                undecided = nil
                echoes.insert(keyCode)
                return pressed(hidCode, mods: mods) ?? true
            }
            settle()
            if HIDKeys.isListening && !Bada.table.boundKeys.isDisjoint(with: hidKeys) {
                // It may be カタカナ: kept, and decided once the HID report has had its time.
                let at = Date()
                undecided = .event(code: code, keyCode: keyCode, mods: mods, at: at)
                kept.insert(keyCode)
                DispatchQueue.main.asyncAfter(deadline: .now() + eventWindow) {
                    if case .event(_, _, _, let since) = undecided, since == at { settle() }
                }
                return true
            }
            let used = pressed(code, mods: mods) ?? true
            if used { kept.insert(keyCode) }
            return used
        }
        settle()
        let ours = Modes.isOurs
        let result = down(code, mods: mods, isRepeat: isRepeat, mayWait: ours)
        for press in result.late { run(press) }
        guard ours else { return false }
        guard let press = result.now else {
            // A key of a bound pair (半角/全角): it runs when it is released.
            kept.insert(keyCode)
            wait()
            return true
        }
        let state = state()
        // No binding: the key isn't the table's, and goes on to the input method.
        guard Bada.table.binding(for: press, state: state) != nil else { return false }
        if isRepeat { return true }
        let used = perform(Bada.table.decide(press, state: state, profiles: Bada.profiles))
        if used { kept.insert(keyCode) }
        return used
    }

    /// A modifier went down, from the tap. To a key that waits, or a pair that is forming, it is
    /// another key (§9.3). It is no press of its own: nothing is decided for it here.
    static func modifierPressed(_ code: String) {
        settle()
        for press in down(code, mayWait: false).late { run(press) }
    }

    /// A modifier pressed and released alone, from the tap (§9.3): bound by its code, `AltRight`.
    static func modifierTapped(_ code: String) {
        guard Modes.isOurs else { return }
        settle()
        let press = Chord.key(code, [])
        let state = state()
        guard Bada.table.binding(for: press, state: state) != nil else { return }
        perform(Bada.table.decide(press, state: state, profiles: Bada.profiles))
    }

    // MARK: From the HID reader

    /// One of the PC boards' language keys, read at the HID level: 無変換, 変換, カタカナ.
    static func hid(_ code: String, down isDown: Bool) {
        guard Modes.isAdded, Bada.table.boundKeys.contains(code) else { return }
        if !isDown {
            settle()
            for press in sequencer.up(code) { run(press) }
            return
        }
        if case .event(_, let keyCode, let mods, _) = undecided {
            // The key event came first, and was this key's.
            undecided = nil
            echoes.insert(keyCode)
            kept.remove(keyCode)
            _ = pressed(code, mods: mods)
            return
        }
        settle()
        let at = Date()
        undecided = .hid(code: code, at: at)
        DispatchQueue.main.asyncAfter(deadline: .now() + hidWindow) {
            if case .hid(_, let since) = undecided, since == at { settle() }
        }
    }

    /// Caps Lock went down (CapsLock reads it at the HID level): another key, to the keys that wait.
    static func otherKeyPressed(_ code: String) {
        settle()
        for press in down(code, mayWait: false).late { run(press) }
    }

    // MARK: From the input method

    /// A language key that reached the input method: the tap is off, or it let the key through.
    /// Returns true when the key is used up.
    static func reached(_ code: String, mods: Mods) -> Bool {
        var code = code
        if (code == "Lang1" || code == "Lang2"), case .hid(let hidCode, _) = undecided {
            undecided = nil
            code = hidCode   // カタカナ, as かな
        } else {
            settle()
        }
        // Without the tap macOS switches the mode on 英数 and かな before the key gets here.
        let undone = Modes.undoAdoption()
        let used = pressed(code, mods: mods) ?? true
        if undone { Modes.reconcile() }
        return used
    }

    // MARK: Running

    /// The key whose press was undecided is what it seemed: a かな that was no カタカナ, a HID key no
    /// key event came for.
    private static func settle() {
        guard let waiting = undecided else { return }
        undecided = nil
        switch waiting {
        case .event(let code, _, let mods, _):
            let result = down(code, mods: mods)
            for press in result.late { run(press) }
            if let press = result.now { run(press) } else { wait() }
        case .hid(let code, _):
            _ = pressed(code, mods: Mods(NSEvent.modifierFlags))
        }
    }

    /// A language key went down. Returns whether its press is used up; nil when it waits for its
    /// release (§9.3).
    private static func pressed(_ code: String, mods: Mods) -> Bool? {
        let result = down(code, mods: mods)
        for press in result.late { run(press) }
        guard let press = result.now else {
            wait()
            return nil
        }
        return perform(decide(press))
    }

    /// A press that runs after its key event is gone: a key that waited, a pair, a key decided late.
    /// If the table has no use for the key after all (no binding of its own, or `pass`), it goes to
    /// the OS then (§9.3): a key that came as a key event is posted again. A HID key the OS had all along.
    private static func run(_ press: Chord) {
        let used = perform(decide(press))
        if !used, case .key(let code, _) = press, tapKeys.contains(code), let keyCode = keyCode(named: code) {
            LanguageKeyTap.post(keyCode)
        }
    }

    /// While another input source is selected, only a `select` is bada's business: anything else
    /// leaves the key to macOS.
    private static func decide(_ press: Chord) -> Decision {
        let state = state()
        let decision = Bada.table.decide(press, state: state, profiles: Bada.profiles)
        if state.away, case .select = decision.then { return decision }
        return state.away ? Decision(then: .pass) : decision
    }

    /// Does what the table decided. Returns true when the key is used up, false when the app or the
    /// OS should have it (`pass`), or the engine (`key`, `type`: the input method's own business).
    @discardableResult
    static func perform(_ decision: Decision) -> Bool {
        switch decision.then {
        case .select(let mode, let profile):
            Modes.go(to: mode, profile: profile)
            return true
        case .action(let name):
            BadaInputController.active?.action(name)
            return true
        case .onboarding:
            OnboardingWindow.shared.show()
            return true
        case .swallow:
            return true
        case .pass, .key, .type:
            return false
        }
    }

    /// A key began to wait for its release. If the release never comes (the tap was off for a
    /// moment), the key runs alone after a while rather than at some later key.
    private static func wait() {
        let since = Date()
        waitingSince = since
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            guard waitingSince == since, sequencer.isWaiting else { return }
            for press in sequencer.expire() { run(press) }
        }
    }
}
