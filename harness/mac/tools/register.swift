// Registers bada with Text Input Sources so it shows in System Settings without logging out.
// Then both modes are enabled, always: bada has exactly two (the standard, §8.1). Modes already on
// are left alone (turning them off and on again left only one of the two on).
// Usage: swift register.swift <Bada.app>
import Carbon
import Foundation

let path = CommandLine.arguments[1]
let bundleID = Bundle(path: path)?.bundleIdentifier ?? "com.teammirae.inputmethod.Bada"
let modes = [bundleID + ".Muhenkan", bundleID + ".Henkan"]

func sources(enabledOnly: Bool) -> [TISInputSource] {
    let query = [kTISPropertyBundleID as String: bundleID] as CFDictionary
    return TISCreateInputSourceList(query, !enabledOnly)?.takeRetainedValue() as? [TISInputSource] ?? []
}
func id(_ s: TISInputSource) -> String {
    TISGetInputSourceProperty(s, kTISPropertyInputSourceID).map { Unmanaged<CFString>.fromOpaque($0).takeUnretainedValue() as String } ?? ""
}
func settle(_ seconds: Double) { CFRunLoopRunInMode(.defaultMode, seconds, false) }

guard TISRegisterInputSource(URL(fileURLWithPath: path) as CFURL) == noErr else { print("TISRegisterInputSource failed"); exit(1) }
settle(1)
let all = sources(enabledOnly: false).filter { modes.contains(id($0)) }
// No off-and-on cycle for modes already enabled: with two modes, macOS then kept only one.
// One mode at a time, each given time to land: enabled back to back, macOS kept only one of them.
for _ in 0..<5 {
    let on = Set(sources(enabledOnly: true).map(id))
    let missing = all.filter { !on.contains(id($0)) }
    if missing.isEmpty { break }
    for source in missing {
        TISEnableInputSource(source)
        settle(1.5)
    }
}
// The keylayouts (the Keyboard Viewer's legends, hidden): macOS often lists them only after a second
// registration of a freshly installed bundle.
let keylayout = bundleID + ".keylayout.en-qwerty"
for _ in 0..<4 where !sources(enabledOnly: false).map(id).contains(keylayout) {
    TISRegisterInputSource(URL(fileURLWithPath: path) as CFURL)
    settle(1.5)
}
if !sources(enabledOnly: false).map(id).contains(keylayout) {
    print("The keylayouts aren't listed yet: the Keyboard Viewer shows macOS's own keyboard until they are (log out and in).")
}
let now = Set(sources(enabledOnly: true).map(id)).intersection(modes)
print(now.isEmpty
    ? "Registered. Add bada (muhenkan) under English and bada (henkan) under Japanese in System Settings → Keyboard → Input Sources (log out and in if they aren't listed)."
    : "Registered; enabled: \(now.sorted().joined(separator: ", ")).")
