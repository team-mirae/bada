import Cocoa
import IOKit.hid
import IOKit.hidsystem

/// Caps Lock (the standard, §9.4), without remapping the key: every press is read at the HID level
/// (`HIDKeys`, Input Monitoring), the key table decides what it does, bada sets the lock, and while
/// the key is down (and a moment after) the toggle macOS makes on its own is undone. The key tap
/// (Accessibility) swallows the Caps Lock events that disagree with bada, so apps never see macOS's
/// own toggle. It needs both; without them Caps Lock is macOS's.
///
/// - Bound in the key table (to `mode.toggle`, say), the key isn't the lock: its binding runs and
///   the lock doesn't change. ⇧, ⌃, ⌥ or ⌘ with Caps Lock are other chords, with bindings of their
///   own or none.
/// - Unbound, it is the lock. `caps_lock: respect`: everywhere. `transient`: in muhenkan only,
///   turned off when muhenkan is left; pressed in henkan it does nothing, and if another program
///   turns it on in henkan, bada turns it off.
enum CapsLock {
    static var setting: KeyTable.CapsLockSetting { Bada.table.capsLock }
    static var rule: KeyTable.CapsRule { Bada.table.capsRule }

    /// Handled: the key tap is on and Input Monitoring is granted (HIDKeys is listening).
    static var isHandled: Bool { LanguageKeyTap.isOn && HIDKeys.isListening }

    /// The lock as bada holds it.
    private static var on = systemState
    /// The press being handled is macOS's own (another input source): left alone.
    private static var native = false

    /// The lock is on, for what bada types (§6.4): as bada holds it when it handles Caps Lock
    /// (macOS's key events don't carry that), else as the key event says.
    static func isOn(_ flags: NSEvent.ModifierFlags) -> Bool {
        isHandled ? on : flags.contains(.capsLock)
    }

    /// Until when a Caps Lock change that disagrees with `on` is macOS's own toggle, to undo.
    private static var enforceUntil = Date.distantPast
    private static var keyDown = false

    /// macOS's Caps Lock state, as IOHIDSystem has it (CGEventSource's flags can lag behind it, as
    /// with Karabiner-Elements' virtual keyboard).
    static var systemState: Bool {
        var state = false
        withSystem { IOHIDGetModifierLockState($0, Int32(kIOHIDCapsLockState), &state) }
        return state
    }

    private static func withSystem(_ body: (io_connect_t) -> Void) {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass))
        guard service != 0 else { return }
        defer { IOObjectRelease(service) }
        var connect: io_connect_t = 0
        guard IOServiceOpen(service, mach_task_self_, UInt32(kIOHIDParamConnectType), &connect) == KERN_SUCCESS else { return }
        defer { IOServiceClose(connect) }
        body(connect)
    }

    /// Sets macOS's Caps Lock state (and so its LED).
    private static func setSystem(_ value: Bool) {
        DiagnosticLog.note("Caps Lock: set system \(value)")
        withSystem { IOHIDSetModifierLockState($0, Int32(kIOHIDCapsLockState), value) }
    }

    /// Holds the lock at `value`: set, and the LEDs set directly too, even when macOS already reads
    /// it so (a keyboard's LED can lag behind, as through Karabiner-Elements).
    private static func hold(_ value: Bool) {
        on = value
        setSystem(value)
        HIDKeys.setCapsLED(value)
        StatePublisher.refresh()
    }

    /// The physical key, from HIDKeys: down or up. `on` is still what it was before the press: a
    /// change macOS reports first waits for this (`changed`).
    static func key(down: Bool) {
        guard isHandled else { return }
        DiagnosticLog.note("Caps Lock: key \(down ? "down" : "up"), held \(on), system \(systemState)")
        if native {
            if !down { native = false }
            return
        }
        if !down {
            keyDown = false
            enforceUntil = Date().addingTimeInterval(0.5)
            if systemState != on { setSystem(on) }
            return
        }
        keyDown = true
        enforceUntil = .distantFuture
        // Another input source (ABC in a password field): Caps Lock is macOS's own toggle, untouched.
        guard Modes.selectedNow() != nil else {
            keyDown = false
            enforceUntil = .distantPast
            native = true
            on.toggle()
            StatePublisher.refresh()
            return
        }
        // A key that waits for its release (§9.3) runs first: Caps Lock is another key to it.
        KeyRouter.otherKeyPressed(KeyTable.capsLockKey)
        let mods = Mods(CGEventSource.flagsState(.combinedSessionState))
        let decision = Bada.table.decide(.key(KeyTable.capsLockKey, mods), state: KeyRouter.state(), profiles: Bada.profiles)
        guard decision.binding != nil else {
            // Unbound, the key is the lock: the table says what the lock is afterwards.
            hold(decision.capsLock ?? on)
            return
        }
        if decision.then == .pass {
            // Bound to `pass`: the OS has the key, and to the OS it is the lock.
            hold(!on)
            return
        }
        // Bound, it isn't the lock: its binding runs and the lock doesn't change.
        hold(on)
        KeyRouter.perform(decision)
        if case .select = decision.then { blink() }
    }

    /// From the key tap: a Caps Lock change macOS reports. Returns false to swallow it (macOS's own
    /// toggle, undone).
    static func changed(to value: Bool) -> Bool {
        guard isHandled else { return true }
        if value == on { return true }
        DiagnosticLog.note("Caps Lock: macOS says \(value), held \(on)\(keyDown || Date() < enforceUntil ? " (the key: undone)" : "")")
        if keyDown || Date() < enforceUntil {
            DispatchQueue.main.async { if systemState != on { setSystem(on) } }
            return false
        }
        // macOS often reports its own toggle before the key's HID report arrives: wait for it before
        // taking the change as another program's. Swallowed meanwhile.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            if keyDown || Date() < enforceUntil {
                if systemState != on { setSystem(on) }
                return
            }
            // Not the key: another program. In henkan, off unless the lock is respected.
            if setting == .transient && Modes.selectedNow() == .henkan {
                DiagnosticLog.note("Caps Lock: another program turned it on in henkan: off")
                hold(false)
            } else {
                on = value
                StatePublisher.refresh()
            }
        }
        return false
    }

    /// The mode changed: `transient` turns the lock off when muhenkan is left.
    static func modeChanged(to mode: Mode) {
        guard isHandled, setting == .transient, mode != .muhenkan, on else { return }
        hold(false)
    }

    /// The Caps Lock LED lit for a moment, set directly on each keyboard: the key switched mode.
    static func blink() {
        HIDKeys.setCapsLED(true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { HIDKeys.setCapsLED(on) }
    }

    /// When bada starts handling it: in sync with macOS, and off in henkan if the lock is transient.
    static func start() {
        on = systemState
        if setting == .transient, Modes.selectedNow() == .henkan, on { hold(false) }
        if UserDefaults(suiteName: "com.apple.HIToolbox")?.integer(forKey: "TISRomanSwitchState") == 1 {
            DiagnosticLog.note("Caps Lock: macOS's \"Use Caps Lock to switch to and from ABC\" is on; turn it off for bada's Caps Lock")
        }
    }
}
