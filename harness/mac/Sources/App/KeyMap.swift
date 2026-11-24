import Carbon.HIToolbox

/// macOS key codes → W3C `KeyboardEvent.code` (the standard, §4), as a JIS keyboard sends them.
let keyNames: [Int: String] = [
    kVK_ANSI_Grave: "Backquote",
    kVK_ANSI_1: "Digit1", kVK_ANSI_2: "Digit2", kVK_ANSI_3: "Digit3", kVK_ANSI_4: "Digit4", kVK_ANSI_5: "Digit5",
    kVK_ANSI_6: "Digit6", kVK_ANSI_7: "Digit7", kVK_ANSI_8: "Digit8", kVK_ANSI_9: "Digit9", kVK_ANSI_0: "Digit0",
    kVK_ANSI_Minus: "Minus", kVK_ANSI_Equal: "Equal", kVK_JIS_Yen: "IntlYen",
    kVK_ANSI_Q: "KeyQ", kVK_ANSI_W: "KeyW", kVK_ANSI_E: "KeyE", kVK_ANSI_R: "KeyR", kVK_ANSI_T: "KeyT",
    kVK_ANSI_Y: "KeyY", kVK_ANSI_U: "KeyU", kVK_ANSI_I: "KeyI", kVK_ANSI_O: "KeyO", kVK_ANSI_P: "KeyP",
    kVK_ANSI_LeftBracket: "BracketLeft", kVK_ANSI_RightBracket: "BracketRight",
    kVK_ANSI_A: "KeyA", kVK_ANSI_S: "KeyS", kVK_ANSI_D: "KeyD", kVK_ANSI_F: "KeyF", kVK_ANSI_G: "KeyG",
    kVK_ANSI_H: "KeyH", kVK_ANSI_J: "KeyJ", kVK_ANSI_K: "KeyK", kVK_ANSI_L: "KeyL",
    kVK_ANSI_Semicolon: "Semicolon", kVK_ANSI_Quote: "Quote", kVK_ANSI_Backslash: "Backslash",
    kVK_ANSI_Z: "KeyZ", kVK_ANSI_X: "KeyX", kVK_ANSI_C: "KeyC", kVK_ANSI_V: "KeyV", kVK_ANSI_B: "KeyB",
    kVK_ANSI_N: "KeyN", kVK_ANSI_M: "KeyM",
    kVK_ANSI_Comma: "Comma", kVK_ANSI_Period: "Period", kVK_ANSI_Slash: "Slash", kVK_JIS_Underscore: "IntlRo",
    kVK_ISO_Section: "IntlBackslash",
    kVK_F1: "F1", kVK_F2: "F2", kVK_F3: "F3", kVK_F4: "F4", kVK_F5: "F5", kVK_F6: "F6", kVK_F7: "F7", kVK_F8: "F8",
    kVK_F9: "F9", kVK_F10: "F10", kVK_F11: "F11", kVK_F12: "F12", kVK_F13: "F13", kVK_F14: "F14", kVK_F15: "F15",
    kVK_F16: "F16", kVK_F17: "F17", kVK_F18: "F18", kVK_F19: "F19", kVK_F20: "F20",
    // The keypad's Enter is Enter to the engine, as it is to every app.
    kVK_Space: "Space", kVK_Return: "Enter", kVK_ANSI_KeypadEnter: "Enter", kVK_Delete: "Backspace",
    kVK_Tab: "Tab", kVK_Escape: "Escape", kVK_ForwardDelete: "Delete",
    kVK_UpArrow: "ArrowUp", kVK_DownArrow: "ArrowDown", kVK_LeftArrow: "ArrowLeft", kVK_RightArrow: "ArrowRight",
    kVK_Home: "Home", kVK_End: "End", kVK_PageUp: "PageUp", kVK_PageDown: "PageDown", kVK_Help: "Insert",
    kVK_ANSI_Keypad0: "Numpad0", kVK_ANSI_Keypad1: "Numpad1", kVK_ANSI_Keypad2: "Numpad2", kVK_ANSI_Keypad3: "Numpad3",
    kVK_ANSI_Keypad4: "Numpad4", kVK_ANSI_Keypad5: "Numpad5", kVK_ANSI_Keypad6: "Numpad6", kVK_ANSI_Keypad7: "Numpad7",
    kVK_ANSI_Keypad8: "Numpad8", kVK_ANSI_Keypad9: "Numpad9", kVK_ANSI_KeypadDecimal: "NumpadDecimal",
    kVK_ANSI_KeypadPlus: "NumpadAdd", kVK_ANSI_KeypadMinus: "NumpadSubtract", kVK_ANSI_KeypadMultiply: "NumpadMultiply",
    kVK_ANSI_KeypadDivide: "NumpadDivide", kVK_ANSI_KeypadEquals: "NumpadEqual", kVK_ANSI_KeypadClear: "NumLock",
    kVK_JIS_KeypadComma: "NumpadComma",
    kVK_JIS_Kana: "Lang1", kVK_JIS_Eisu: "Lang2", kVK_CapsLock: "CapsLock",
]

/// The modifier keys, for a modifier pressed and released alone (the standard, §9.3).
let modifierNames: [Int: String] = [
    kVK_Shift: "ShiftLeft", kVK_RightShift: "ShiftRight", kVK_Control: "ControlLeft", kVK_RightControl: "ControlRight",
    kVK_Option: "AltLeft", kVK_RightOption: "AltRight", kVK_Command: "MetaLeft", kVK_RightCommand: "MetaRight",
]

/// The key's code. A key with no code of its own is `Unidentified`: the engine passes it on.
func keyName(_ keyCode: Int) -> String {
    keyNames[keyCode] ?? "Unidentified"
}

/// The key code that sends a code, for the keys the input method may hand back to macOS.
func keyCode(named code: String) -> Int? {
    keyNames.first { $0.value == code && $0.key != kVK_ANSI_KeypadEnter }?.key
}
