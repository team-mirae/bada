import Cocoa
import WebKit

/// The settings window (the standard, §10.1): the shared settings page (settings/), in a web view.
///
/// The page reads and writes nothing itself: a host does. Here the host is the command line inside
/// the bundle (Contents/Helpers), `bada settings --no-open --embedded`, which serves the page on 127.0.0.1 and reads
/// and writes the settings folder (settings/README.md). It runs while the window is open.
final class SettingsWindow: NSObject, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate {
    static let shared = SettingsWindow()

    private var window: NSWindow?
    private var web: WKWebView?
    private var host: Process?
    /// The app is only this window (`Bada --settings`): closing it ends the app.
    var quitsWhenClosed = false

    func show() {
        if window == nil { build() }
        if host?.isRunning != true { start() }
        // bada is background-only: while the window is open it is an accessory app, so the window
        // can be key (OnboardingWindow has the same).
        NSApp.setActivationPolicy(.accessory)
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }

    private func build() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 760),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "bada settings"
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 520, height: 420)
        window.delegate = self
        window.center()
        window.setFrameAutosaveName("BadaSettings")
        let web = WKWebView(frame: window.contentLayoutRect, configuration: WKWebViewConfiguration())
        web.navigationDelegate = self
        web.uiDelegate = self
        web.autoresizingMask = [.width, .height]
        window.contentView = web
        self.window = window
        self.web = web
    }

    /// Starts the host and loads the address it prints: the first line of its output.
    private func start() {
        // In Contents/Helpers, apart from the app's own program: a Mac's disk doesn't tell bada from Bada.
        let tool = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/bada")
        guard FileManager.default.isExecutableFile(atPath: tool.path),
              let page = Bundle.main.resourceURL?.appendingPathComponent("settings", isDirectory: true) else {
            return say("The command line isn't in this copy of bada, so the settings page can't be shown. The settings are the files in the settings folder.")
        }
        let host = Process()
        host.executableURL = tool
        host.arguments = ["settings", "--no-open", "--embedded", "--page", page.path]
        // The same folder the input method reads, wherever a test run has put it.
        var environment = ProcessInfo.processInfo.environment
        environment["BADA_CONFIG_DIR"] = Bada.folder.path
        host.environment = environment
        let out = Pipe()
        host.standardOutput = out
        host.standardError = FileHandle.nullDevice
        var seen = Data()
        out.fileHandleForReading.readabilityHandler = { [weak self] handle in
            seen.append(handle.availableData)
            guard let end = seen.firstIndex(of: 0x0A) else { return }
            handle.readabilityHandler = nil
            let line = String(decoding: seen[..<end], as: UTF8.self).trimmingCharacters(in: .whitespaces)
            DispatchQueue.main.async {
                guard let url = URL(string: line), url.host == "127.0.0.1" else {
                    self?.say("The settings host didn't start.")
                    return
                }
                self?.web?.load(URLRequest(url: url))
            }
        }
        do {
            try host.run()
            self.host = host
        } catch {
            say("The settings host didn't start: \(error.localizedDescription)")
        }
    }

    private func say(_ text: String) {
        let safe = text.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        web?.loadHTMLString("<body style=\"font: 14px -apple-system; margin: 40px; color: #5d6b78\">\(safe)</body>", baseURL: nil)
    }

    private func stop() {
        host?.terminate()
        host = nil
    }

    func windowWillClose(_ notification: Notification) {
        stop()
        // The page is loaded again from a new host the next time: its address dies with this one.
        web?.loadHTMLString("", baseURL: nil)
        if quitsWhenClosed { exit(0) }
        DispatchQueue.main.async { NSApp.setActivationPolicy(.prohibited) }
    }

    // MARK: What the page asks of the app

    /// The page's own address stays in the view. `bada-app:` is the page asking the app for what
    /// only the app can do; any other link opens in the browser.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = action.request.url else { return decisionHandler(.cancel) }
        if url.scheme == "bada-app" {
            switch url.absoluteString {
            case "bada-app:ask/accessibility": LanguageKeyTap.start(ask: true)
            case "bada-app:ask/input-monitoring": HIDKeys.start(ask: true)
            default: break
            }
            return decisionHandler(.cancel)
        }
        if url.host == "127.0.0.1" || url.scheme == "about" { return decisionHandler(.allow) }
        if url.scheme == "https" || url.scheme == "http" { NSWorkspace.shared.open(url) }
        decisionHandler(.cancel)
    }

    /// A link that asks for a new window: the browser has it.
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for action: WKNavigationAction,
                 windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let url = action.request.url, url.scheme == "https" || url.scheme == "http" { NSWorkspace.shared.open(url) }
        return nil
    }

    /// The page's Import: a file the user picks.
    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters, initiatedByFrame frame: WKFrameInfo,
                 completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.begin { completionHandler($0 == .OK ? panel.urls : nil) }
    }
}
