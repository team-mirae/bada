import Cocoa
import IOKit.hid

/// Keyboard keys macOS doesn't turn into key events of their own (the standard, §9.3): a PC board's
/// 無変換 (HID usage 0x8B) and 変換 (0x8A) come through as nothing, and カタカナ (0x88) as かな. bada
/// reads them at the HID level, which needs Input Monitoring. Caps Lock is read here too (CapsLock).
///
/// Reading is all it does: nothing is taken from the keyboard, and nothing is sent to it but the
/// Caps Lock light.
enum HIDKeys {
    /// The usages read here (keyboard usage page 7), by their code.
    static let codes: [Int: String] = [0x88: "KanaMode", 0x8A: "Convert", 0x8B: "NonConvert"]

    private static var manager: IOHIDManager?

    static var isListening: Bool { manager != nil }

    /// Whether macOS lets bada read keys at the HID level.
    static var isAllowed: Bool { IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted }

    /// Starts listening, asking for Input Monitoring if `ask` (once; macOS remembers the answer).
    static func start(ask: Bool) {
        guard manager == nil else { return }
        if !isAllowed {
            guard ask else { return }
            IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { start(ask: false) }
            return
        }
        let m = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        IOHIDManagerSetDeviceMatching(m, [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop,
                                          kIOHIDDeviceUsageKey: kHIDUsage_GD_Keyboard] as CFDictionary)
        IOHIDManagerRegisterInputValueCallback(m, { _, _, _, value in HIDKeys.value(value) }, nil)
        IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        let result = IOHIDManagerOpen(m, IOOptionBits(kIOHIDOptionsTypeNone))
        // Exclusive access: some keyboard is seized (Karabiner-Elements seizes the physical ones and
        // types through its own virtual keyboard). The others, that virtual one included, are open.
        guard result == kIOReturnSuccess || result == kIOReturnExclusiveAccess else {
            DiagnosticLog.note("HID keys: couldn't open (\(result))")
            return
        }
        manager = m
        DiagnosticLog.note(result == kIOReturnSuccess ? "HID keys: on" : "HID keys: on (some keyboards are seized by another app)")
        CapsLock.start()
        KeyRouter.tableChanged()
        StatePublisher.refresh()
    }

    /// On when the key tap is (Caps Lock needs both), or when the key table binds one of these keys.
    static func check() {
        if LanguageKeyTap.isOn || !Bada.table.boundKeys.isDisjoint(with: KeyRouter.hidKeys) {
            start(ask: false)
        }
    }

    /// Lights or darkens the Caps Lock LED of every keyboard, directly (CapsLock.blink).
    static func setCapsLED(_ lit: Bool) {
        guard let manager, let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else { return }
        for device in devices {
            let match = [kIOHIDElementUsagePageKey: kHIDPage_LEDs, kIOHIDElementUsageKey: kHIDUsage_LED_CapsLock] as CFDictionary
            guard let elements = IOHIDDeviceCopyMatchingElements(device, match, IOOptionBits(kIOHIDOptionsTypeNone)) as? [IOHIDElement] else { continue }
            for element in elements {
                IOHIDDeviceSetValue(device, element, IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, element, 0, lit ? 1 : 0))
            }
        }
    }

    /// Each keyboard's Caps Lock LED as last set (for the diagnostic log).
    static func capsLEDs() -> String {
        guard let manager, let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else { return "none" }
        var out: [String] = []
        for device in devices {
            let match = [kIOHIDElementUsagePageKey: kHIDPage_LEDs, kIOHIDElementUsageKey: kHIDUsage_LED_CapsLock] as CFDictionary
            guard let elements = IOHIDDeviceCopyMatchingElements(device, match, IOOptionBits(kIOHIDOptionsTypeNone)) as? [IOHIDElement], let element = elements.first else { continue }
            let name = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "?"
            var value = Unmanaged.passUnretained(IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, element, 0, -1))
            let r = IOHIDDeviceGetValue(device, element, &value)
            out.append("\(name)=\(r == kIOReturnSuccess ? String(IOHIDValueGetIntegerValue(value.takeUnretainedValue())) : "?")")
        }
        return out.joined(separator: ", ")
    }

    private static func value(_ value: IOHIDValue) {
        let element = IOHIDValueGetElement(value)
        guard IOHIDElementGetUsagePage(element) == kHIDPage_KeyboardOrKeypad else { return }
        let usage = Int(IOHIDElementGetUsage(element))
        let state = IOHIDValueGetIntegerValue(value)
        guard state == 0 || state == 1 else { return }
        if usage == kHIDUsage_KeyboardCapsLock {
            CapsLock.key(down: state == 1)
            return
        }
        guard let code = codes[usage] else { return }
        if state == 1 { DiagnosticLog.note(String(format: "HID keys: usage 0x%02X", usage)) }
        KeyRouter.hid(code, down: state == 1)
    }
}
