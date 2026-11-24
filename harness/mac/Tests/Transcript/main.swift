import Foundation

// Replays conformance transcripts (tools/conformance/README.md, bada-transcript/1) through the
// harness's own code: the engine wrapper (Engine.swift) and the reply applier (ReplyApplier.swift),
// on a text field in memory. It checks two things on every line: the engine's reply against the
// transcript's `expect`, and the text field against what the standard's §6.3 says it must hold.
//   transcript <file.jsonl>…

var failures = 0
var checks = 0
func fail(_ file: String, _ line: Int, _ why: String) {
    failures += 1
    print("FAIL \(file):\(line): \(why)")
}

/// `expect` names only the fields that matter: an object matches when every field it has matches,
/// an array must match item for item, and null also matches a missing field.
func matches(_ expect: Any, _ got: Any?) -> Bool {
    if expect is NSNull { return got == nil || got is NSNull }
    guard let got else { return false }
    if let expect = expect as? [String: Any] {
        guard let got = got as? [String: Any] else { return false }
        return expect.allSatisfy { matches($0.value, got[$0.key]) }
    }
    if let expect = expect as? [Any] {
        guard let got = got as? [Any], got.count == expect.count else { return false }
        return zip(expect, got).allSatisfy { matches($0, $1) }
    }
    if let expect = expect as? NSNumber, let got = got as? NSNumber {
        // A JSON true is not a 1.
        let isBool = CFGetTypeID(expect) == CFBooleanGetTypeID()
        guard isBool == (CFGetTypeID(got) == CFBooleanGetTypeID()) else { return false }
        return expect == got
    }
    if let expect = expect as? String, let got = got as? String { return expect == got }
    return false
}

func same(_ a: Any, _ b: Any) -> Bool { matches(a, b) && matches(b, a) }

/// One text field of the transcript: an engine, the field it types in, and what the field must hold
/// by the simplest reading of the replies: every commit and every key the app types, one after the other.
final class Field {
    let engine: Engine
    let model = TextFieldModel()
    var document = ""
    /// Text typed and selected for an `ask` to find, not yet replaced.
    var seeded = ""
    init(_ engine: Engine) { self.engine = engine }
}

/// What the app types for a key the engine passed on with no `types`: only what every app agrees on.
func appTypes(_ event: [String: Any]) -> String {
    let plain = event["ctrl"] as? Bool != true && event["meta"] as? Bool != true && event["alt"] as? Bool != true
    guard plain else { return "" }
    switch event["key"] as? String {
    case "Space": return " "
    case "Enter": return "\n"
    default: return ""
    }
}

