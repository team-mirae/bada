import Foundation

/// A text field in memory, as the standard's §6.3 has it: a list of characters, a selection (a caret
/// when empty) and an optional marked range. Positions are in Unicode scalar values.
final class TextFieldModel: TextTarget {
    private(set) var text: [Unicode.Scalar] = []
    private(set) var selected: Range<Int> = 0..<0
    private(set) var marked: Range<Int>?
    private(set) var menu = Menu()
    private(set) var rtl = false
    /// How many times each thing was done, for the tests.
    private(set) var commits = 0, marks = 0, unmarks = 0, shows = 0, asks = 0

    var hasMarked: Bool { marked != nil }
    var caret: Int { selected.lowerBound }
    var string: String { String(String.UnicodeScalarView(text)) }
    var markedString: String { marked.map { String(String.UnicodeScalarView(text[$0])) } ?? "" }
    /// The text with the marked part left out: what is really in the document.
    var settled: String {
        var copy = text
        if let marked { copy.removeSubrange(marked) }
        return String(String.UnicodeScalarView(copy))
    }

    private func replace(_ range: Range<Int>, with string: String) -> Range<Int> {
        let scalars = Array(string.unicodeScalars)
        text.replaceSubrange(range, with: scalars)
        return range.lowerBound..<(range.lowerBound + scalars.count)
    }

    func commit(_ string: String, replacingSelection: Bool) {
        commits += 1
        // The selection if asked for or nothing is marked, else the marked range.
        let put = replace(replacingSelection ? selected : (marked ?? selected), with: string)
        selected = put.upperBound..<put.upperBound
        marked = nil
    }

    func mark(_ preedit: Preedit) {
        marks += 1
        let put = replace(marked ?? selected, with: preedit.text)
        marked = put
        let caret = put.lowerBound + min(preedit.caret, put.count)
        selected = caret..<caret
    }

    func unmark() {
        unmarks += 1
        guard let marked else { return }
        text.removeSubrange(marked)
        selected = marked.lowerBound..<marked.lowerBound
        self.marked = nil
    }

    func show(_ menu: Menu, rtl: Bool) {
        shows += 1
        self.menu = menu
        self.rtl = rtl
    }

    func selection() -> String {
        asks += 1
        guard marked == nil else { return "" }
        return String(String.UnicodeScalarView(text[selected]))
    }

    func around(before: Int, after: Int) -> (before: String, after: String) {
        asks += 1
        // With text marked or selected: around that text (§5).
        let middle = marked ?? selected
        let head = text[max(0, middle.lowerBound - before)..<middle.lowerBound]
        let tail = text[middle.upperBound..<min(text.count, middle.upperBound + after)]
        return (String(String.UnicodeScalarView(head)), String(String.UnicodeScalarView(tail)))
    }

    // What the user and the app do, outside of replies.

    /// Types text at the caret and selects it.
    func insertAndSelect(_ string: String) {
        let put = replace(selected, with: string)
        selected = put
    }

    /// Puts the caret, or a selection, somewhere.
    func select(_ range: Range<Int>) {
        selected = range
    }
}
