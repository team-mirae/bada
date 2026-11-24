import Carbon.HIToolbox
import Cocoa
import InputMethodKit

/// One controller per text input client; each owns an engine: one text field (the standard, §3.1).
/// The engine turns keys into text. This class asks the key table first (§9), feeds the engine what
/// the table doesn't take, applies each reply to the app's text field (§6.3) and shows the menu.
@objc(BadaInputController)
final class BadaInputController: IMKInputController, TextTarget {
    /// nil when the engine was refused (another `abi.major`): every key then goes to the app.
    private let engine: Engine? = Bada.session.flatMap { Engine($0) }
    private var marked = false
    /// The last reply to this field's events: what shows, and what the key table decides from.
    private(set) var last = Reply()
    /// `Modes.generation` when this engine last heard `select`.
    private var selectedAt = -1
    private var isActive = false
    /// The client a reply is being applied to.
    private var target: IMKTextInput?

    /// The controller that got the last key, else the last activated one.
    private(set) static weak var focused: BadaInputController?
    private(set) static weak var keyed: BadaInputController?
    static var typing: BadaInputController? { keyed ?? focused }
    /// The controller of the text field in use now; nil when none has the keyboard.
    static var active: BadaInputController? {
        guard let controller = typing, controller.isActive else { return nil }
        return controller
    }

    private static let notFound = NSRange(location: NSNotFound, length: NSNotFound)
    /// The underline of a fixed segment: a plain gray, which reads the same in every app.
    private static let fixedColor = NSColor(srgbRed: 0.6, green: 0.6, blue: 0.6, alpha: 1)

    override func recognizedEvents(_ sender: Any!) -> Int {
        Int(NSEvent.EventTypeMask.keyDown.rawValue)
    }

    // MARK: Keys

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event, event.type == .keyDown, let client = sender as? IMKTextInput else { return false }
        Self.keyed = self
        isActive = true
        guard let engine else { return false }
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let mods = Mods(flags)
        let keyCode = Int(event.keyCode)
        let code = keyName(keyCode)
        // A key bada posted itself: 英数 or かな for macOS's switch (LanguageKeys), which is no press of
        // the user's; or a key that waited and turned out to be the OS's (§9.3), which the app gets.
        if LanguageKeyTap.wasPosted(keyCode) {
            return keyCode == kVK_JIS_Eisu || keyCode == kVK_JIS_Kana
        }
        sync(client)

        // Language keys never reach the engine (§4): they are the key table's, or the OS's.
        if KeyTable.isLanguageKey(code) {
            if event.isARepeat { return Bada.table.binding(for: .key(code, mods), state: KeyRouter.state()) != nil }
            return KeyRouter.reached(code, mods: mods)
        }
        // A lock pressed by itself is no key press (§5): the Mac's Clear key is Num Lock by its code.
        if code == "NumLock" { return false }

        // Any other key: the table's if it has a binding for it and can see chords now, else the engine's.
        let decision = Bada.table.decide(.key(code, mods), state: KeyRouter.state(), profiles: Bada.profiles, chords: LanguageKeyTap.isOn)
        // A binding runs once for each press: the repeats of a held key that has one are swallowed (§9.1).
        if decision.binding != nil && event.isARepeat { return true }
        switch decision.then {
        case .key:
            break
        case .type(let text):
            // Caps Lock's upper_snake rule (§9.4): the harness flushes and types, without sending the key.
            apply(engine.flush(), to: client)
            client.insertText(text, replacementRange: Self.notFound)
            return true
        case .pass:
            return false
        default:
            return KeyRouter.perform(decision)
        }

