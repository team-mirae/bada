import Carbon.HIToolbox
import Cocoa
import InputMethodKit

/// Keys caught system-wide, before any app sees them (the standard, §9.3). It needs Accessibility;
/// bada asks for it only from the input menu ("Allow Language Keys Everywhere…").
///
/// - 英数 and かな: macOS doesn't always pass かな to an input method that is already in a Japanese
///   mode, and some apps keep these keys. The tap takes them while one of bada's modes is among the
///   user's input sources.
/// - Any other chord the key table binds (`ctrl+alt+shift+meta+Space`, an F key, a modifier pressed
///   and released alone), and the releases that pairs of language keys need: while bada is the
///   selected input source.
/// - Caps Lock (§9.4): the changes macOS makes on its own are swallowed (CapsLock).
///
/// The tap never selects an input source itself. A key that changes mode is handed to macOS as a
/// plain 英数 or かな (`post`), so macOS's own switch does it (LanguageKeys).
enum LanguageKeyTap {
    private static var tap: CFMachPort?
    private static var polling = false
    static var isOn: Bool { tap != nil }
    /// Marks the 英数 / かな events bada posts, so the tap lets them through.
    private static let marker: Int64 = 0x6261_6432   // "bad2"
    /// The last key posted, and when: it may reach the input method as well as macOS's switch.
    private static var lastPosted: (keyCode: Int, at: Date)?
    /// The modifier keys that are down, by key code.
    private static var held: Set<Int> = []
    /// A modifier that went down with no other held, by its key code: released with no other key
    /// in between, it is a press of its own.
    private static var alone: Int?

    /// Whether a key event that reached the input method is one bada posted itself a moment ago.
    static func wasPosted(_ keyCode: Int) -> Bool {
        guard let last = lastPosted else { return false }
        return last.keyCode == keyCode && Date().timeIntervalSince(last.at) < 1
    }

    /// A plain key (no modifiers), for macOS: 英数 or かな to switch input source with, or a key
    /// that waited and turned out to be the OS's (§9.3).
    static func post(_ code: Int) {
        let source = CGEventSource(stateID: .privateState)
        source?.userData = marker
        for down in [true, false] {
            guard let event = CGEvent(keyboardEventSource: source, virtualKey: CGKeyCode(code), keyDown: down) else { continue }
            event.flags = []
            event.setIntegerValueField(.eventSourceUserData, value: marker)
            event.post(tap: .cgSessionEventTap)
        }
        lastPosted = (code, Date())
    }

    /// Turns on what macOS already allows, asking for nothing.
    static func check() {
        HIDKeys.check()
        if Modes.isAdded { start(ask: false) }
    }

    static func start(ask: Bool) {
        guard tap == nil else { return }
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: ask] as CFDictionary
        guard AXIsProcessTrustedWithOptions(options) else {
            if ask && !polling {
                polling = true
                poll()
            }
            return
        }
        let mask = (1 << CGEventType.keyDown.rawValue) | (1 << CGEventType.keyUp.rawValue) | (1 << CGEventType.flagsChanged.rawValue)
        guard let port = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap,
                                           eventsOfInterest: CGEventMask(mask), callback: callback, userInfo: nil)
        else {
            DiagnosticLog.note("key tap: couldn't create the tap")
            return
        }
        tap = port
        CFRunLoopAddSource(CFRunLoopGetMain(), CFMachPortCreateRunLoopSource(nil, port, 0), .commonModes)
        CGEvent.tapEnable(tap: port, enable: true)
        DiagnosticLog.note("key tap: on")
        HIDKeys.check()   // Caps Lock needs the HID keys too
        KeyRouter.tableChanged()
        StatePublisher.refresh()
    }

    private static func poll() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            guard tap == nil else { polling = false; return }
            if AXIsProcessTrusted() { polling = false; start(ask: false) } else { poll() }
        }
    }

    private static let callback: CGEventTapCallBack = { _, type, event, _ in
        let pass = Unmanaged.passUnretained(event)
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            KeyRouter.forget()   // releases may have been missed
            held = []
            alone = nil
            return pass
        }
        if event.getIntegerValueField(.eventSourceUserData) == marker { return pass }
        let keyCode = Int(event.getIntegerValueField(.keyboardEventKeycode))
        if type == .flagsChanged {
            if keyCode == kVK_CapsLock {
                return CapsLock.changed(to: event.flags.contains(.maskAlphaShift)) ? pass : nil
            }
            modifier(keyCode, flags: event.flags)
            return pass
        }
        guard type == .keyDown || type == .keyUp else { return pass }
        if type == .keyDown { alone = nil }
        let isRepeat = type == .keyDown && event.getIntegerValueField(.keyboardEventAutorepeat) != 0
        return KeyRouter.tapped(keyCode: keyCode, down: type == .keyDown, mods: Mods(event.flags), isRepeat: isRepeat) ? nil : pass
    }

    /// A modifier went down or up. Down, it is another key to a key that waits (§9.3). Pressed and
    /// released with no other key in between, it is a press the key table can bind by its code
    /// (`AltRight`), which runs on the release. The modifier itself is never kept from the app.
    private static func modifier(_ keyCode: Int, flags: CGEventFlags) {
        guard let code = modifierNames[keyCode] else { return }
        if held.remove(keyCode) != nil {
            // Up.
            if alone == keyCode {
                alone = nil
                KeyRouter.modifierTapped(code)
            }
            return
        }
        // Down. With the flags clear it wasn't: a release whose press was never seen.
        guard !Mods(flags).isEmpty else { return }
        alone = held.isEmpty ? keyCode : nil
        held.insert(keyCode)
        KeyRouter.modifierPressed(code)
    }
}
