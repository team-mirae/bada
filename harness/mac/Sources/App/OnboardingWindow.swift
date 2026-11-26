import Cocoa

/// The first run (the standard, §8.3). A new install has no muhenkan layout and no henkan profile,
/// and a key that would enter henkan opens this instead. The easy mode: pick the muhenkan layout and
/// one henkan layout; that writes `settings.json`, and the key works from then on.
///
/// The full settings are the settings window's (SettingsWindow), from the input menu.
final class OnboardingWindow: NSObject, NSWindowDelegate {
    static let shared = OnboardingWindow()

    private var window: NSWindow?
    private var muhenkan = NSPopUpButton()
    private var henkan = NSPopUpButton()
    private var status = NSTextField(labelWithString: "")

    /// The languages layouts are named in, the user's first.
    private var languages: [String] { Locale.preferredLanguages + ["en"] }

    func show() {
        if window == nil { build() }
        fill()
        // bada is background-only (LSBackgroundOnly): such an app can't be active, so its window
        // would never take a click on a pop-up. While the window is open it is an accessory app:
        // active and key, still no Dock icon.
        NSApp.setActivationPolicy(.accessory)
        NSApp.activate(ignoringOtherApps: true)
        window?.center()
        window?.makeKeyAndOrderFront(nil)
    }

    private func build() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 250), styleMask: [.titled, .closable],
                              backing: .buffered, defer: false)
        window.title = "bada"
        window.isReleasedWhenClosed = false
        window.delegate = self

        let heading = NSTextField(labelWithString: "Set up bada")
        heading.font = .boldSystemFont(ofSize: NSFont.systemFontSize + 2)
        let intro = NSTextField(wrappingLabelWithString:
            "Pick the layout that types directly (muhenkan) and one layout that composes (henkan). "
            + "You can add more layouts and profiles later, in Settings.")
        intro.preferredMaxLayoutWidth = 420

        func row(_ title: String, _ control: NSView) -> NSStackView {
            let label = NSTextField(labelWithString: title)
            label.alignment = .right
            label.widthAnchor.constraint(equalToConstant: 110).isActive = true
            let row = NSStackView(views: [label, control])
            row.orientation = .horizontal
            row.spacing = 8
            return row
        }
        let start = NSButton(title: "Start", target: self, action: #selector(start(_:)))
        start.keyEquivalent = "\r"
        let folder = NSButton(title: "Open Settings Folder", target: self, action: #selector(openFolder(_:)))
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancel.keyEquivalent = "\u{1b}"
        let buttons = NSStackView(views: [folder, NSView(), cancel, start])
        buttons.orientation = .horizontal
        buttons.spacing = 8

        status.textColor = .systemRed
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3
        status.preferredMaxLayoutWidth = 420

        let stack = NSStackView(views: [heading, intro, row("Muhenkan:", muhenkan), row("Henkan:", henkan), status, buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        buttons.widthAnchor.constraint(equalToConstant: 420).isActive = true
        window.contentView = stack
        self.window = window
    }

    /// The layouts to pick from: `bada_describe`'s, each under the name of its kind.
    private func fill() {
        let layouts = Bada.description?.layouts ?? []
        func fill(_ button: NSPopUpButton, _ list: [LayoutInfo], prefer: String) {
            button.removeAllItems()
            for layout in list {
                button.addItem(withTitle: layout.name(in: languages))
                button.lastItem?.representedObject = layout.id
            }
            if let at = list.firstIndex(where: { $0.id == prefer }) { button.selectItem(at: at) }
        }
        // Only a layout that types directly can be the muhenkan layout (§7.5); the henkan pick is
        // among the layouts that compose.
        fill(muhenkan, layouts.filter { $0.kind == "muhenkan" }, prefer: Bada.store?.muhenkanLayout ?? "en-qwerty")
        fill(henkan, layouts.filter { $0.kind == "henkan" }, prefer: "")
        status.stringValue = Bada.refusal.map { "There is no engine: \($0)." } ?? ""
    }

    @objc private func start(_ sender: Any?) {
        guard let direct = muhenkan.selectedItem?.representedObject as? String,
              let composing = henkan.selectedItem?.representedObject as? String else { return }
        let name = henkan.selectedItem?.title ?? composing
        let url = Bada.folder.appendingPathComponent("settings.json")
        do {
            let existing = FileManager.default.contents(atPath: url.path)
            let data = try Onboarding.settings(muhenkan: direct, henkan: composing, name: name, harnessID: Bada.harnessID, existing: existing)
            try AtomicFile.write(data, to: url)
            // The folder is watched, but a moment late: read it now, so the key works at once.
            Bada.reload()
            close()
        } catch Onboarding.Failure.unreadable {
            status.stringValue = "settings.json is there already, and it isn't a bada-settings/2 file. Fix it or move it away, then try again."
        } catch {
            status.stringValue = "settings.json couldn't be written: \(error.localizedDescription)"
        }
    }

    @objc private func openFolder(_ sender: Any?) {
        try? FileManager.default.createDirectory(at: Bada.folder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Bada.folder)
    }

    @objc private func cancel(_ sender: Any?) {
        close()
    }

    private func close() {
        window?.orderOut(nil)
        background()
    }

    func windowWillClose(_ notification: Notification) {
        background()
    }

    /// A background app again, once the window is gone.
    private func background() {
        DispatchQueue.main.async { NSApp.setActivationPolicy(.prohibited) }
    }
}
