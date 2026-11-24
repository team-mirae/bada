// Writes the keyboard layouts the input method shows (overrideKeyboard), so the Keyboard Viewer
// shows what bada types: one for each stock layout that types (not the searches), named by the
// layout's id. Each is macOS's ABC layout as a JIS keyboard has it, with the layout's own
// characters on the plain, ⇧ and Caps Lock layers. Every other layer (⌥, ⌘, ⌃) is ABC's, so
// shortcuts are unchanged.
//
// The characters are asked of the engine itself, key by key: no table of them lives here. A layout
// that types directly says what each key types (`types`); one that composes shows it (the preedit).
//
//   (Makefile) build/make-keylayout build/keylayouts
// It also writes KLInfo.plist there: the Info.plist entries that hide the layouts from the list of
// input sources.
import Carbon
import Foundation

let args = CommandLine.arguments
guard args.count == 2 else {
    FileHandle.standardError.write("usage: make-keylayout outdir/\n".data(using: .utf8)!)
    exit(2)
}
let out = args[1]

/// The 48 typing keys (the standard, §4): the keys a layout types on.
let typingKeys: Set<String> = Set(
    ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Digit0", "Minus", "Equal", "IntlYen",
     "BracketLeft", "BracketRight", "Semicolon", "Quote", "Backslash", "Comma", "Period", "Slash", "IntlRo"]
    + "QWERTYUIOPASDFGHJKLZXCVBNM".map { "Key\($0)" })

let description: EngineDescription
do { description = try EngineDescription.linked() } catch {
    FileHandle.standardError.write("make-keylayout: the engine is refused: \(error)\n".data(using: .utf8)!)
    exit(1)
}

/// What each typing key shows on a layout: [plain, ⇧].
func legends(_ layout: LayoutInfo) -> [String: [String]] {
    let direct = layout.kind == "muhenkan"
    let settings: [String: Any] = direct
        ? ["muhenkan": ["layout": layout.id]]
        : ["henkan": ["profiles": [["id": "only", "name": "only", "layouts": [layout.id]]]]]
    guard let session = EngineSession(data: EngineData(), harness: "com.teammirae.bada.mac", version: description.version, features: []),
          session.send(["type": "settings", "set": settings]).ok, let engine = Engine(session),
          engine.select(direct ? .muhenkan : .henkan, profile: direct ? nil : "only").ok else {
        FileHandle.standardError.write("make-keylayout: \(layout.id) can't be set up\n".data(using: .utf8)!)
        exit(1)
    }
    var legends: [String: [String]] = [:]
    for key in typingKeys {
        legends[key] = [Mods(), Mods.shift].map { mods in
            let reply = engine.key(key, mods: mods)
            // What the key puts on the screen: committed at once, or shown as it composes; a key
            // passed on types what the engine says it types.
            let shown = reply.commit + reply.preedit.text
            _ = engine.reset()
            return shown.isEmpty ? reply.types ?? "" : shown
        }
    }
    return legends
}

// ABC, as a JIS keyboard types it.
let filter = [kTISPropertyInputSourceID as String: "com.apple.keylayout.ABC"] as CFDictionary
guard let abc = (TISCreateInputSourceList(filter, true)?.takeRetainedValue() as? [TISInputSource])?.first,
      let property = TISGetInputSourceProperty(abc, kTISPropertyUnicodeKeyLayoutData) else {
    FileHandle.standardError.write("make-keylayout: macOS's ABC layout can't be read\n".data(using: .utf8)!)
    exit(1)
}
let data = Unmanaged<CFData>.fromOpaque(property).takeUnretainedValue()
let layoutData = unsafeBitCast(CFDataGetBytePtr(data), to: UnsafePointer<UCKeyboardLayout>.self)
/// A keyboard type macOS takes for a JIS board: the one in use if it is one, else a known one.
let jisType = [Int(LMGetKbdType()), 18, 42].first { KBGetLayoutType(Int16($0)) == kKeyboardJIS } ?? 18
func abcChars(_ code: Int, _ mods: Int) -> String {
    var dead: UInt32 = 0, length = 0
    var buffer = [UniChar](repeating: 0, count: 8)
    UCKeyTranslate(layoutData, UInt16(code), UInt16(kUCKeyActionDisplay), UInt32((mods >> 8) & 0xFF), UInt32(jisType),
                   OptionBits(kUCKeyTranslateNoDeadKeysBit), &dead, buffer.count, &length, &buffer)
    return String(utf16CodeUnits: buffer, count: length)
}