        // In a terminal, ⌥ in muhenkan goes to the app untouched (⌥ as Meta: readline's ⌥B, ⌥F).
        if mods.contains(.alt) && Modes.current == .muhenkan && !marked && Self.terminals.contains(client.bundleIdentifier() ?? "") {
            return false
        }
        let reply = engine.key(code, mods: mods, isRepeat: event.isARepeat)
        apply(reply, to: client)
        // Refused: nothing changed, and the key is swallowed (§6.2, §6.3).
        guard reply.ok else { return true }
        if reply.consumed { return true }
        // A key the engine didn't consume goes to the app. If the engine says what it types (§6.4),
        // macOS types it when its own layout gives the same text; else the text is inserted here.
        switch Typing.resolve(types: reply.types, osTypes: event.characters, capsLock: CapsLock.isOn(flags)) {
        case .app:
            return false
        case .insert(let text):
            client.insertText(text, replacementRange: Self.notFound)
            return true
        }
    }

    private static let terminals: Set<String> = [
        "com.apple.Terminal", "com.googlecode.iterm2", "net.kovidgoyal.kitty", "com.github.wez.wezterm",
        "io.alacritty", "org.alacritty", "dev.warp.Warp-Stable", "com.mitchellh.ghostty",
    ]

    /// An action of the key table (§9.2): `menu.next`, `fix`, `width.full`, a layout's own.
    func action(_ name: String) {
        guard let engine, let client = client() as IMKTextInput? else { return }
        sync(client)
        apply(engine.action(name), to: client)
    }

    /// Tells the engine the mode and the profile, if they changed since it last heard (§3.1): after
    /// `bada_new`, and when they changed while another field had focus. What was staged is committed.
    func sync(_ given: IMKTextInput? = nil) {
        guard selectedAt != Modes.generation, let engine else { return }
        guard let client = given ?? (client() as IMKTextInput?) else { return }
        selectedAt = Modes.generation
        let reply = engine.select(Modes.current, profile: Modes.profile)
        if !reply.ok { DiagnosticLog.note("select \(Modes.current.rawValue) \(Modes.profile ?? "null") was refused: \(reply.error?.code ?? "?")") }
        apply(reply, to: client)
    }

    // MARK: Focus and mode

    override func activateServer(_ sender: Any!) {
        super.activateServer(sender)
        Self.focused = self
        isActive = true
        let client = sender as? IMKTextInput
        // The mode macOS has selected, unless a switch bada asked for is still on its way.
        if let mode = Modes.selectedNow(), mode != Modes.current, !LanguageKeys.isPending {
            Modes.adopt(mode, byUser: false)
        }
        if let client {
            sync(client)
            Keyboards.show(for: Modes.current, in: client)
        }
        publish(client)
        DispatchQueue.main.async { LanguageKeyTap.check() }
    }

    override func setValue(_ value: Any!, forTag tag: Int, client sender: Any!) {
        let client = sender as? IMKTextInput
        if tag == kTextServiceInputModePropertyTag, let id = value as? String, let mode = Modes.mode(sourceID: id) {
            // macOS selected a mode: the one bada asked for, or one it chose itself.
            if !LanguageKeys.confirm(mode) { Modes.adopt(mode, byUser: true) }
            if let client { sync(client) }
        }
        super.setValue(value, forTag: tag, client: sender)
        // After super: selecting the mode puts its own keyboard in place.
        if tag == kTextServiceInputModePropertyTag, let client {
            Keyboards.show(for: Modes.current, in: client)
            publish(client)
        }
    }

    override func deactivateServer(_ sender: Any!) {
        flush(sender, cause: "blur")
        isActive = false
        SuggestionWindow.shared.hide()
        super.deactivateServer(sender)
    }

    /// The app wants what is being composed committed: a click in the text, or an edit of its own.
    override func commitComposition(_ sender: Any!) {
        flush(sender, cause: "harness")
    }

    private func flush(_ sender: Any!, cause: String) {
        guard let engine, let client = (sender as? IMKTextInput) ?? (client() as IMKTextInput?) else { return }
        apply(engine.flush(cause: cause), to: client)
    }

    /// Selects a mode's input source through this field's session.
    func selectSource(_ mode: Mode) {
        guard let client = client() as IMKTextInput? else { return Modes.selectSource(mode) }
        Keyboards.show(for: mode, in: client)
        DispatchQueue.main.async { Modes.selectSource(mode, in: client) }
    }

    /// The keyboard of the layout in use, for the Keyboard Viewer.
    func showKeyboard() {
        guard let client = client() as IMKTextInput? else { return }
        Keyboards.show(for: Modes.current, in: client)
    }

    // MARK: Replies

    /// Applies a reply as the standard's §6.3 has it, and remembers what shows.
    private func apply(_ reply: Reply, to client: IMKTextInput) {
        target = client
        defer { target = nil }
        var final = reply
        ReplyApplier.apply(reply, to: self) { [engine] event in
            // The engine asked for text (`ask`): answered once, with one `text` event.
            let answer = engine?.send(event) ?? Reply.none
            if answer.ok { final = answer }
            return answer
        }
        if final.ok {
            last = final
        } else if final.error?.code == "panic" {
            last = Reply()   // the engine has reset itself: nothing is staged
        }
        publish(client)
    }

    var hasMarked: Bool { marked }

    func commit(_ text: String, replacingSelection: Bool) {
        guard let target else { return }
        target.insertText(text, replacementRange: replacingSelection ? target.selectedRange() : Self.notFound)
        marked = false
    }

    /// The marked text, with its parts drawn apart: what is still being read has a plain underline,
    /// what is fixed a gray one, and the row the user moved to a thick one.
    func mark(_ preedit: Preedit) {
        guard let target else { return }
        let text = NSMutableAttributedString(string: preedit.text)
        for (index, run) in preedit.runs.enumerated() {
            var attributes: [NSAttributedString.Key: Any] = [.markedClauseSegment: index]
            switch run.kind {
            case .live:
                attributes[.underlineStyle] = NSUnderlineStyle.single.rawValue
            case .fixed:
                attributes[.underlineStyle] = NSUnderlineStyle.single.rawValue
                attributes[.underlineColor] = Self.fixedColor
            case .picked:
                attributes[.underlineStyle] = NSUnderlineStyle.thick.rawValue
            }
            text.addAttributes(attributes, range: run.range)
        }
        let caret = NSRange(location: preedit.utf16Offset(scalars: preedit.caret), length: 0)
        target.setMarkedText(text, selectionRange: caret, replacementRange: Self.notFound)
        marked = true
    }

    func unmark() {
        guard let target else { return }
        target.setMarkedText("", selectionRange: NSRange(location: 0, length: 0), replacementRange: Self.notFound)
        marked = false
    }

    func selection() -> String {
        guard let target, !marked else { return "" }
        let range = target.selectedRange()
        guard range.location != NSNotFound, range.length > 0 else { return "" }
        var actual = NSRange(location: NSNotFound, length: 0)
        return target.string(from: range, actualRange: &actual) ?? ""
    }

    /// The text around the caret, at most `before` and `after` characters (Unicode scalar values).
    /// With text marked or selected: around that text (§5).
    func around(before: Int, after: Int) -> (before: String, after: String) {
        guard let target else { return ("", "") }
        var range = target.selectedRange()
        if marked {
            let whole = target.markedRange()
            if whole.location != NSNotFound { range = whole }
        }
        guard range.location != NSNotFound else { return ("", "") }
        var actual = NSRange(location: NSNotFound, length: 0)
        // A scalar is at most two UTF-16 units: ask for twice as many, then cut to the count.
        let start = max(0, range.location - before * 2)
        let head = before > 0 && range.location > start
            ? target.string(from: NSRange(location: start, length: range.location - start), actualRange: &actual) ?? "" : ""
        let end = range.location + range.length
        let tail = after > 0 ? target.string(from: NSRange(location: end, length: after * 2), actualRange: &actual) ?? "" : ""
        return (String(String.UnicodeScalarView(head.unicodeScalars.suffix(before))),
                String(String.UnicodeScalarView(tail.unicodeScalars.prefix(after))))
    }

    private var menuGeneration = 0

    func show(_ menu: Menu, rtl: Bool) {
        menuGeneration += 1
        let generation = menuGeneration
        guard !menu.rows.isEmpty, let client = target else {
            SuggestionWindow.shared.hide()
            return
        }
        // The caret's rectangle is a round trip into the app: asked once the key is done.
        DispatchQueue.main.async { [weak self] in
            guard let self, generation == self.menuGeneration else { return }
            var caret = NSRect.zero
            _ = client.attributes(forCharacterIndex: 0, lineHeightRectangle: &caret)
            SuggestionWindow.shared.onPick = { [weak self] row in self?.pick(row) }
            SuggestionWindow.shared.show(menu, below: caret)
        }
    }

    /// A click on a row of the menu: `pick` moves to it, and the click commits it (§5).
    private func pick(_ row: Int) {
        guard let engine, isActive, let client = client() as IMKTextInput? else { return }
        let reply = engine.pick(row: row)
        apply(reply, to: client)
        if reply.ok { apply(engine.flush(), to: client) }
    }

    /// The state for other programs (§6.7): after every reply, and on mode, profile and focus changes.
    private func publish(_ client: IMKTextInput?) {
        StatePublisher.publish(last, app: client?.bundleIdentifier())
    }

    // MARK: The input menu

    override func menu() -> NSMenu! {
        let menu = NSMenu()
        // The henkan profiles, by name (§8.1): the one in use is checked.
        for profile in Bada.profiles {
            let item = NSMenuItem(title: profile.name, action: #selector(pickProfile(_:)), keyEquivalent: "")
            item.representedObject = profile.id
            item.state = Modes.current == .henkan && Modes.profile == profile.id ? .on : .off
            menu.addItem(item)
        }
        if Bada.profiles.isEmpty {
            menu.addItem(NSMenuItem(title: "Set Up bada…", action: #selector(setUp(_:)), keyEquivalent: ""))
        }
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Open Settings Folder", action: #selector(openSettingsFolder(_:)), keyEquivalent: ""))
        if !AXIsProcessTrusted() || !HIDKeys.isAllowed {
            menu.addItem(NSMenuItem(title: "Allow Language Keys Everywhere…", action: #selector(allowKeys(_:)), keyEquivalent: ""))
        }
        let problems = (Bada.store?.problems ?? []).filter(\.isError)
        if let refusal = Bada.refusal {
            menu.addItem(.separator())
            menu.addItem(NSMenuItem(title: "No engine: \(refusal)", action: nil, keyEquivalent: ""))
        } else if let first = problems.first {
            menu.addItem(.separator())
            let more = problems.count > 1 ? " (and \(problems.count - 1) more)" : ""
            menu.addItem(NSMenuItem(title: "\(first.file) wasn't used: \(first.message)\(more)", action: nil, keyEquivalent: ""))
        }
        return menu
    }

    /// The menu item an input-menu action was sent for: IMK passes a dictionary that holds it.
    private func item(_ sender: Any?) -> NSMenuItem? {
        (sender as? NSDictionary)?.object(forKey: kIMKCommandMenuItemName as Any) as? NSMenuItem ?? sender as? NSMenuItem
    }

    @objc func pickProfile(_ sender: Any?) {
        guard let id = item(sender)?.representedObject as? String, Bada.profiles.contains(where: { $0.id == id }) else { return }
        Modes.go(to: .henkan, profile: id)
    }

    @objc func setUp(_ sender: Any?) {
        OnboardingWindow.shared.show()
    }

    @objc func openSettingsFolder(_ sender: Any?) {
        try? FileManager.default.createDirectory(at: Bada.folder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Bada.folder)
    }

    /// Asks for Accessibility (the key tap) and Input Monitoring (the PC boards' keys and Caps Lock).
    @objc func allowKeys(_ sender: Any?) {
        LanguageKeyTap.start(ask: true)
        HIDKeys.start(ask: true)
    }
}