func replay(_ path: String) {
    let file = (path as NSString).lastPathComponent
    guard let content = try? String(contentsOfFile: path, encoding: .utf8) else {
        fail(file, 0, "can't be read")
        return
    }
    let lines = content.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
    var session: EngineSession?
    var fields: [String: Field] = [:]
    var answered: Set<Int> = []   // lines already sent as the answer to an ask
    var sent = 0
    var ended: [String] = []   // what each field held when it was freed

    for (index, text) in lines.enumerated() where !text.trimmingCharacters(in: .whitespaces).isEmpty {
        let number = index + 1
        if answered.contains(index) { continue }
        guard let line = JSON.object(Data(text.utf8)) else {
            fail(file, number, "the line isn't a JSON object")
            continue
        }
        if line["transcript"] != nil || line["note"] != nil { continue }

        if let expect = line["describe"] {
            checks += 1
            do {
                let description = try EngineDescription.linked()
                if !matches(expect, description.json) { fail(file, number, "describe doesn't match") }
            } catch {
                fail(file, number, "the engine is refused: \(error)")
            }
            continue
        }
        if let initial = line["init"] {
            fields = [:]   // the engines first, then their session
            session = EngineSession(data: EngineData(), initJSON: JSON.text(initial) ?? "{}")
            checks += 1
            if session == nil { fail(file, number, "no session") }
            continue
        }
        if let name = line["new"] as? String {
            checks += 1
            if let session, let engine = Engine(session) { fields[name] = Field(engine) } else { fail(file, number, "no engine") }
            continue
        }
        if let name = line["free"] as? String {
            if let field = fields[name] { ended.append("\(name): \(JSON.text([field.model.string]) ?? "")") }
            fields[name] = nil
            continue
        }

        // A send: to a field, or to the session alone.
        let raw = line["send_raw"] as? String
        let event = line["send"] as? [String: Any]
        guard raw != nil || event != nil else {
            fail(file, number, "a line the replay doesn't know")
            continue
        }
        guard let session else {
            fail(file, number, "a send before init")
            continue
        }
        sent += 1
        let json = raw ?? JSON.text(event!) ?? "{}"
        guard let name = line["to"] as? String else {
            let reply = session.send(json: json)
            checks += 1
            if let expect = line["expect"], !matches(expect, reply.json) { fail(file, number, "the reply doesn't match: \(reply.text)") }
            continue
        }
        guard let field = fields[name] else {
            fail(file, number, "no engine named \(name)")
            continue
        }
        let model = field.model
        let reply = field.engine.send(json: json)
        checks += 1
        if let expect = line["expect"], !matches(expect, reply.json) { fail(file, number, "the reply doesn't match: \(reply.text)") }

        // If the reply asks for the selection and the transcript answers on its next line, the text
        // of that answer is what the user has selected.
        var next: (index: Int, line: [String: Any])?
        if reply.ok, reply.ask != nil {
            if let at = lines.indices.dropFirst(index + 1).first(where: { !lines[$0].trimmingCharacters(in: .whitespaces).isEmpty }),
               let candidate = JSON.object(Data(lines[at].utf8)), candidate["to"] as? String == name,
               let answer = candidate["send"] as? [String: Any], answer["type"] as? String == "text" {
                next = (at, candidate)
                if let selection = answer["selection"] as? String, !model.hasMarked {
                    model.insertAndSelect(selection)
                    field.seeded = selection
                }
            }
        }

        let before = (model.string, model.caret, model.hasMarked)
        var answers = 0
        ReplyApplier.apply(reply, to: model) { textEvent in
            answers += 1
            if let next {
                answered.insert(next.index)
                sent += 1
                checks += 1
                if !same(textEvent, next.line["send"] as Any) {
                    fail(file, next.index + 1, "the applier answers \(JSON.text(textEvent) ?? "?"), the transcript \(lines[next.index])")
                }
            }
            let answer = field.engine.send(textEvent)
            if let next, let expect = next.line["expect"], !matches(expect, answer.json) {
                fail(file, next.index + 1, "the reply doesn't match: \(answer.text)")
            }
            // The simplest reading: a commit on the selection replaces what was selected.
            if answer.ok, !answer.commit.isEmpty {
                if answer.replacesSelection, field.document.hasSuffix(field.seeded) { field.document.removeLast(field.seeded.count) }
                field.document += answer.commit
            }
            field.seeded = ""
            return answer
        }

        checks += 1
        if reply.ok {
            if reply.ask != nil && answers != 1 { fail(file, number, "an ask was answered \(answers) times") }
            if reply.ask == nil && answers != 0 { fail(file, number, "a text event nobody asked for") }
            field.document += field.seeded   // typed by the user, still there if nothing replaced it
            field.seeded = ""
            field.document += reply.commit
            // Then the app gets a key the engine didn't consume (§6.4).
            if event?["type"] as? String == "key", !reply.consumed {
                switch Typing.resolve(types: reply.types, osTypes: nil, capsLock: false) {
                case .insert(let typed):
                    model.commit(typed, replacingSelection: false)
                    field.document += typed
                case .app:
                    let typed = appTypes(event!)
                    if !typed.isEmpty {
                        model.commit(typed, replacingSelection: false)
                        field.document += typed
                    }
                }
            }
            // What the field must hold now. With an ask, the answer's reply is what shows.
            if reply.ask == nil {
                if model.markedString != reply.preedit.text { fail(file, number, "marked \"\(model.markedString)\", the preedit is \"\(reply.preedit.text)\"") }
                if model.menu != reply.menu { fail(file, number, "the menu shown isn't the reply's") }
                let runs = reply.preedit.runs
                if runs.reduce(0, { $0 + $1.range.length }) != reply.preedit.text.utf16.count { fail(file, number, "the preedit's runs don't cover it") }
            }
            if model.settled != field.document { fail(file, number, "the field holds \"\(model.settled)\", want \"\(field.document)\"") }
            let settledCount = field.document.unicodeScalars.count
            let caret = model.hasMarked ? settledCount + (reply.ask == nil ? reply.preedit.caret : model.caret - settledCount) : settledCount
            if model.caret != caret { fail(file, number, "the caret is at \(model.caret), want \(caret)") }
        } else {
            // Nothing changed: the field keeps what the last reply showed.
            if answers != 0 { fail(file, number, "a refused event was answered") }
            if (model.string, model.caret, model.hasMarked) != before { fail(file, number, "a refused event changed the field") }
        }
    }
    print("\(file): \(sent) events sent; fields at the end: \(ended.joined(separator: ", "))")
}

let paths = Array(CommandLine.arguments.dropFirst())
guard !paths.isEmpty else {
    print("usage: transcript <file.jsonl>…")
    exit(2)
}
// Fields are freed with the transcript's own `free` lines; whatever is left goes with `replay`.
for path in paths { replay(path) }
print("transcripts: \(paths.count) files, \(checks) checks, \(failures) failed")
exit(failures == 0 ? 0 : 1)
