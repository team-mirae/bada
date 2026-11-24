import Foundation

// Replays every case and every sequence of docs/cases/key-table.json (bada-key-cases/1) on KeyTable
// and KeySequencer (the standard, §9.6).
//   key-table-cases <key-table.json>

let arguments = CommandLine.arguments
guard arguments.count == 2, let data = FileManager.default.contents(atPath: arguments[1]),
      let file = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
    print("usage: key-table-cases <docs/cases/key-table.json>")
    exit(2)
}
guard file["format"] as? String == "bada-key-cases/1" else {
    print("not a bada-key-cases/1 file")
    exit(2)
}

var failures = 0
var passed = 0
func fail(_ name: String, _ why: String) {
    failures += 1
    print("FAIL \(name): \(why)")
}

// The named profile lists and tables.
var profileLists: [String: [Profile]] = [:]
for (name, list) in file["profiles"] as? [String: [[String: Any]]] ?? [:] {
    profileLists[name] = list.map { Profile(id: $0["id"] as? String ?? "", name: $0["id"] as? String ?? "", layouts: $0["layouts"] as? [String] ?? []) }
}
var tables: [String: KeyTable] = [:]
for (name, raw) in file["tables"] as? [String: Any] ?? [:] {
    let (table, flaws) = KeyTable.read(raw, preset: .jisMac)
    if !flaws.isEmpty { fail("table \(name)", "\(flaws)") }
    tables[name] = table
}

// The presets of §9.5 are the cases' own tables of the same keyboards.
for (name, preset) in [("jis-mac", KeyTable.Preset.jisMac), ("jis-pc", .jisPC), ("no-language-keys", .noLanguageKeys)] {
    var want = KeyTable()
    want.bindings = KeyTable.preset(preset)
    if tables[name] == want { passed += 1 } else { fail("preset \(name)", "the preset differs from the cases' table") }
    // A table with no bindings gets the preset.
    if KeyTable.read([String: Any](), preset: preset).table == want { passed += 1 } else { fail("preset \(name)", "an empty keys part doesn't give the preset") }
}

func state(_ json: [String: Any]) -> KeyState {
    KeyState(mode: Mode(rawValue: json["mode"] as? String ?? "") ?? .muhenkan,
             profile: json["profile"] as? String, previous: json["previous"] as? String,
             menuRows: json["menu_rows"] as? Int ?? 0, staged: json["staged"] as? String ?? "",
             capsLock: json["caps_lock"] as? Bool ?? false)
}

func word(_ outcome: Outcome) -> String {
    switch outcome {
    case .select: return "select"
    case .action: return "action"
    case .key: return "key"
    case .type: return "type"
    case .pass: return "pass"
    case .swallow: return "swallow"
    case .onboarding: return "onboarding"
    }
}

/// What differs between a decision and what a case expects; nil when nothing does.
func difference(_ decision: Decision, _ expect: [String: Any], before: KeyState) -> String? {
    var wrong: [String] = []
    let then = expect["then"] as? String ?? ""
    if word(decision.then) != then { wrong.append("then \(word(decision.then)), want \(then)") }
    if decision.binding != expect["binding"] as? Int { wrong.append("binding \(String(describing: decision.binding)), want \(String(describing: expect["binding"]))") }
    if decision.ran != expect["ran"] as? String { wrong.append("ran \(decision.ran ?? "null"), want \(expect["ran"] as? String ?? "null")") }
    switch decision.then {
    case .select(let mode, let profile):
        if mode.rawValue != expect["mode"] as? String { wrong.append("mode \(mode.rawValue), want \(String(describing: expect["mode"]))") }
        if profile != expect["profile"] as? String { wrong.append("profile \(profile ?? "null"), want \(String(describing: expect["profile"]))") }
    case .action(let name):
        if name != expect["name"] as? String { wrong.append("name \(name), want \(String(describing: expect["name"]))") }
    case .type(let text):
        if text != expect["text"] as? String { wrong.append("text \(text), want \(String(describing: expect["text"]))") }
    default: break
    }
    if let lock = expect["caps_lock"] as? Bool, (decision.capsLock ?? before.capsLock) != lock {
        wrong.append("caps_lock \(decision.capsLock ?? before.capsLock), want \(lock)")
    }
    return wrong.isEmpty ? nil : wrong.joined(separator: "; ")
}

let cases = file["cases"] as? [[String: Any]] ?? []
for item in cases {
    let name = item["name"] as? String ?? "?"
    guard let table = tables[item["table"] as? String ?? ""], let profiles = profileLists[item["profiles"] as? String ?? ""],
          let press = Chord(item["press"] as? String ?? ""), let expect = item["expect"] as? [String: Any] else {
        fail(name, "the case can't be read")
        continue
    }
    let before = state(item["state"] as? [String: Any] ?? [:])
    let decision = table.decide(press, state: before, profiles: profiles)
    if let wrong = difference(decision, expect, before: before) { fail(name, wrong) } else { passed += 1 }
}

let sequences = file["sequences"] as? [[String: Any]] ?? []
for item in sequences {
    let name = item["name"] as? String ?? "?"
    guard let table = tables[item["table"] as? String ?? ""], let profiles = profileLists[item["profiles"] as? String ?? ""],
          let keys = item["keys"] as? [String], let expect = item["expect"] as? [[String: Any]] else {
        fail(name, "the sequence can't be read")
        continue
    }
    var now = state(item["state"] as? [String: Any] ?? [:])
    var sequencer = KeySequencer()
    var happened: [(at: Int, decision: Decision, before: KeyState)] = []
    for (at, key) in keys.enumerated() {
        let parts = key.split(separator: ":").map(String.init)
        // A key waits only for a pair binding that holds when it is pressed.
        sequencer.pairs = table.pairs(holding: now)
        let presses = parts[0] == "down" ? sequencer.down(parts[1]).all : sequencer.up(parts[1])
        for press in presses {
            let decision = table.decide(press, state: now, profiles: profiles)
            happened.append((at, decision, now))
            now.apply(decision)
        }
    }
    var wrong: [String] = []
    if happened.count != expect.count { wrong.append("\(happened.count) things happened, want \(expect.count)") }
    for (got, want) in zip(happened, expect) {
        if got.at != want["at"] as? Int { wrong.append("at \(got.at), want \(String(describing: want["at"]))") }
        if let w = difference(got.decision, want, before: got.before) { wrong.append(w) }
    }
    if wrong.isEmpty { passed += 1 } else { fail(name, wrong.joined(separator: "; ")) }
}

print("key table: \(cases.count) cases, \(sequences.count) sequences, 6 preset checks: \(passed) passed, \(failures) failed")
exit(failures == 0 ? 0 : 1)
