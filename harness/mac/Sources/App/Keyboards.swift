import Carbon.HIToolbox
import InputMethodKit

/// The keyboard layout macOS uses while bada is selected, which is also what the Keyboard Viewer
/// shows. The app carries one for each stock layout that types (tools/make-keylayout.swift writes
/// them from the engine itself): the layout's own characters on the plain and ⇧ layers, and ABC's on
/// the others, so shortcuts are unchanged.
///
/// It is for show, and to let macOS type a key itself when it can: what a key types is always the
/// engine's (`types`, the standard §6.4), and the input method inserts the text when the keyboard
/// macOS has would type something else.
enum Keyboards {
    static func sourceID(_ layout: String) -> String { Bada.bundleID + ".keylayout." + layout }

    /// The layout whose keyboard shows: the muhenkan layout, or the first layout of the profile in
    /// use. One board can't show every layout of a profile.
    static func layout(for mode: Mode) -> String? {
        switch mode {
        case .muhenkan: return Bada.store?.muhenkanLayout
        case .henkan: return Bada.profiles.first { $0.id == Modes.profile }?.layouts.first
        }
    }

    /// The keyboard to set, by its input source id; nil: macOS's own keyboard stays. A layout with
    /// no keyboard of its own (a custom one, a search) shows the JIS legends, which are en-qwerty's.
    static func keyboard(for mode: Mode) -> String? {
        guard let layout = layout(for: mode) else { return nil }
        if Bundle.main.url(forResource: layout, withExtension: "keylayout") != nil { return sourceID(layout) }
        return sourceID("en-qwerty")
    }

    /// Sets the mode's keyboard, now and once macOS has finished selecting the mode (which may put
    /// the mode's own keyboard back). It is asked for by name, not looked up first: the input
    /// method's process may not see a layout macOS has just registered.
    static func show(for mode: Mode, in client: IMKTextInput) {
        guard let name = keyboard(for: mode) else { return }
        client.overrideKeyboard(withKeyboardNamed: name)
        for delay in [0.15, 0.6] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard Modes.current == mode, keyboard(for: mode) == name else { return }
                client.overrideKeyboard(withKeyboardNamed: name)
            }
        }
    }

    /// The key table's preset for the keyboard macOS reports (§9.5): a JIS board gets the Mac's JIS
    /// preset, any other the one for a keyboard with no language keys. A PC's JIS board is the
    /// harness setting `"keyboard": "jis-pc"`: macOS can't tell it from Apple's.
    static func preset() -> KeyTable.Preset {
        KBGetLayoutType(Int16(LMGetKbdType())) == kKeyboardJIS ? .jisMac : .noLanguageKeys
    }
}
