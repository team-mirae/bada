import Cocoa
import InputMethodKit

// Input methods are background apps: IMKServer accepts client connections, and the system creates
// one BadaInputController per text input client.

if CommandLine.arguments.contains("--allow-keys") {
    // Asks macOS for what the language keys need, as the input menu's "Allow Language Keys
    // Everywhere…" does, with no input method started. macOS shows its own prompts; this waits
    // while they are up, and leaves when both are allowed or after two minutes.
    let prompt = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
    _ = AXIsProcessTrustedWithOptions(prompt)
    HIDKeys.ask()
    let until = Date().addingTimeInterval(120)
    while Date() < until, !(AXIsProcessTrusted() && HIDKeys.isAllowed) {
        RunLoop.main.run(until: Date().addingTimeInterval(1))
    }
    print("Accessibility: \(AXIsProcessTrusted() ? "allowed" : "not allowed"); Input Monitoring: \(HIDKeys.isAllowed ? "allowed" : "not allowed")")
    exit(0)
}

if CommandLine.arguments.contains("--check") {
    // A self-check, with no input method started: the engine linked in, the settings folder as it
    // would be read, and the state as it would be published. Prints, and changes nothing.
    Bada.start(watch: false)
    if let refusal = Bada.refusal {
        print("no engine: \(refusal)")
        exit(1)
    }
    let description = Bada.description!
    print("bada \(Bada.version) (\(Bada.harnessID)); engine \(description.id) \(description.version), bada-compatible \(description.abiMajor).\(description.abiMinor)")
    print("layouts: \(description.layouts.map(\.id).joined(separator: " "))")
    print("data: \(Bada.dataNames.isEmpty ? "none" : Bada.dataNames.joined(separator: " "))")
    print("folder: \(Bada.folder.path)")
    print("muhenkan layout: \(Bada.store?.muhenkanLayout ?? "none"); profiles: \(Bada.profiles.map(\.id))")
    print("key table: \(Bada.table.bindings.map { $0.on.text + " → " + $0.actions.map(\.name).joined(separator: ", ") })")
    for problem in Bada.store?.problems ?? [] { print("\(problem.level): \(problem.file) \(problem.path): \(problem.message)") }
    print(JSON.text(StatePublisher.current(), pretty: true) ?? "")
    exit(0)
}

if CommandLine.arguments.contains("--tide-helper") {
    // bada-tide's device side (TideHelper), started by the input method: a plain run loop, no
    // NSApplication. Runs until its stdin closes.
    TideHelper.run()
}

if CommandLine.arguments.contains("--tide-probe") {
    // The helper with its notes on stderr, for finding why a link fails: run it from a terminal
    // (`BADA_HOME` names the settings folder), and type state lines on stdin if needed.
    TideHelper.verbose = true
    TideHelper.run()
}

if CommandLine.arguments.contains("--settings") {
    // The settings window alone, with no input method started: a way to see it from a build.
    // `BADA_HOME` names the folder it changes, as for `--check`. It leaves when the window closes.
    SettingsWindow.shared.quitsWhenClosed = true
    SettingsWindow.shared.show()
    NSApplication.shared.run()
    exit(0)
}

let connectionName = Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String ?? "com.teammirae.inputmethod.Bada_Connection"
Bada.start()
Modes.start()
DiagnosticLog.note("launched from \(Bundle.main.bundlePath)")
LanguageKeyTap.check()
let server = IMKServer(name: connectionName, bundleIdentifier: Bundle.main.bundleIdentifier)
_ = server
NSApplication.shared.run()
