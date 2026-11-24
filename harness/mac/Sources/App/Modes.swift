import Carbon
import Cocoa
import InputMethodKit

extension Mode {
    /// The mode's input source (the standard, §12): bada has exactly two, one for each mode.
    var sourceID: String { Bada.bundleID + (self == .muhenkan ? ".Muhenkan" : ".Henkan") }
}

/// The mode and the henkan profile: the harness's own (the standard, §3.1). The key table decides
/// them, each text field hears them with `select`, and macOS's input source follows.
enum Modes {
    /// The mode bada is in, as the key table or macOS last decided.
    private(set) static var current = Mode.muhenkan
    /// The profile last selected, by its id; nil before the first.
    private(set) static var profile: String?
    /// The profile in use before that one: what `profile.last` goes back to.
    private(set) static var previous: String?
    /// Goes up whenever the text fields must hear `select` again.
    private(set) static var generation = 0

    private static var ours: Bool?
    private static var enabled: Bool?
    private static var watching = false
    /// Where things were before macOS switched the mode by itself, and when.
    private static var adopted: (mode: Mode, profile: String?, previous: String?, at: Date)?

    static func mode(sourceID: String) -> Mode? {
        [Mode.muhenkan, .henkan].first { $0.sourceID == sourceID }
    }

    /// Which of bada's modes macOS has selected now; nil: another input source.
    static func selectedNow() -> Mode? {
        let source = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
        guard let value = TISGetInputSourceProperty(source, kTISPropertyInputSourceID) else { return nil }
        let mode = mode(sourceID: Unmanaged<CFString>.fromOpaque(value).takeUnretainedValue() as String)
        ours = mode != nil
        return mode
    }

    static func isEnabled(_ mode: Mode) -> Bool {
        let query = [kTISPropertyInputSourceID as String: mode.sourceID] as CFDictionary
        return (TISCreateInputSourceList(query, false)?.takeRetainedValue() as? [TISInputSource])?.isEmpty == false
    }

    /// One of bada's modes is the selected input source. Kept between changes: the key tap asks on
    /// every key.
    static var isOurs: Bool {
        if let ours { return ours }
        return selectedNow() != nil
    }

    /// One of bada's modes is among the user's input sources. Kept between changes, as `isOurs`.
    static var isAdded: Bool {
        if let enabled { return enabled }
        let value = isEnabled(.henkan) || isEnabled(.muhenkan)
        enabled = value
        return value
    }

    /// At launch: the mode macOS has selected, and the profile the last run was in.
    static func start() {
        watch()
        // The profile is kept across runs as its id (§8.1), in the state bada publishes itself.
        if let data = try? Data(contentsOf: StatePublisher.url), let state = JSON.object(data) {
            if let id = state["profile"] as? String, has(id) { profile = id }
        }
        if selectedNow() == .henkan, let id = Bada.table.entering(last: profile, profiles: Bada.profiles) {
            profile = id
            current = .henkan
        }
        KeyRouter.tableChanged()
    }

    private static func has(_ id: String) -> Bool { Bada.profiles.contains { $0.id == id } }

    private static func watch() {
        guard !watching else { return }
        watching = true
        let center = DistributedNotificationCenter.default()
        center.addObserver(forName: Notification.Name(kTISNotifySelectedKeyboardInputSourceChanged as String), object: nil, queue: .main) { _ in
            ours = nil
            StatePublisher.refresh()
        }
        center.addObserver(forName: Notification.Name(kTISNotifyEnabledKeyboardInputSourcesChanged as String), object: nil, queue: .main) { _ in
            enabled = nil
            ours = nil
        }
    }

    /// The state itself. A `select` that leaves muhenkan, or changes the profile, makes the profile
    /// the one selected, and the one before it is what `profile.last` goes back to (§9.2).
    private static func set(_ mode: Mode, profile id: String?) {
        let before = current
        if mode == .henkan, let id, id != profile {
            if let last = profile { previous = last }
            profile = id
        }
        current = mode
        generation += 1
        if before != mode { CapsLock.modeChanged(to: mode) }
    }

    /// A `select` of the key table (§9.2): the field in use hears it at once, so what was staged is
    /// committed before the next key; then macOS's input source follows if the mode changed.
    static func go(to mode: Mode, profile id: String?) {
        adopted = nil
        set(mode, profile: id)
        BadaInputController.active?.sync()
        if selectedNow() != mode {
            LanguageKeys.switchSource(to: mode)
        } else {
            BadaInputController.active?.showKeyboard()
        }
        StatePublisher.refresh()
    }