/// The layers: its modifiers, the Carbon modifiers to translate ABC with, and which of the layout's
/// own layers it takes, if any: 0 plain, 1 ⇧, 2 Caps Lock. Without a Caps Lock layer of its own a
/// keylayout's Caps Lock types its plain layer.
let maps: [(String, Int, Int?)] = [
    ("<modifier keys=\"\"/>", 0, 0),
    ("<modifier keys=\"anyShift caps?\"/>", shiftKey, 1),
    ("<modifier keys=\"caps\"/>", alphaLock, 2),
    ("<modifier keys=\"caps? anyOption\"/>", optionKey, nil),
    ("<modifier keys=\"anyShift caps? anyOption\"/>", shiftKey | optionKey, nil),
    ("<modifier keys=\"anyShift? caps? anyOption? command\"/>", cmdKey, nil),
    ("<modifier keys=\"anyShift? caps? anyOption? command? anyControl\"/>", controlKey, nil),
]
func xml(_ s: String) -> String {
    s.unicodeScalars.map { u in
        u.value < 0x20 || u.value == 0x7F || "\"&'<>".unicodeScalars.contains(u) ? String(format: "&#x%04X;", u.value) : String(u)
    }.joined()
}

/// Letters in capitals, as the lock types them.
func capitals(_ text: String) -> String {
    text.map { $0.isLetter && $0.isLowercase ? $0.uppercased() : String($0) }.joined()
}

try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
var info = ""
let typing = description.layouts.filter { $0.read != "search" }
for (index, layout) in typing.enumerated() {
    let own = legends(layout)
    // Bound to the lock, a layout that types directly types capitals; one that composes ignores it.
    func legend(_ code: Int, _ layer: Int) -> String? {
        guard let name = keyNames[code], let pair = own[name] else { return nil }
        if layer == 2 { return layout.kind == "muhenkan" ? capitals(pair[0]) : pair[0] }
        return pair[layer]
    }
    // Ids of their own, below the ones bada ime's two layouts took.
    var text = """
    <?xml version="1.1" encoding="UTF-8"?>
    <!DOCTYPE keyboard SYSTEM "file://localhost/System/Library/DTDs/KeyboardLayout.dtd">
    <!-- Written by harness/mac/tools/make-keylayout.swift from the engine's \(layout.id), for the Keyboard Viewer. -->
    <keyboard group="126" id="\(-20530 - index)" name="\(layout.id)" maxout="\(max(2, own.values.flatMap { $0 }.map(\.utf16.count).max() ?? 1))">
        <layouts>
            <layout first="0" last="255" mapSet="all" modifiers="mods"/>
        </layouts>
        <modifierMap id="mods" defaultIndex="0">

    """
    for (i, (modifier, _, _)) in maps.enumerated() { text += "        <keyMapSelect mapIndex=\"\(i)\">\(modifier)</keyMapSelect>\n" }
    text += "    </modifierMap>\n    <keyMapSet id=\"all\">\n"
    for (i, (_, mods, layer)) in maps.enumerated() {
        text += "        <keyMap index=\"\(i)\">\n"
        for code in 0..<128 {
            var s = abcChars(code, mods)
            if let layer, let changed = legend(code, layer) { s = changed }
            if !s.isEmpty { text += "            <key code=\"\(code)\" output=\"\(xml(s))\"/>\n" }
        }
        text += "        </keyMap>\n"
    }
    text += "    </keyMapSet>\n</keyboard>\n"
    try text.write(toFile: out + "/\(layout.id).keylayout", atomically: true, encoding: .utf8)
    info += "\t<key>KLInfo_\(layout.id)</key>\n\t<dict>\n\t\t<key>ISOCode</key>\n\t\t<string>\(layout.lang)</string>\n\t\t<key>TSMInvisibleKey</key>\n\t\t<true/>\n\t</dict>\n"
}
try info.write(toFile: out + "/KLInfo.plist", atomically: true, encoding: .utf8)
print("make-keylayout: \(typing.map(\.id).joined(separator: " "))")
