import Foundation

// The published state (the standard, §6.7): `bada-compatible-state/2`. The last reply's display
// fields, and the harness's own under `harness`. Read-only from outside.

/// The harness's own part of the state.
struct HarnessState: Equatable {
    var id: String
    var version: String
    /// The focused app's bundle id.
    var app: String?
    /// A password field has the keyboard: nothing typed is published.
    var secure = false
    /// One of bada's two modes is the selected input source.
    var active = false
    var capsLockOn = false
    var capsLockSetting = KeyTable.CapsLockSetting.respect
    var capsRule = KeyTable.CapsRule.allcaps
    /// The language keys of §9.3 that the harness sees now.
    var available: [String] = []
    /// Whether it can take chords on other keys.
    var chords = false
    /// The permissions it lacks.
    var missing: [String] = []
    /// What was wrong with the settings, layouts and dictionary it last read.
    var problems: [Problem] = []
    /// bada-tide: the keyboards it links to (bada-tide §8), as published. Nil: no tide in this harness.
    var tide: [[String: Any]]? = nil

    static func == (a: HarnessState, b: HarnessState) -> Bool {
        a.id == b.id && a.version == b.version && a.app == b.app && a.secure == b.secure && a.active == b.active
            && a.capsLockOn == b.capsLockOn && a.capsLockSetting == b.capsLockSetting && a.capsRule == b.capsRule
            && a.available == b.available && a.chords == b.chords && a.missing == b.missing && a.problems == b.problems
            && (a.tide.map { JSON.text($0) } ?? nil) == (b.tide.map { JSON.text($0) } ?? nil)
    }
}

enum PublishedState {
    static let format = "bada-compatible-state/2"

    /// `reply`: the last reply to the focused text field's events; nil before any.
    static func build(mode: Mode?, profile: String?, reply: Reply?, harness: HarnessState) -> [String: Any] {
        var state: [String: Any] = [
            "format": format,
            "mode": mode?.rawValue ?? NSNull(),
            "profile": profile ?? NSNull(),
            "staged": "",
            "preedit": ["text": "", "caret": 0, "segments": [Any]()] as [String: Any],
            "menu": ["rows": [Any](), "selected": NSNull(), "strip": false] as [String: Any],
            "ask": NSNull(),
            "dir": "ltr",
        ]
        // Nothing typed is published in a password field.
        if let reply, !harness.secure {
            state["staged"] = reply.staged
            state["preedit"] = [
                "text": reply.preedit.text, "caret": reply.preedit.caret,
                "segments": reply.preedit.segments.map { ["len": $0.len, "kind": $0.kind.rawValue] as [String: Any] },
            ] as [String: Any]
            state["menu"] = [
                "rows": reply.menu.rows.map { row -> [String: Any] in
                    var json: [String: Any] = ["text": row.text, "entry": row.entry]
                    if let note = row.note { json["note"] = note }
                    if let lang = row.lang { json["lang"] = lang }
                    if let layout = row.layout { json["layout"] = layout }
                    return json
                },
                "selected": reply.menu.selected ?? NSNull(), "strip": reply.menu.strip,
            ] as [String: Any]
            state["ask"] = reply.json["ask"] ?? NSNull()
            state["dir"] = reply.rtl ? "rtl" : "ltr"
        }
        state["harness"] = [
            "id": harness.id, "version": harness.version, "app": harness.app ?? NSNull(),
            "secure": harness.secure, "active": harness.active,
            "caps_lock": ["on": harness.capsLockOn, "setting": harness.capsLockSetting.rawValue, "rule": harness.capsRule.rawValue] as [String: Any],
            "keys": ["available": harness.available, "chords": harness.chords, "missing": harness.missing] as [String: Any],
            "problems": harness.problems.map(\.json),
        ] as [String: Any]
        if let tide = harness.tide, var h = state["harness"] as? [String: Any] {
            h["tide"] = tide
            state["harness"] = h
        }
        return state
    }

    /// Which language keys the harness sees, and which permissions it lacks (§9.3, the macOS column).
    /// `accessibility`: the key tap is on. `inputMonitoring`: keys are read at the HID level.
    /// `tide`: the keys a bada-tide keyboard reports now (bada-tide §4), which need no permission.
    static func keys(accessibility: Bool, inputMonitoring: Bool, tide: Set<String> = []) -> (available: [String], chords: Bool, missing: [String]) {
        // 英数, かな and a PC board's 半角/全角 arrive as key events; 無変換, 変換 and カタカナ only at
        // the HID level (or from a tide keyboard); Caps Lock needs both.
        var available = ["Lang1", "Lang2", "Backquote"]
        if inputMonitoring || !tide.isDisjoint(with: ["NonConvert", "Convert", "KanaMode"]) {
            available += ["NonConvert", "Convert", "KanaMode"].filter { inputMonitoring || tide.contains($0) }
        }
        if inputMonitoring && accessibility { available.append("CapsLock") }
        var missing: [String] = []
        if !accessibility { missing.append("Accessibility") }
        if !inputMonitoring { missing.append("Input Monitoring") }
        return (available, accessibility, missing)
    }
}
