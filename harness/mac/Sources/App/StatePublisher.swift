import Carbon
import Cocoa

/// bada's state for other programs (the standard, §6.7), read-only: `bada-compatible-state/2`, the
/// last reply's display fields with the harness's own under `harness`, published whenever it changes.
///
/// - File: `~/Library/Application Support/bada/state.json`, rewritten atomically (a temporary file,
///   then a rename).
/// - Distributed notification `com.teammirae.inputmethod.Bada.state`, whose object is the same JSON.
///
/// Hammerspoon: `hs.pathwatcher` on the file or `hs.distributednotifications.new(fn,
/// "com.teammirae.inputmethod.Bada.state")`. Nothing read from outside flows back into the engine.
///
/// In a password field (secure input) nothing typed is published: `staged`, `preedit` and `menu`
/// are empty, and `harness.secure` is true.
enum StatePublisher {
    static let notification = Notification.Name(Bada.bundleID + ".state")
    static let url = Bada.folder.appendingPathComponent("state.json")

    /// The last reply of the focused text field, and the app it is in: kept so a Caps Lock or input
    /// source change can publish without an engine call.
    private static var reply: Reply?
    private static var app: String?
    private static var published: String?

    /// After a reply to a text field's event, or a focus change.
    static func publish(_ reply: Reply?, app: String?) {
        self.reply = reply
        if let app { self.app = app }
        refresh()
    }

    /// The state as it is now.
    static func current() -> [String: Any] {
        let keys = PublishedState.keys(accessibility: LanguageKeyTap.isOn, inputMonitoring: HIDKeys.isListening,
                                       tide: Tide.availableKeys)
        var problems = (Bada.store?.problems ?? []) + Tide.problems
        if let refusal = Bada.refusal {
            problems.insert(Problem(file: "", path: "", code: "no-engine", message: refusal, level: "error"), at: 0)
        }
        let harness = HarnessState(
            id: Bada.harnessID, version: Bada.version, app: app,
            secure: IsSecureEventInputEnabled(), active: Modes.isOurs,
            capsLockOn: CapsLock.isOn(NSEvent.modifierFlags), capsLockSetting: CapsLock.setting, capsRule: CapsLock.rule,
            available: keys.available, chords: keys.chords, missing: keys.missing, problems: problems,
            tide: Tide.published)
        return PublishedState.build(mode: Modes.current, profile: Modes.profile, reply: reply, harness: harness)
    }

    /// Publishes again with the harness's fields as they are now (mode, profile, Caps Lock, input
    /// source, permissions, problems). Only when the JSON changed.
    static func refresh() {
        Tide.changed()   // mode, profile, Caps Lock or the table may have changed: renew the lease now
        guard let json = JSON.text(current()), json != published else { return }
        published = json
        try? AtomicFile.write(Data(json.utf8), to: url)
        DistributedNotificationCenter.default().postNotificationName(notification, object: json, userInfo: nil, deliverImmediately: true)
    }
}
