import Foundation

// Applying a reply (the standard, §6.3). The same steps for every reply, whatever the event; the
// text field is behind a protocol, so the input method and the tests share this one copy.

/// The text field a reply is applied to: a list of characters, a selection (a caret when empty) and
/// an optional marked range.
protocol TextTarget: AnyObject {
    /// Whether text is marked now.
    var hasMarked: Bool { get }
    /// Replaces a range with `text`; the caret goes after it and nothing is marked. The range is the
    /// selection if `replacingSelection` or nothing is marked, else the marked range.
    func commit(_ text: String, replacingSelection: Bool)
    /// Replaces the marked range if any, else the selection, with the preedit; marks it and puts the
    /// caret where the preedit says.
    func mark(_ preedit: Preedit)
    /// Deletes the marked text.
    func unmark()
    /// Shows the menu; one with no rows closes it. `rtl`: drawn right to left.
    func show(_ menu: Menu, rtl: Bool)
    /// The selected text: "" if nothing is selected or something is marked.
    func selection() -> String
    /// The text around the caret, at most this many characters each side.
    func around(before: Int, after: Int) -> (before: String, after: String)
}

enum ReplyApplier {
    /// Applies `reply`. If it asks for text (`ask`), `answer` is called once with the `text` event
    /// and the reply to that is applied too, never deeper.
    static func apply(_ reply: Reply, to target: TextTarget, answer: (([String: Any]) -> Reply)?) {
        apply(reply, to: target, answer: answer, isAnswer: false)
    }

    private static func apply(_ reply: Reply, to target: TextTarget, answer: (([String: Any]) -> Reply)?, isAnswer: Bool) {
        guard reply.ok else {
            // Nothing changed: the field keeps what the last reply showed. After a panic the engine
            // has reset itself, so what shows is no longer staged.
            if reply.error?.code == "panic" {
                if target.hasMarked { target.unmark() }
                target.show(Menu(), rtl: false)
            }
            return
        }
        if !reply.commit.isEmpty {
            target.commit(reply.commit, replacingSelection: reply.replacesSelection)
        }
        if !reply.preedit.text.isEmpty {
            target.mark(reply.preedit)
        } else if target.hasMarked {
            target.unmark()
        }
        target.show(reply.menu, rtl: reply.rtl)
        if let ask = reply.ask, !isAnswer, let answer {
            apply(answer(textEvent(for: ask, from: target)), to: target, answer: answer, isAnswer: true)
        }
    }

    /// The `text` event that answers an `ask` (§5).
    static func textEvent(for ask: Ask, from target: TextTarget) -> [String: Any] {
        switch ask {
        case .selection:
            return ["type": "text", "selection": target.selection()]
        case .around(let before, let after):
            let text = target.around(before: before, after: after)
            return ["type": "text", "before": text.before, "after": text.after]
        }
    }
}

/// What a key the engine didn't consume types (§6.4), and who types it.
enum Typing {
    enum Verdict: Equatable {
        /// The app gets the key: the OS's layout types the same text, or the engine has no say.
        case app
        /// The harness inserts this text itself and keeps the key from the app.
        case insert(String)
    }

    /// `types`: the reply's. `osTypes`: what the OS's keyboard layout makes of the key, looked at
    /// only to see whether it is the same text. `capsLock`: the lock is on, so letters are capitals.
    static func resolve(types: String?, osTypes: String?, capsLock: Bool) -> Verdict {
        guard let types else { return .app }
        let text = capsLock ? capitals(types) : types
        return osTypes == text ? .app : .insert(text)
    }

    /// The letters in capitals, as the lock types them; everything else as it is.
    static func capitals(_ text: String) -> String {
        text.map { $0.isLetter && $0.isLowercase ? $0.uppercased() : String($0) }.joined()
    }
}
