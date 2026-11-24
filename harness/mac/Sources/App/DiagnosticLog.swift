import Foundation

/// The input method's own diagnostics: one line per event in ~/Library/Logs/Bada.log. Off unless
/// the settings file turns it on: `"harness": {"com.teammirae.bada.mac": {"log": true}}`.
/// Nothing typed is ever written.
enum DiagnosticLog {
    /// Whether the log is on: checked before building a line that costs something.
    static var isOn: Bool { Bada.store?.harness.log ?? false }

    static func note(_ message: String) {
        guard isOn else { return }
        let url = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs/Bada.log")
        let line = "\(ISO8601DateFormatter().string(from: Date())) [\(ProcessInfo.processInfo.processIdentifier)] \(message)\n"
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(line.data(using: .utf8)!)
            try? handle.close()
        } else {
            try? line.write(to: url, atomically: true, encoding: .utf8)
        }
    }
}
