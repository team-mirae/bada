import Foundation

// The harness's own rules, apart from the two replays: how it reads a description and a reply, how
// it applies one (the standard, §6.3), what a key passed on types (§6.4), the settings folder
// (§10.1), the published state (§6.7), the first run (§8.3), and the corners of the key table that
// the standard's cases leave to the harness.
//   unit <repository root> <a folder to work in>

var failures = 0
var checks = 0
func check(_ condition: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !condition() {
        failures += 1
        print("FAIL (line \(line)): \(what)")
    }
}

guard CommandLine.arguments.count == 3 else {
    print("usage: unit <repository root> <a folder to work in>")
    exit(2)
}
let root = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let harnessID = "com.teammirae.bada.mac"

// MARK: The engine's description (§1, §3.2)

do {
    let description = try EngineDescription.linked()
    check(description.abiName == "bada-compatible" && description.abiMajor == 2, "the linked engine is bada-compatible 2")
    check(description.id == "com.teammirae.bada", "its id")
    check(description.layouts.contains { $0.id == "en-qwerty" && $0.kind == "muhenkan" }, "en-qwerty types directly")
    check(description.layouts.contains { $0.id == "ja-romaji" && $0.kind == "henkan" }, "ja-romaji composes")
    check((description.defaultSettings["henkan"] as? [String: Any])?["profiles"] as? [Any] != nil, "a new install's settings have a profile list")
    check(description.layouts.first { $0.id == "ko-2-set" }?.name(in: ["ko-KR", "en"]) == "한국어 - 두벌식", "a layout's name by language")
    check(description.layouts.first { $0.id == "ko-2-set" }?.name(in: ["fr"]) == "Korean - 2-set", "…and in English otherwise")
} catch {
    check(false, "the linked engine is refused: \(error)")
}
func refusal(_ text: String) -> EngineDescription.Refusal? {
    do { _ = try EngineDescription(text: text); return nil } catch { return error as? EngineDescription.Refusal }
}
check(refusal(#"{"abi": {"name": "bada-compatible", "major": 3, "minor": 0}}"#) == .abi(name: "bada-compatible", major: 3), "an engine with major 3 is refused")
check(refusal(#"{"abi": {"name": "bada-compatible", "major": 1, "minor": 3}}"#) == .abi(name: "bada-compatible", major: 1), "an engine with major 1 is refused")
check(refusal(#"{"abi": {"name": "other", "major": 2, "minor": 0}}"#) == .abi(name: "other", major: 2), "another standard is refused")
check(refusal("{}") == .unreadable && refusal("nonsense") == .unreadable, "a description that can't be read is refused")
check(refusal(#"{"abi": {"name": "bada-compatible", "major": 2, "minor": 7}, "novelty": 1}"#) == nil, "a later minor version is taken, with what it adds ignored")

// MARK: Data (§11)

check(EngineData() != nil, "no buffers: bada_data_new(NULL, 0)")
check(EngineData([EngineData.Buffer(name: "a-name-the-engine-doesnt-know", bytes: Data([1, 2, 3]), borrowed: true)]) != nil,
      "a buffer the engine doesn't know is ignored")

// MARK: Replies (§6.1)

let bare = Reply(text: #"{"ok": true}"#)
check(bare.ok && bare.commit == "" && !bare.replacesSelection && !bare.consumed && bare.types == nil, "a missing field is its default")
check(bare.preedit.text == "" && bare.menu.rows.isEmpty && bare.menu.selected == nil && !bare.menu.strip, "…the preedit and the menu too")
check(bare.mode == nil && bare.profile == nil && bare.staged == "" && bare.ask == nil && !bare.rtl, "…and the rest")
let full = Reply(text: #"{"ok": true, "commit": "あ", "replace": "selection", "consumed": true, "types": null, "preedit": {"text": "a😀b", "caret": 2, "segments": [{"len": 1, "kind": "fixed"}, {"len": 1, "kind": "picked"}, {"len": 1, "kind": "live"}]}, "menu": {"rows": [{"text": "x", "entry": 0, "lang": "ja", "note": "n", "layout": "ja-kana"}, {"text": "y", "entry": -1}], "selected": 1, "strip": true}, "mode": "henkan", "profile": "work", "staged": "a😀b", "ask": {"around": {"before": 20, "after": 0}}, "dir": "rtl", "later": 1}"#)
check(full.replacesSelection && full.consumed && full.mode == .henkan && full.profile == "work" && full.rtl, "a reply's fields")
check(full.menu.rows.count == 2 && full.menu.rows[0].lang == "ja" && full.menu.rows[0].note == "n" && full.menu.rows[1].entry == -1 && full.menu.selected == 1 && full.menu.strip, "its menu")
check(full.ask == .around(before: 20, after: 0), "its ask for the text around the caret")
check(Reply(text: #"{"ok": true, "ask": {"selection": "hanja"}}"#).ask == .selection("hanja"), "an ask for the selection")
// Positions are in Unicode scalar values; the app's text field counts UTF-16.
check(full.preedit.utf16Offset(scalars: 2) == 3, "a caret after an emoji, in UTF-16")
let runs = full.preedit.runs
check(runs.count == 3 && runs[0].range == NSRange(location: 0, length: 1) && runs[1].range == NSRange(location: 1, length: 2)
      && runs[2].range == NSRange(location: 3, length: 1), "segments as UTF-16 ranges")
check(runs.map(\.kind) == [.fixed, .picked, .live], "…with their kinds")
let short = Reply(text: #"{"ok": true, "preedit": {"text": "abc", "segments": [{"len": 1, "kind": "fixed"}]}}"#)
check(short.preedit.runs.count == 1 && short.preedit.runs[0].kind == .live && short.preedit.runs[0].range.length == 3, "segments that don't add up aren't trusted")
check(short.preedit.caret == 3, "a missing caret is the end")
check(Reply(text: #"{"ok": true, "preedit": {"text": "", "caret": null, "segments": []}}"#).preedit.caret == 0, "a null caret too")
let refused = Reply(text: #"{"ok": false, "error": {"code": "no-profile", "message": "m"}}"#)
check(!refused.ok && refused.error?.code == "no-profile", "an error and its code")
check(Reply(text: "not json").error?.code == "bad-json" && !Reply(text: "not json").ok, "a reply that isn't JSON changes nothing")

// MARK: Applying a reply (§6.3)

func reply(_ json: String) -> Reply { Reply(text: json) }
do {
    let field = TextFieldModel()
    field.insertAndSelect("hello ")
    field.select(6..<6)
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "한", "caret": 1}}"#), to: field, answer: nil)
    check(field.string == "hello 한" && field.markedString == "한" && field.caret == 7, "a preedit is marked at the caret")
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "한국", "caret": 1}}"#), to: field, answer: nil)
    check(field.string == "hello 한국" && field.markedString == "한국" && field.caret == 7, "the next one replaces it, with the caret where it says")
    ReplyApplier.apply(reply(#"{"ok": false, "error": {"code": "bad-row", "message": ""}}"#), to: field, answer: nil)
    check(field.string == "hello 한국" && field.markedString == "한국", "ok false changes nothing")
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "한국", "preedit": {"text": "ㅇ"}}"#), to: field, answer: nil)
    check(field.settled == "hello 한국" && field.markedString == "ㅇ" && field.caret == 9, "commit, then the new preedit after it")
    ReplyApplier.apply(reply(#"{"ok": true}"#), to: field, answer: nil)
    check(field.string == "hello 한국" && !field.hasMarked && field.caret == 8, "an empty preedit deletes the marked text")
    // A commit on the selection.
    field.select(0..<5)
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "ＨＥＬＬＯ", "replace": "selection"}"#), to: field, answer: nil)
    check(field.string == "ＨＥＬＬＯ 한국" && field.caret == 5, "replace selection: the commit takes the selection's place")
    // …even when text is marked: the marked text stays, no longer marked.
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "x"}}"#), to: field, answer: nil)
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "Z", "replace": "selection"}"#), to: field, answer: nil)
    check(field.string == "ＨＥＬＬＯxZ 한국" && !field.hasMarked && field.caret == 7, "replace selection with text marked: the commit goes to the selection, not over the marked text")
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "y"}}"#), to: field, answer: nil)
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "W"}"#), to: field, answer: nil)
    check(field.string == "ＨＥＬＬＯxZW 한국" && !field.hasMarked, "replace marked: the commit takes the marked text's place")
    field.select(0..<8)
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "ＨＥＬＬＯ"}"#), to: field, answer: nil)
    // With nothing marked, a plain commit replaces the selection too.
    field.select(0..<5)
    ReplyApplier.apply(reply(#"{"ok": true, "commit": "hi"}"#), to: field, answer: nil)
    check(field.string == "hi 한국" && field.caret == 2, "with nothing marked the commit replaces the selection")
    // A panic: the engine has reset itself, so the marked text and the menu go.
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "かん"}, "menu": {"rows": [{"text": "かん", "entry": 0}, {"text": "缶", "entry": 1}], "selected": 0}}"#), to: field, answer: nil)
    check(field.markedString == "かん" && field.menu.rows.count == 2, "a preedit with a menu")
    ReplyApplier.apply(reply(#"{"ok": false, "error": {"code": "panic", "message": "boom"}}"#), to: field, answer: nil)
    check(field.string == "hi 한국" && !field.hasMarked && field.menu.rows.isEmpty, "on panic the marked text is deleted and the menu closed")
}
do {
    // An ask is answered once, with one text event, and never deeper.
    let field = TextFieldModel()
    field.insertAndSelect("abc")
    var events: [[String: Any]] = []
    ReplyApplier.apply(reply(#"{"ok": true, "consumed": true, "ask": {"selection": "full"}}"#), to: field) { event in
        events.append(event)
        // The answer's reply asks again: that one is not answered (a few times at most, so a
        // harness that did answer would fail here rather than never return).
        return reply(events.count < 4 ? #"{"ok": true, "commit": "ａｂｃ", "replace": "selection", "ask": {"selection": "again"}}"# : #"{"ok": true}"#)
    }
    check(events.count == 1 && events[0]["type"] as? String == "text" && events[0]["selection"] as? String == "abc", "the selection is sent once")
    check(field.string == "ａｂｃ", "…and the answer's reply is applied")
    // With text marked, nothing counts as selected.
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "x"}, "ask": {"selection": "hanja"}}"#), to: field) { event in
        events.append(event)
        return reply(#"{"ok": true, "preedit": {"text": "x"}}"#)
    }
    check(events.count == 2 && events[1]["selection"] as? String == "", "with something marked the selection is \"\"")
    ReplyApplier.apply(reply(#"{"ok": true}"#), to: field, answer: nil)
    // The text around the caret, no longer than asked.
    field.select(2..<2)
    ReplyApplier.apply(reply(#"{"ok": true, "ask": {"around": {"before": 1, "after": 5}}}"#), to: field) { event in
        events.append(event)
        return reply(#"{"ok": true}"#)
    }
    check(events.count == 3 && events[2]["before"] as? String == "ｂ" && events[2]["after"] as? String == "ｃ" && events[2]["selection"] == nil,
          "the text around the caret is sent, no longer than asked")
    // With text marked: around that text, not inside it.
    ReplyApplier.apply(reply(#"{"ok": true, "preedit": {"text": "한국", "caret": 1}, "ask": {"around": {"before": 2, "after": 2}}}"#), to: field) { event in
        events.append(event)
        return reply(#"{"ok": true}"#)
    }
    check(events.count == 4 && events[3]["before"] as? String == "ａｂ" && events[3]["after"] as? String == "ｃ", "with text marked, the text around it")
    events.removeLast()
    // A refused event asks nothing.
    ReplyApplier.apply(reply(#"{"ok": false, "error": {"code": "bad-event", "message": ""}, "ask": {"selection": "x"}}"#), to: field) { event in
        events.append(event)
        return reply(#"{"ok": true}"#)
    }
    check(events.count == 3, "a refused event's ask isn't answered")
}

// MARK: Keys passed on (§6.4)

check(Typing.resolve(types: nil, osTypes: "a", capsLock: false) == .app, "types null: the app gets the key as the OS has it")
check(Typing.resolve(types: "a", osTypes: "a", capsLock: false) == .app, "the OS types the same: it types")
check(Typing.resolve(types: "\\", osTypes: "_", capsLock: false) == .insert("\\"), "the OS would type something else: the harness inserts")
check(Typing.resolve(types: "ㅂ", osTypes: nil, capsLock: false) == .insert("ㅂ"), "the OS types nothing (a dead key): the harness inserts")
check(Typing.resolve(types: "a", osTypes: "a", capsLock: true) == .insert("A"), "the lock on: letters in capitals")
check(Typing.resolve(types: "a", osTypes: "A", capsLock: true) == .app, "…typed by the OS when it has the lock too")
check(Typing.resolve(types: "1", osTypes: "1", capsLock: true) == .app, "the lock leaves digits alone")
check(Typing.capitals("aß1é") == "ASS1É", "capitals of letters only")

// MARK: The key table, where the cases are silent

let profiles = [Profile(id: "main", name: "main", layouts: ["ko-2-set"]), Profile(id: "ja", name: "ja", layouts: ["ja-kana"])]
do {
    let example = JSON.object(try Data(contentsOf: root.appendingPathComponent("docs/schemas/examples/settings.json"))) ?? [:]
    let read = KeyTable.read(example["keys"], preset: .jisMac)
    check(read.flaws.isEmpty && read.table.bindings.count == 12 && read.table.cycle == ["main", "work", "ja"], "the schema's example settings file is read")
    check(read.table.pairs == [Chord.pair(of: "KanaMode", "Lang1")], "…with its pair")
    // かな through a session with that file (§9.5): the next entry wherever the menu has two rows or
    // more, and otherwise the key to its profile. The state is carried from press to press.
    var state = KeyState(mode: .muhenkan, profile: nil)
    func press(_ code: String, _ mods: Mods = []) -> Decision {
        let decision = read.table.decide(.key(code, mods), state: state, profiles: profiles)
        state.apply(decision)
        return decision
    }
    /// What the engine's reply would leave after typing.
    func typed(_ staged: String, rows: Int) { state.staged = staged; state.menuRows = rows }
    check(press("Lang1").then == .select(.henkan, "main"), "かな in muhenkan enters its profile")
    check(press("KanaMode").then == .select(.henkan, "ja"), "カタカナ goes to its own")
    typed("かんじ", rows: 3)
    check(press("Lang1").then == .action("menu.next") && state.profile == "ja", "かな with a menu in another profile is the next entry, and the profile stays")
    check(press("Lang1", .shift).then == .action("menu.prev") && state.profile == "ja", "…and ⇧かな the one before")
    typed("か", rows: 1)
    check(press("Lang1", .shift).then == .swallow && state.profile == "ja", "⇧かな with one row does nothing")
    check(press("Lang1") == Decision(then: .select(.henkan, "main"), binding: 7, ran: "profile:main", capsLock: nil), "かな with one row isn't the next entry: it goes to its profile")
    check(state.mode == .henkan && state.profile == "main" && state.previous == "ja" && state.staged == "" && state.menuRows == 0, "…which commits what was staged")
    check(press("Lang1").then == .swallow, "かな in its own profile with no menu does nothing")
    typed("한", rows: 2)
    check(press("Lang1").then == .action("menu.next") && state.profile == "main", "…and with a menu it is the next entry")
    check(press("KanaMode").then == .select(.henkan, "ja") && press("Lang1").then == .select(.henkan, "main"), "with nothing staged in another profile, かな is its profile's key")
    check(press("Lang2").then == .select(.muhenkan, "main") && press("Lang1").then == .select(.henkan, "main"), "…and from muhenkan too")
} catch {
    check(false, "the example settings file can't be read: \(error)")
}
func flawPaths(_ keys: [String: Any]) -> [String] { KeyTable.read(keys, preset: .jisMac).flaws.map { "\($0.path) \($0.code)" } }
check(flawPaths(["bindings": [["on": "shift+ctrl+Lang1", "do": ["fix"]]]]) == ["keys.bindings[0].on bad-value"], "modifiers out of order aren't a chord")
check(flawPaths(["bindings": [["on": "Lang1", "do": ["jump"]]]]) == ["keys.bindings[0].do[0] unknown-action"], "an action that doesn't exist")
check(flawPaths(["bindings": [["on": "Lang1", "do": [String]()]]]) == ["keys.bindings[0].do bad-value"], "an empty do list")
check(flawPaths(["bindings": [["on": "Lang1", "do": ["fix"], "when": "sometimes"]]]) == ["keys.bindings[0].when bad-value"], "a when that isn't one")
check(flawPaths(["bindings": [["on": "CapsLock+Lang1", "do": ["fix"]]]]) == ["keys.bindings[0].on bad-value"], "Caps Lock makes no pair")
check(flawPaths(["bindings": [["on": "Lang1+Lang1", "do": ["fix"]]]]) == ["keys.bindings[0].on bad-value"], "a key makes no pair with itself")
check(flawPaths(["caps_lock": "toggle"]) == ["keys.caps_lock bad-value"], "caps_lock has two values")
check(flawPaths(["extra": 1]) == ["keys.extra unknown-field"], "a field keys doesn't have")
check(KeyTable.read(["bindings": [Any]()], preset: .jisMac).table.bindings.isEmpty, "bindings that are there, even none, are used as they are")
check(KeyTable.read(["cycle": ["a"]], preset: .jisPC).table.bindings == KeyTable.preset(.jisPC), "without bindings the keyboard's preset, whatever else is set")
check(Chord("ctrl+alt+shift+meta+Space")?.text == "ctrl+alt+shift+meta+Space" && Chord("Lang1+KanaMode")?.text == "KanaMode+Lang1", "a chord is written as on has it")
do {
    var table = KeyTable()
    table.bindings = [KeyTable.Binding(on: .key("F13", []), actions: [.profileFirst], when: nil),
                      KeyTable.Binding(on: .key("Lang1", []), actions: [.mode(.henkan), .menuNext], when: nil),
                      KeyTable.Binding(on: .key("Lang2", []), actions: [.mode(.muhenkan)], when: nil),
                      KeyTable.Binding(on: .key("Lang1", []), actions: [.fix], when: .henkan)]
    let muhenkan = KeyState(mode: .muhenkan, profile: nil)
    // Chords on other keys need Accessibility: without it only language keys are the table's.
    check(table.decide(.key("F13", []), state: muhenkan, profiles: profiles).then == .select(.henkan, "main"), "a chord's binding, when chords are seen")
    check(table.decide(.key("F13", []), state: muhenkan, profiles: profiles, chords: false) == Decision(then: .key), "…and not when they aren't: the key is the engine's")
    check(table.decide(.key("Lang1", []), state: muhenkan, profiles: profiles, chords: false).then == .select(.henkan, "main"), "a language key's binding holds either way")
    // While another input source is selected no mode is in use.
    var away = KeyState(mode: .henkan, profile: "main", staged: "한")
    away.away = true
    check(table.decide(.key("Lang1", []), state: away, profiles: profiles) == Decision(then: .select(.henkan, "main"), binding: 1, ran: "mode:henkan", capsLock: nil),
          "away: mode:henkan applies though henkan was the last mode, and no when holds")
    away.mode = .muhenkan
    check(table.decide(.key("Lang2", []), state: away, profiles: profiles).then == .select(.muhenkan, "main"), "away: mode:muhenkan applies too")
    // The profile after a select, and the one before it.
    var state = KeyState(mode: .muhenkan, profile: "main")
    state.apply(Decision(then: .select(.henkan, "ja")))
    check(state.mode == .henkan && state.profile == "ja" && state.previous == "main", "a select makes its profile the one in use, and remembers the one before")
    state.apply(Decision(then: .select(.muhenkan, "ja")))
    check(state.mode == .muhenkan && state.profile == "ja" && state.previous == "main", "leaving henkan keeps both")
    check(table.entering(last: "ja", profiles: profiles) == "ja" && table.entering(last: "gone", profiles: profiles) == "main"
          && table.entering(last: nil, profiles: []) == nil, "the profile henkan is entered with")
}
do {
    // Pairs: a repeat of a held key is no new press, and a release that never comes is given up.
    var sequencer = KeySequencer()
    sequencer.pairs = [Chord.pair(of: "KanaMode", "Lang1")]
    check(sequencer.down("Lang1").all.isEmpty && sequencer.isWaiting, "a key of a bound pair waits")
    check(sequencer.down("Lang1", isRepeat: true).all.isEmpty, "its repeats are nothing")
    check(sequencer.down("KeyA", isRepeat: true) == KeySequencer.Down(late: [], now: .key("KeyA", [])) && sequencer.isWaiting, "another key's repeat doesn't end the wait")
    check(sequencer.expire() == [.key("Lang1", [])] && !sequencer.isWaiting, "a wait given up runs the key alone")
    check(sequencer.up("Lang1").isEmpty, "…and its late release is nothing")
    check(sequencer.down("Lang1", mods: .shift).all == [.key("Lang1", .shift)], "with a modifier it is another chord: it runs when pressed")
    check(sequencer.down("Lang1", mayWait: false).all == [.key("Lang1", [])], "a key that may not wait runs when pressed")
    sequencer.pairs = []
    check(sequencer.down("Lang1").all == [.key("Lang1", [])], "with no pair seen, nothing waits")
    // "Another key" is any key, a modifier included: it ends a wait, and breaks a pair that is forming.
    sequencer.pairs = [Chord.pair(of: "KanaMode", "Lang1")]
    _ = sequencer.down("Lang1")
    check(sequencer.down("ShiftLeft", mayWait: false).late == [.key("Lang1", [])], "a modifier ends a wait: the key runs its own binding")
    _ = sequencer.up("Lang1")
    _ = sequencer.down("Lang1")
    _ = sequencer.down("KanaMode")
    check(sequencer.down("AltRight", mayWait: false).late.isEmpty && sequencer.up("KanaMode").isEmpty && sequencer.up("Lang1").isEmpty,
          "a modifier while both are down: no pair, and nothing for either key")
}
do {
    // A key waits only for a pair binding that holds when it is pressed.
    let read = KeyTable.read(["bindings": [["on": "Lang1+Lang2", "do": ["none"], "when": "henkan"], ["on": "Convert+KanaMode", "do": ["fix"]]]], preset: .jisMac)
    check(read.table.pairs(holding: KeyState(mode: .muhenkan, profile: nil)) == [Chord.pair(of: "Convert", "KanaMode")], "in muhenkan a pair bound for henkan isn't waited for")
    check(read.table.pairs(holding: KeyState(mode: .henkan, profile: "main")).count == 2, "in henkan it is")
}

// MARK: The settings folder (§10.1)

// A settings folder of the test's own: the user's is never read or written.
let temporary = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true).standardizedFileURL
try? FileManager.default.removeItem(at: temporary)
try FileManager.default.createDirectory(at: temporary.appendingPathComponent("layouts"), withIntermediateDirectories: true)
func put(_ file: String, _ text: String) {
    try! AtomicFile.write(Data(text.utf8), to: temporary.appendingPathComponent(file))
}
func remove(_ file: String) { try? FileManager.default.removeItem(at: temporary.appendingPathComponent(file)) }

let description = try EngineDescription.linked()
let session = EngineSession(data: EngineData(), harness: harnessID, version: "0.1.0", features: ["segments", "pick", "around"])!
let store = SettingsStore(folder: temporary, session: session, defaults: description.defaultSettings, harnessID: harnessID, autoPreset: .jisMac)
/// The engine's own view of its settings, to see what really is in effect.
func inEffect() -> [String: Any] { session.send(["type": "settings"]).json["settings"] as? [String: Any] ?? [:] }
func layoutInEffect() -> String? { (inEffect()["muhenkan"] as? [String: Any])?["layout"] as? String }

store.reload()
check(store.profiles.isEmpty && store.muhenkanLayout == nil && store.problems.isEmpty, "an empty folder is a new install: no layout, no profile")
check(store.table.bindings == KeyTable.preset(.jisMac), "…with the keyboard's preset")
check(store.layouts.count == description.layouts.count && store.layouts.allSatisfy { $0.stock && !$0.modified }, "…and the stock layouts")
check(!store.reload(), "reading it again changes nothing")

let example = try String(contentsOf: root.appendingPathComponent("docs/schemas/examples/settings.json"), encoding: .utf8)
put("settings.json", example)
check(store.reload(), "a settings file is taken")
check(store.profiles.map(\.id) == ["main", "work", "ja", "sym"] && store.profiles[0].name == "한국어" && store.muhenkanLayout == "en-qwerty", "its profiles and its muhenkan layout")
check(store.table.bindings.count == 12 && store.table.cycle == ["main", "work", "ja"] && store.problems.isEmpty, "its key table, and no problem")
check(layoutInEffect() == "en-qwerty", "the engine has its part")

// A file that can't be used changes nothing, and the problems are kept.
put("settings.json", "{ this isn't JSON")
check(store.reload(), "a broken file is noticed")
check(store.problems.map(\.code) == ["bad-json"] && store.problems[0].file == "settings.json" && store.problems[0].isError, "…as a problem")
check(store.profiles.count == 4 && store.table.bindings.count == 12 && layoutInEffect() == "en-qwerty", "…and nothing changed")
put("settings.json", #"{"format": "bada-settings/1", "settings": {}}"#)
store.reload()
check(store.problems.map(\.code) == ["bad-format"] && store.profiles.count == 4, "another format changes nothing")
// A flaw in the key table refuses the whole file: the engine's part isn't taken either.
put("settings.json", #"{"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-dvorak"}}, "keys": {"bindings": [{"on": "Lang1", "do": ["jump"]}]}}"#)
store.reload()
check(store.problems.map { "\($0.path) \($0.code)" } == ["keys.bindings[0].do[0] unknown-action"], "a flaw in keys is a problem with its path")
check(store.muhenkanLayout == "en-qwerty" && layoutInEffect() == "en-qwerty" && store.table.bindings.count == 12, "…and the file changes nothing, the engine's part included")
// And the other way round: an error in the engine's part keeps the old key table.
put("settings.json", #"{"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "x", "name": "x", "layouts": ["xx-nope"]}]}}, "keys": {"bindings": []}}"#)
store.reload()
check(store.problems.map { "\($0.path) \($0.code)" } == ["engine.henkan.profiles[0].layouts[0] unknown-layout"], "the engine's problem, with a path into the file")
check(store.profiles.count == 4 && store.table.bindings.count == 12, "…and nothing changed")
put("settings.json", #"{"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "ko-2-set"}}}"#)
store.reload()
check(store.problems.map { "\($0.path) \($0.code)" } == ["engine.muhenkan.layout not-muhenkan"] && store.muhenkanLayout == "en-qwerty", "a layout that composes can't be the muhenkan layout")
put("settings.json", #"{"format": "bada-settings/2", "extra": {}}"#)
store.reload()
check(store.problems.map { "\($0.path) \($0.code)" } == ["extra unknown-field"], "a part the file doesn't have")

// The harness's own part: the keyboard whose preset the table gets, and the log.
put("settings.json", #"{"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-colemak"}, "henkan": {"profiles": [{"id": "ja", "name": "日本語", "layouts": ["ja-romaji"]}]}}, "harness": {"com.teammirae.bada.mac": {"keyboard": "jis-pc", "log": true, "shiny": 1}, "com.teammirae.bada.windows": {"anything": true}}}"#)
check(store.reload(), "a good file again")
check(store.table.bindings == KeyTable.preset(.jisPC) && store.harness.log && store.harness.keyboard == .jisPC, "the harness's own part names the keyboard")
check(store.problems.map { "\($0.path) \($0.code) \($0.level)" } == ["harness.com.teammirae.bada.mac.shiny unknown-field warning"], "a setting it doesn't know is a warning, and the file is taken")
check(store.profiles.map(\.id) == ["ja"] && store.muhenkanLayout == "en-colemak" && layoutInEffect() == "en-colemak", "…with the rest of it")
put("settings.json", #"{"format": "bada-settings/2", "harness": {"com.teammirae.bada.mac": {"keyboard": "ansi"}}}"#)
store.reload()
check(store.problems.map { "\($0.path) \($0.code) \($0.level)" } == ["harness.com.teammirae.bada.mac.keyboard bad-value error"] && store.profiles.map(\.id) == ["ja"], "a wrong value there refuses the file")
// With no engine part, the engine's defaults; with no keys, the preset of the keyboard macOS reports.
put("settings.json", #"{"format": "bada-settings/2"}"#)
store.reload()
check(store.problems.isEmpty && store.profiles.isEmpty && store.muhenkanLayout == nil && store.table.bindings == KeyTable.preset(.jisMac), "a file with only its format is a new install")
store.autoPreset = .noLanguageKeys
check(store.reload() && store.table.bindings == KeyTable.preset(.noLanguageKeys), "another keyboard, another preset")
store.autoPreset = .jisMac
store.reload()

// layouts/*.json: the user's layouts, and changed copies of stock ones.
let stock = JSON.object(try Data(contentsOf: root.appendingPathComponent("engine/layouts/ko-2-set.json")))!
var mine = stock
mine["id"] = "ko-mine"
mine["name"] = ["en": "Korean - mine"]
put("layouts/mine.json", JSON.text(mine)!)
check(store.reload(), "a layout file is taken")
check(store.layouts.contains { $0.id == "ko-mine" && !$0.stock }, "…as a custom layout")
check(!store.problems.contains(where: \.isError), "…with no error (\(store.problems.map { "\($0.file) \($0.path) \($0.code)" }))")
put("settings.json", #"{"format": "bada-settings/2", "engine": {"muhenkan": {"layout": "en-qwerty"}, "henkan": {"profiles": [{"id": "mine", "name": "내 것", "layouts": ["ko-mine"]}]}}}"#)
store.reload()
check(store.profiles.map(\.id) == ["mine"] && !store.problems.contains(where: \.isError), "a profile can use it")
do {
    let engine = Engine(session)!
    _ = engine.select(.henkan, profile: "mine")
    check(engine.key("KeyR").preedit.text == "ㄱ", "…and it types")
}
put("layouts/stock.json", JSON.text(stock)!)
store.reload()
check(store.layouts.contains { $0.id == "ko-2-set" && $0.stock && $0.modified }, "a file with a stock id replaces the stock layout, marked modified")
check(store.problems.contains { $0.file == "layouts/stock.json" && $0.code == "stock-modified" && $0.level == "warning" }, "…with the warning")
remove("layouts/stock.json")
store.reload()
check(store.layouts.contains { $0.id == "ko-2-set" && $0.stock && !$0.modified }, "removing the file brings the stock layout back")
check(!store.problems.contains { $0.file == "layouts/stock.json" }, "…and its problems go with it")
put("layouts/bad.json", #"{"format": "bada-layout/3", "id": "ko-bad", "name": {"en": "bad"}, "lang": "ko", "kind": "henkan", "read": "direct"}"#)
store.reload()
check(store.problems.contains { $0.file == "layouts/bad.json" && $0.isError } && !store.layouts.contains { $0.id == "ko-bad" }, "a layout with an error isn't loaded")
put("layouts/bad.json", "[1, 2")
store.reload()
check(store.problems.contains { $0.file == "layouts/bad.json" && $0.code == "bad-json" }, "a layout file that isn't JSON is a problem")
remove("layouts/bad.json")
// A layout that goes while a profile still uses it: the settings are read again, and say so.
remove("layouts/mine.json")
store.reload()
check(!store.layouts.contains { $0.id == "ko-mine" }, "a layout file that is gone takes its layout with it")
check(store.problems.contains { $0.file == "settings.json" && $0.code == "unknown-layout" }, "…and the settings that named it have a problem (\(store.problems.map { "\($0.file) \($0.path) \($0.code)" }))")

// dictionary.json
put("dictionary.json", #"{"format": "bada-dictionary", "version": 1, "positive": [{"word": "데이터센터", "count": 40}], "negative": [{"word": "감소합니다"}]}"#)
store.reload()
check(store.dictionary == ["positive": 1, "negative": 1], "a dictionary is loaded (\(store.dictionary))")
put("dictionary.json", #"{"format": "something-else", "version": 1}"#)
store.reload()
check(store.problems.contains { $0.file == "dictionary.json" && $0.isError } && store.dictionary == ["positive": 1, "negative": 1], "a dictionary of another format changes nothing")
remove("dictionary.json")
store.reload()
check(store.dictionary == ["positive": 0, "negative": 0] && !store.problems.contains { $0.file == "dictionary.json" }, "no file: no user dictionary")

// No settings file again: a new install.
remove("settings.json")
check(store.reload() && store.profiles.isEmpty && store.muhenkanLayout == nil && store.problems.isEmpty, "the settings file gone: a new install again")

// MARK: The first run (§8.3)

do {
    let data = try Onboarding.settings(muhenkan: "en-qwerty", henkan: "ja-romaji", name: "Japanese - Romaji", harnessID: harnessID, existing: nil)
    let file = JSON.object(data) ?? [:]
    check(file["format"] as? String == "bada-settings/2" && file["keys"] as? [String: Any] != nil
          && (file["harness"] as? [String: Any])?[harnessID] != nil, "the easy mode writes a whole settings file")
    try AtomicFile.write(data, to: temporary.appendingPathComponent("settings.json"))
    store.reload()
    check(store.muhenkanLayout == "en-qwerty" && store.profiles == [Profile(id: "main", name: "Japanese - Romaji", layouts: ["ja-romaji"])]
          && store.problems.isEmpty, "…that gives the muhenkan layout and one profile with one layout")
    // The key that opened the onboarding works from then on.
    let decision = store.table.decide(.key("Lang1", []), state: KeyState(mode: .muhenkan, profile: nil), profiles: store.profiles)
    check(decision.then == .select(.henkan, "main"), "かな then enters henkan")
    check(KeyTable.preset(.jisMac) == store.table.bindings, "…on the keyboard's preset")
    // On a file that is there already: everything else is kept, and the new profile gets a free id.
    let again = try Onboarding.settings(muhenkan: "en-dvorak", henkan: "ko-2-set", name: "한국어", harnessID: harnessID, existing: data)
    let second = JSON.object(again) ?? [:]
    let list = ((second["engine"] as? [String: Any])?["henkan"] as? [String: Any])?["profiles"] as? [[String: Any]] ?? []
    check(list.map { $0["id"] as? String } == ["main", "main-2"] && ((second["engine"] as? [String: Any])?["muhenkan"] as? [String: Any])?["layout"] as? String == "en-dvorak",
          "a second run adds a profile and keeps the first")
    var unreadable = false
    do { _ = try Onboarding.settings(muhenkan: "en-qwerty", henkan: "ja-romaji", name: "x", harnessID: harnessID, existing: Data("{ broken".utf8)) } catch { unreadable = true }
    check(unreadable, "a settings file that can't be read is left alone")
} catch {
    check(false, "the onboarding failed: \(error)")
}

// MARK: The published state (§6.7)

do {
    let typed = Reply(text: #"{"ok": true, "preedit": {"text": "한국어", "caret": 3, "segments": [{"len": 3, "kind": "live"}]}, "menu": {"rows": [{"text": "한국어", "entry": 0}, {"text": "韓國語", "entry": 1, "lang": "ko"}], "selected": 0, "strip": false}, "mode": "henkan", "profile": "work", "staged": "한국어"}"#)
    let keys = PublishedState.keys(accessibility: false, inputMonitoring: false)
    var harness = HarnessState(id: harnessID, version: "0.1.0", app: "com.apple.TextEdit", active: true,
                               available: keys.available, chords: keys.chords, missing: keys.missing,
                               problems: [Problem(file: "settings.json", path: "keys.cycle", code: "bad-value", message: "m", level: "error")])
    let state = PublishedState.build(mode: .henkan, profile: "work", reply: typed, harness: harness)
    check(state["format"] as? String == "bada-compatible-state/2" && state["mode"] as? String == "henkan" && state["profile"] as? String == "work", "the state's format, mode and profile")
    check(state["staged"] as? String == "한국어" && (state["preedit"] as? [String: Any])?["text"] as? String == "한국어"
          && (state["preedit"] as? [String: Any])?["caret"] as? Int == 3, "what is typed")
    let rows = (state["menu"] as? [String: Any])?["rows"] as? [[String: Any]] ?? []
    check(rows.count == 2 && rows[1]["lang"] as? String == "ko" && rows[0]["lang"] == nil && (state["menu"] as? [String: Any])?["selected"] as? Int == 0, "the menu's rows")
    let own = state["harness"] as? [String: Any] ?? [:]
    check(own["id"] as? String == harnessID && own["app"] as? String == "com.apple.TextEdit" && own["secure"] as? Bool == false && own["active"] as? Bool == true, "the harness's own fields")
    let published = own["keys"] as? [String: Any] ?? [:]
    check(published["available"] as? [String] == ["Lang1", "Lang2", "Backquote"] && published["chords"] as? Bool == false
          && published["missing"] as? [String] == ["Accessibility", "Input Monitoring"], "what it can detect with no permission")
    check((own["problems"] as? [[String: Any]])?.first?["path"] as? String == "keys.cycle", "the problems of the files it last read")
    check((own["caps_lock"] as? [String: Any])?["setting"] as? String == "respect" && (own["caps_lock"] as? [String: Any])?["rule"] as? String == "allcaps", "Caps Lock's settings")
    let all = PublishedState.keys(accessibility: true, inputMonitoring: true)
    check(all.available == ["Lang1", "Lang2", "Backquote", "NonConvert", "Convert", "KanaMode", "CapsLock"] && all.chords && all.missing.isEmpty, "…and with both")
    check(PublishedState.keys(accessibility: false, inputMonitoring: true).available == ["Lang1", "Lang2", "Backquote", "NonConvert", "Convert", "KanaMode"],
          "Caps Lock needs both permissions")
    // Nothing typed is published in a password field.
    harness.secure = true
    let secret = PublishedState.build(mode: .henkan, profile: "work", reply: typed, harness: harness)
    check(secret["staged"] as? String == "" && (secret["preedit"] as? [String: Any])?["text"] as? String == ""
          && ((secret["menu"] as? [String: Any])?["rows"] as? [Any])?.isEmpty == true, "in a password field nothing typed is published")
    check((secret["harness"] as? [String: Any])?["secure"] as? Bool == true && secret["mode"] as? String == "henkan", "…but the mode is, and that it is secure")
    check(!(JSON.text(secret) ?? "한국어").contains("한국어") && !(JSON.text(secret) ?? "韓").contains("韓"), "…nowhere in it")
    check(JSON.text(state) == JSON.text(PublishedState.build(mode: .henkan, profile: "work", reply: typed, harness: { var h = harness; h.secure = false; return h }())),
          "the same state is the same JSON, so it is published only when it changed")
    let before = PublishedState.build(mode: .muhenkan, profile: nil, reply: nil, harness: harness)
    check(before["profile"] is NSNull && before["staged"] as? String == "" && JSON.text(before) != nil, "a state before any reply")
}

// MARK: Watching the folder (§10.1)

do {
    var reloads = 0
    let watcher = FolderWatcher(temporary, ignoring: FolderWatcher.isNotSettings) { reloads += 1 }
    check(watcher != nil, "the folder is watched")
    func wait(_ seconds: Double, until done: () -> Bool = { false }) {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end && !done() { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.05)) }
    }
    // Changes from just before the watcher started may still arrive: let them, and count from here.
    wait(1.0)
    reloads = 0
    // What bada writes itself, the undo history and files on their way to a rename aren't settings.
    put("state.json", "{}")
    try? FileManager.default.createDirectory(at: temporary.appendingPathComponent("history"), withIntermediateDirectories: true)
    put("history/1.json", "{}")
    wait(1.0)
    check(reloads == 0, "the state bada writes itself, and the history, don't reload (\(reloads))")
    put("settings.json", #"{"format": "bada-settings/2"}"#)
    wait(5) { reloads > 0 }
    check(reloads == 1, "a change to settings.json reloads, once (\(reloads))")
    put("layouts/new.json", "{}")
    wait(5) { reloads > 1 }
    check(reloads == 2, "a change in layouts reloads (\(reloads))")
    watcher?.stop()
    check(FolderWatcher.isNotSettings("/x/bada/state.json") && FolderWatcher.isNotSettings("/x/bada/history/3.json")
          && FolderWatcher.isNotSettings("/x/bada/.settings.json.12.tmp") && !FolderWatcher.isNotSettings("/x/bada/settings.json")
          && !FolderWatcher.isNotSettings("/x/bada/layouts/mine.json") && !FolderWatcher.isNotSettings("/x/bada/dictionary.json"), "which files are settings")
}

// MARK: bada-tide (docs/bada-tide-v1.md): the translator, its templates and the deny list

do {
    let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../docs/schemas/examples/tide-translator.json").standardized
    let example = try! Data(contentsOf: root)
    let t = try! TideTranslator(json: example)
    check(t.id == "ikarosu" && t.match.reportSize == 64 && t.conflicts.first?.bundleIDs == ["jp.co.pfu.hhkb-keymap-tool"], "tide: the example translator reads")
    let hello = t.hello(size: 64)!
    check(hello.count == 64 && Array(hello[0..<7]) == [0xAA, 0xAA, 0xF1, 0x01, 0x02, 1, 0] && hello[7...].allSatisfy { $0 == 0 }, "tide: hello is the template, padded")
    let state = t.state(mode: .henkan, profile: 1, caps: true, take: ["KanaMode", "Lang1"], size: 64)!
    check(Array(state[0..<9]) == [0xAA, 0xAA, 0xF1, 0x02, 0x04, 1, 1, 1, 0x28], "tide: state carries mode, profile, caps and the take mask")
    check(t.state(mode: .muhenkan, profile: nil, caps: false, take: [], size: 64)![6] == 255, "tide: no profile is profile_none")
    var reply = [UInt8](repeating: 0, count: 64)
    reply[0...10] = [0x55, 0x55, 0xF1, 0, 0x01, 0x06, 1, 0, 0x11, 0xB8, 0x0B]
    check(t.read(reply) == .hello(status: "ok", features: ["keys", "take"], leaseMs: 3000), "tide: hello reply: status, features, lease")
    reply[3] = 4
    if case .hello(let status, _, _) = t.read(reply) { check(status == "blocked", "tide: hello reply status 4 is blocked") } else { check(false, "tide: hello reply status 4 is blocked") }
    var st = [UInt8](repeating: 0, count: 64)
    st[0...5] = [0x55, 0x55, 0xF1, 2, 0x02, 0x06]
    check(t.read(st) == .state(status: "no-lease"), "tide: state reply no-lease")
    var ev = [UInt8](repeating: 0, count: 64)
    ev[0...14] = [0x55, 0x55, 0xF1, 0, 0x10, 0x09, 0x88, 1, 0x22, 7, 1, 0, 0, 0, 1]
    check(t.read(ev) == .key(code: "KanaMode", down: true, mods: [.shift], seq: 7, dropped: true), "tide: a key event: code, edge, both shifts as shift, seq, dropped")
    ev[6] = 0x04
    check(t.read(ev) == .other, "tide: an event for a key that is no language key is ignored")
    check(t.read([UInt8](repeating: 0, count: 64)) == .other, "tide: an unrelated report is ignored")
    func with(_ edit: (inout [String: Any]) -> Void) -> Data {
        var o = try! JSONSerialization.jsonObject(with: example) as! [String: Any]
        edit(&o)
        return try! JSONSerialization.data(withJSONObject: o)
    }
    let denied = with { o in var s = o["state"] as! [String: Any]; s["send"] = "AA AA E0 {mode}"; o["state"] = s }
    check((try? TideTranslator(json: denied)) == nil && { do { _ = try TideTranslator(json: denied); return false } catch TideTranslator.Problem.denied("state") { return true } catch { return false } }(),
          "tide: a state template that begins like the PFU update request is denied")
    let via = with { o in var m = o["match"] as! [String: Any]; m["report_size"] = 32; o["match"] = m
                           var h = o["hello"] as! [String: Any]; h["send"] = "0B {major}"; o["hello"] = h }
    check({ do { _ = try TideTranslator(json: via); return false } catch TideTranslator.Problem.denied("hello") { return true } catch { return false } }(),
          "tide: a 32-byte hello starting 0B (VIA bootloader jump) is denied")
    let major2 = with { o in o["version"] = ["major": 2, "minor": 0] }
    check((try? TideTranslator(json: major2)) == nil, "tide: another tide major is not read")
    check(TideTemplate("AA {x:le24}") == nil && TideTemplate("AAA") == nil && TideTemplate("{x:be16} 01")?.encode(["x": 0x1234], size: 4) == [0x12, 0x34, 1, 0],
          "tide: templates: unknown widths and bad bytes refused, big-endian fields")
}

// MARK: harness settings: apps (issue #11)

do {
    let (settings, flaws, warnings) = HarnessSettings.read(["apps": ["com.ccpgames.eveonline": ["menu": false], "com.apple.TextEdit": ["menu": true]]], path: "harness.x")
    check(flaws.isEmpty && warnings.isEmpty, "apps: a valid list reads with no problem")
    check(!settings.showsMenu(in: "com.ccpgames.eveonline"), "apps: menu false draws no menu in that app")
    check(settings.showsMenu(in: "com.apple.TextEdit") && settings.showsMenu(in: "com.other.app") && settings.showsMenu(in: nil),
          "apps: every other app, and an unknown one, gets the menu")
    let bad = HarnessSettings.read(["apps": ["a.b": ["menu": "no"], "c.d": 3]], path: "harness.x")
    check(bad.flaws.map(\.path) == ["harness.x.apps.a.b.menu", "harness.x.apps.c.d"], "apps: a wrong value is an error, with its path")
    let unknown = HarnessSettings.read(["apps": ["a.b": ["colour": 1]]], path: "harness.x")
    check(unknown.flaws.isEmpty && unknown.warnings.map(\.path) == ["harness.x.apps.a.b.colour"], "apps: an unknown field is a warning")
    check(HarnessSettings.read(["apps": 1], path: "harness.x").flaws.first?.path == "harness.x.apps", "apps: not an object is an error")
}

try? FileManager.default.removeItem(at: temporary)
print("unit: \(checks) checks, \(failures) failed")
exit(failures == 0 ? 0 : 1)