    /// macOS selected one of bada's modes by itself: the input menu, a system shortcut, or 英数 and
    /// かな while the key tap is off. `byUser`: it came as a change, not as what was found at focus.
    static func adopt(_ mode: Mode, byUser: Bool) {
        guard mode != current else { return }
        if mode == .henkan {
            guard let id = Bada.table.entering(last: profile, profiles: Bada.profiles) else {
                // No profile yet: henkan can't be entered (§8.3). Back to muhenkan, and the onboarding.
                DiagnosticLog.note("henkan was selected with no profile")
                if byUser { OnboardingWindow.shared.show() }
                DispatchQueue.main.async { LanguageKeys.direct(.muhenkan) }
                return
            }
            adopted = (current, profile, previous, Date())
            set(.henkan, profile: id)
        } else {
            adopted = (current, profile, previous, Date())
            set(.muhenkan, profile: nil)
        }
    }

    /// A language key reached the input method just after macOS switched the mode on that very key
    /// (the key tap is off): the key table decides from where things were before. Returns whether
    /// the state was put back, so the caller makes macOS's input source follow afterwards.
    static func undoAdoption() -> Bool {
        guard let before = adopted, Date().timeIntervalSince(before.at) < 0.5 else { return false }
        adopted = nil
        let mode = current
        current = before.mode
        profile = before.profile
        previous = before.previous
        generation += 1
        if mode != current { CapsLock.modeChanged(to: current) }
        return true
    }

    /// After `undoAdoption`: macOS's input source, and the field in use, follow the state.
    static func reconcile() {
        BadaInputController.active?.sync()
        if let selected = selectedNow(), selected != current { LanguageKeys.switchSource(to: current) }
        StatePublisher.refresh()
    }

    /// The settings were read again: a profile that is gone can't stay in use (§8.2), and every
    /// field hears `select` again.
    static func settingsChanged() {
        if let id = profile, !has(id) { profile = nil }
        if let id = previous, !has(id) { previous = nil }
        if current == .henkan && profile == nil {
            if let id = Bada.table.entering(last: nil, profiles: Bada.profiles) {
                profile = id
            } else {
                current = .muhenkan
                CapsLock.modeChanged(to: .muhenkan)
                DispatchQueue.main.async { LanguageKeys.direct(.muhenkan) }
            }
        }
        generation += 1
        KeyRouter.tableChanged()
        BadaInputController.active?.sync()
        BadaInputController.active?.showKeyboard()
        LanguageKeyTap.check()
        StatePublisher.refresh()
    }

    /// Selects a mode's input source. With one of ours selected and a text field, the field's
    /// session switches (IMK's own way; TIS from the input method's process can cut the app's
    /// session off).
    static func selectSource(_ mode: Mode, in client: IMKTextInput? = nil) {
        if let client, selectedNow() != nil {
            client.selectMode(mode.sourceID)
            return
        }
        let query = [kTISPropertyInputSourceID as String: mode.sourceID] as CFDictionary
        if let source = (TISCreateInputSourceList(query, false)?.takeRetainedValue() as? [TISInputSource])?.first {
            TISSelectInputSource(source)
        } else {
            DiagnosticLog.note("selecting \(mode.rawValue) failed: not added in System Settings")
        }
    }
}

/// Switching macOS's input source to a mode. The key tap never selects an input source itself: it
/// hands macOS a plain 英数 or かな, and macOS's own switch does it. If macOS hasn't switched in a
/// moment, bada selects the mode itself.
enum LanguageKeys {
    /// A switch handed to macOS, waiting for that mode to be selected.
    private static var pending: (mode: Mode, at: Date)?
    /// How long macOS gets to switch before bada selects the mode itself.
    private static let fallback = 0.3

    /// A switch is on its way: what macOS has selected now isn't news.
    static var isPending: Bool {
        guard let pending else { return false }
        return Date().timeIntervalSince(pending.at) < 1
    }

    static func switchSource(to mode: Mode) {
        guard Modes.isEnabled(mode) else { return }
        guard LanguageKeyTap.isOn, Modes.selectedNow() != mode else {
            direct(mode)
            return
        }
        let at = Date()
        pending = (mode, at)
        LanguageKeyTap.post(mode == .muhenkan ? kVK_JIS_Eisu : kVK_JIS_Kana)
        DispatchQueue.main.asyncAfter(deadline: .now() + fallback) {
            guard let waiting = pending, waiting.at == at else { return }
            pending = nil
            if Modes.selectedNow() == mode {
                DiagnosticLog.note("switched to \(mode.rawValue) without a text field")
            } else {
                DiagnosticLog.note("macOS didn't switch to \(mode.rawValue); selecting it directly")
                direct(mode)
            }
        }
    }

    /// Called when macOS has selected `mode` for a text field: whether bada itself asked for it.
    static func confirm(_ mode: Mode) -> Bool {
        guard let waiting = pending, waiting.mode == mode, Date().timeIntervalSince(waiting.at) < 1 else { return false }
        pending = nil
        return true
    }

    /// bada selecting the mode itself: without the tap, or as the fallback.
    static func direct(_ mode: Mode) {
        if Modes.selectedNow() != nil, let controller = BadaInputController.typing {
            controller.selectSource(mode)
        } else {
            Modes.selectSource(mode)
        }
    }
}
