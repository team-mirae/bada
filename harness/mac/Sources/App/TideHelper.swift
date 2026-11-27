import Cocoa
import IOKit.hid

/// bada-tide's device side (docs/bada-tide-v1.md), in the helper process `Bada --tide-helper` (Tide
/// starts it). It reads the settings folder's `tide/*.json`, matches the keyboards, holds each lease
/// (`hello`, then `state` every third of it and on every change) and passes key events up.
///
/// A separate process because an input method is an NSApplication, and in one macOS delivers the
/// keyboard's HID input reports unreliably (seen: mostly not at all, App Nap held off or not), while
/// sends still go out. A plain run loop gets every report.
///
/// Lines on stdin, from the input method (JSON): `{"state": {"mode", "profile", "caps", "take"}}`,
/// `{"reload": true}`. Lines on stdout: `{"key": {"code", "down", "mods"}}` and `{"status":
/// {"links", "available", "problems"}}`. stdin closing (the input method quit) ends the helper, and
/// with it every lease.
enum TideHelper {
    /// One matched keyboard.
    final class Link {
        enum Phase: String { case connecting, held, conflict, blocked, unsupported, silent, error }
        let device: IOHIDDevice
        let translator: TideTranslator
        var phase = Phase.connecting
        var features: Set<String> = []
        var leaseMs = 3000
        var conflict: String?
        var retryAt = Date.distantPast
        var lastSent: [UInt8]?
        var lastSentAt = Date.distantPast
        var take: Set<String> = []
        /// Taken keys whose press ran here: their release runs here too, whatever take is by then.
        var down: Set<String> = []
        var buffer: UnsafeMutablePointer<UInt8>
        /// The report being sent: it must outlive the asynchronous send.
        var out: UnsafeMutablePointer<UInt8>
        var inFlightSince: Date?
        /// When a report last came from the keyboard: a link that hears nothing takes nothing (§3).
        var lastHeard = Date.distantPast
        /// hellos sent with no answer, since the last answer
        var unanswered = 0
        init(device: IOHIDDevice, translator: TideTranslator) {
            self.device = device
            self.translator = translator
            buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: translator.match.reportSize)
            out = UnsafeMutablePointer<UInt8>.allocate(capacity: translator.match.reportSize)
        }
        deinit {
            buffer.deallocate()
            out.deallocate()
        }
    }

    /// `--tide-probe`: print what happens.
    static var verbose = false
    fileprivate static func note(_ line: String) {
        if verbose { FileHandle.standardError.write(Data((line + "\n").utf8)) }
    }

    /// The input method's state, as it last said (`{"state": ...}`).
    struct Host: Equatable {
        var mode = Mode.muhenkan
        var profile: Int?
        var caps = false
        var take: Set<String> = []
    }
    static var host = Host()

    private static var translators: [TideTranslator] = []
    private(set) static var problems: [Problem] = []
    private static var links: [Link] = []
    private static var managers: [IOHIDManager] = []
    private static var timer: Timer?
    private static var observing = false

    static var folder: URL { Bada.folder.appendingPathComponent("tide") }

    /// The keys tide reads now, from keyboards whose lease is held (for `harness.keys.available`).
    private static var availableKeys: Set<String> {
        Set(links.filter { $0.phase == .held && $0.features.contains("keys") }.flatMap { $0.translator.usage.keys })
    }

    /// What `harness.tide` publishes (bada-tide §8).
    private static var published: [[String: Any]] {
        links.map { l in
            ["translator": l.translator.id, "state": l.phase.rawValue,
             "version": "\(TideTranslator.major).\(TideTranslator.minor)",
             "features": l.features.sorted(), "take": l.take.sorted(),
             "conflict": l.conflict ?? NSNull()] as [String: Any]
        }
    }

    // MARK: Translators

    /// Reads `tide/*.json` again: called at start and whenever the settings folder changes.
    static func reload() {
        var found: [TideTranslator] = []
        var problems: [Problem] = []
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        for url in files.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) where url.pathExtension == "json" {
            let file = "tide/" + url.lastPathComponent
            guard let data = try? Data(contentsOf: url) else { continue }
            do {
                let t = try TideTranslator(json: data)
                if found.contains(where: { $0.id == t.id }) {
                    problems.append(Problem(file: file, path: "id", code: "duplicate-translator", message: "another translator has the id \(t.id)", level: "error"))
                } else {
                    found.append(t)
                }
            } catch TideTranslator.Problem.denied(let message) {
                problems.append(Problem(file: file, path: message + ".send", code: "denied",
                                        message: "the \(message) template begins like a request bada-tide never sends (§7)", level: "error"))
            } catch TideTranslator.Problem.invalid(let why) {
                problems.append(Problem(file: file, path: "", code: "bad-value", message: why, level: "error"))
            } catch {}
        }
        let changed = found.map(\.id) != translators.map(\.id)
        translators = found
        self.problems = problems
        if changed { restart() }
        reportStatus()
    }

    /// Drops every link and opens the devices again for the translators there are now.
    private static func restart() {
        for m in managers { IOHIDManagerClose(m, IOOptionBits(kIOHIDOptionsTypeNone)) }
        managers = []
        links = []
                timer?.invalidate()
        timer = nil
        guard !translators.isEmpty else { return }
        for t in translators {
            let m = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
            IOHIDManagerSetDeviceMatching(m, [kIOHIDVendorIDKey: t.match.vendorID, kIOHIDProductIDKey: t.match.productID,
                                              kIOHIDPrimaryUsagePageKey: t.match.usagePage, kIOHIDPrimaryUsageKey: t.match.usage] as CFDictionary)
            let context = Unmanaged.passRetained(Box(t)).toOpaque()
            IOHIDManagerRegisterDeviceMatchingCallback(m, { ctx, _, _, device in
                guard let ctx else { return }
                TideHelper.matched(device, Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue().translator)
            }, context)
            IOHIDManagerRegisterDeviceRemovalCallback(m, { _, _, _, device in TideHelper.removed(device) }, nil)
            // Reports from every matched keyboard, at the manager (as HIDKeys reads its values): a
            // device-level callback got nothing in the input method's process.
            IOHIDManagerRegisterInputReportCallback(m, { _, _, sender, _, _, report, length in
                guard let sender else { return }
                let device = Unmanaged<IOHIDDevice>.fromOpaque(sender).takeUnretainedValue()
                guard let link = TideHelper.link(for: device) else { return }
                TideHelper.received(link, Array(UnsafeBufferPointer(start: report, count: length)))
            }, nil)
            IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
            let r = IOHIDManagerOpen(m, IOOptionBits(kIOHIDOptionsTypeNone))
            TideHelper.note("tide: \(t.id): \(r == kIOReturnSuccess ? "watching" : "couldn't open (\(r))")")
            managers.append(m)
        }
        if !observing {
            observing = true
            let center = NSWorkspace.shared.notificationCenter
            for name in [NSWorkspace.didLaunchApplicationNotification, NSWorkspace.didTerminateApplicationNotification] {
                center.addObserver(forName: name, object: nil, queue: .main) { _ in tick() }
            }
        }
        // On the main run loop, in every mode: the settings may be read on another thread, and a menu
        // or a drag must not stop the renewals.
        let t = Timer(timeInterval: 0.25, repeats: true) { _ in tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private final class Box {
        let translator: TideTranslator
        init(_ t: TideTranslator) { translator = t }
    }

    // MARK: Devices

    private static func matched(_ device: IOHIDDevice, _ translator: TideTranslator) {
        let m = translator.match
        let version = IOHIDDeviceGetProperty(device, kIOHIDVersionNumberKey as CFString) as? Int ?? 0
        if let lo = m.versionMin, version < lo { return }
        if let hi = m.versionMax, version > hi { return }
        guard !links.contains(where: { $0.device === device }) else { return }
        let link = Link(device: device, translator: translator)
        links.append(link)
        IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        TideHelper.note("tide: \(translator.id): keyboard connected")
        tick()
    }

    fileprivate static func link(for device: IOHIDDevice) -> Link? { links.first { $0.device === device } }

    private static func removed(_ device: IOHIDDevice) {
        guard let i = links.firstIndex(where: { $0.device === device }) else { return }
        TideHelper.note("tide: \(links[i].translator.id): keyboard gone")
        links.remove(at: i)   // a keyboard that comes back is a new one (§3)
        reportStatus()
    }

    /// Sends a report without waiting: the input method's main thread must never block on a keyboard.
    /// One send at a time per keyboard, each given 250 ms; a renewal that can't go now goes at the next tick.
    private static func send(_ link: Link, _ report: [UInt8]) {
        if let since = link.inFlightSince, Date().timeIntervalSince(since) < 1 { return }
        link.lastSent = report
        link.lastSentAt = Date()
        link.inFlightSince = Date()
        report.withUnsafeBufferPointer { link.out.update(from: $0.baseAddress!, count: report.count) }
        let r = IOHIDDeviceSetReportWithCallback(link.device, kIOHIDReportTypeOutput, 0, link.out, report.count, 0.25, { ctx, result, _, _, _, _, _ in
            guard let ctx else { return }
            let link = Unmanaged<Link>.fromOpaque(ctx).takeUnretainedValue()
            link.inFlightSince = nil
            if result != kIOReturnSuccess {
                link.lastSentAt = .distantPast   // try again at the next tick
                TideHelper.note("tide: \(link.translator.id): send failed (\(result))")
            }
        }, Unmanaged.passUnretained(link).toOpaque())
        if r != kIOReturnSuccess {
            link.inFlightSince = nil
            link.lastSentAt = .distantPast
            TideHelper.note("tide: \(link.translator.id): couldn't send (\(r))")
        }
    }

    // MARK: The lease

    /// The keys taken from this keyboard (§4): what the input method asks for, that it can take.
    private static func wantedTake(_ link: Link) -> Set<String> {
        guard link.features.contains("take") else { return [] }
        return host.take.intersection(link.translator.take.keys)
    }

    private static func stateReport(_ link: Link) -> [UInt8]? {
        link.take = wantedTake(link)
        return link.translator.state(mode: host.mode, profile: host.profile, caps: host.caps,
                                     take: link.take, size: link.translator.match.reportSize)
    }

    /// Runs four times a second, and on app launch and quit: conflicts, hellos, renewals.
    static func tick() {
        let running = Set(NSWorkspace.shared.runningApplications.compactMap(\.bundleIdentifier))
        let now = Date()
        for link in links {
            let size = link.translator.match.reportSize
            // §5: a conflicting program runs: no hello, no renewal (the lease runs out by itself).
            if let c = link.translator.conflicts.first(where: { !running.isDisjoint(with: $0.bundleIDs) }) {
                if link.phase != .conflict || link.conflict != c.id {
                    link.phase = .conflict
                    link.conflict = c.id
                    link.take = []
                    TideHelper.note("tide: \(link.translator.id): \(c.name) is running; link paused")
                    reportStatus()
                }
                continue
            }
            if link.phase == .conflict {
                link.phase = .connecting
                link.conflict = nil
            }
            switch link.phase {
            case .unsupported, .error:
                continue
            case .blocked where now < link.retryAt:
                continue
            case .silent where now < link.retryAt:
                continue
            case .connecting, .blocked, .silent:
                // hello, again every second until it is answered; a keyboard that never answers (a
                // stock firmware with the same ids) is left alone for a minute after five tries.
                if link.unanswered >= 5 {
                    if link.phase != .silent { TideHelper.note("tide: \(link.translator.id): no answer to hello; trying again in a minute") }
                    link.phase = .silent
                    link.unanswered = 0
                    link.retryAt = now.addingTimeInterval(60)
                    reportStatus()
                    continue
                }
                if now.timeIntervalSince(link.lastSentAt) >= 1, let r = link.translator.hello(size: size) {
                    link.unanswered += 1
                    send(link, r)
                }
            case .held:
                // §3: a link that hears nothing from the keyboard takes nothing. Its replies come
                // every renewal; two seconds of silence and bada stops renewing, so the lease runs
                // out and the keyboard gives the keys back to macOS.
                if now.timeIntervalSince(link.lastHeard) > 2 {
                    TideHelper.note("tide: \(link.translator.id): the keyboard went quiet; letting the lease run out")
                    link.phase = .connecting
                    link.take = []
                    link.down = []
                    link.lastSentAt = now   // the next hello after a second
                    reportStatus()
                    continue
                }
                guard let r = stateReport(link) else { continue }
                // Every third of the lease, and at once when the state changed (§3).
                if r != link.lastSent || now.timeIntervalSince(link.lastSentAt) * 1000 >= Double(link.leaseMs) / 3 {
                    send(link, r)
                }
            case .conflict:
                continue
            }
        }
        reportStatus()   // only when it changed (take follows the input method's state)
    }

    // MARK: The pipe to the input method

    private static var lastStatus: String?

    private static func emit(_ object: [String: Any]) {
        guard let line = JSON.text(object) else { return }
        FileHandle.standardOutput.write(Data((line + "\n").utf8))
    }

    /// Tells the input method what the links are now, when it changed.
    private static func reportStatus() {
        let status: [String: Any] = ["links": published, "available": availableKeys.sorted(),
                                     "problems": problems.map(\.json)]
        guard let text = JSON.text(status), text != lastStatus else { return }
        lastStatus = text
        emit(["status": status])
    }

    /// One line from the input method.
    private static func command(_ line: String) {
        guard let data = line.data(using: .utf8), let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return }
        if o["reload"] as? Bool == true { reload() }
        if let s = o["state"] as? [String: Any] {
            var h = Host()
            h.mode = Mode(rawValue: s["mode"] as? String ?? "") ?? .muhenkan
            h.profile = s["profile"] as? Int
            h.caps = s["caps"] as? Bool ?? false
            h.take = Set(s["take"] as? [String] ?? [])
            if h != host {
                host = h
                tick()   // renew at once with the new state (§3)
            }
        }
    }

    /// `Bada --tide-helper`: runs until stdin closes.
    static func run() -> Never {
        let input = FileHandle.standardInput
        var pending = Data()
        input.readabilityHandler = { handle in
            let chunk = handle.availableData
            DispatchQueue.main.async {
                if chunk.isEmpty { exit(0) }   // the input method quit: every lease runs out by itself
                pending.append(chunk)
                while let nl = pending.firstIndex(of: 10) {
                    let line = String(decoding: pending[pending.startIndex..<nl], as: UTF8.self)
                    pending.removeSubrange(pending.startIndex...nl)
                    command(line)
                }
            }
        }
        reload()
        RunLoop.main.run()
        exit(0)
    }

    private static func received(_ link: Link, _ report: [UInt8]) {
        let incoming = link.translator.read(report)
        if incoming != .other {
            link.lastHeard = Date()
            link.unanswered = 0
        }
        switch incoming {
        case .hello(let status, let features, let leaseMs):
            switch status {
            case "ok":
                let was = link.phase
                link.phase = .held
                link.features = features
                link.leaseMs = leaseMs
                if was != .held { TideHelper.note("tide: \(link.translator.id): lease held (\(features.sorted().joined(separator: ", ")), \(leaseMs) ms)") }
                link.lastSent = nil   // send the state now
                tick()
            case "blocked":
                link.phase = .blocked
                link.retryAt = Date().addingTimeInterval(Double(link.translator.retryMs) / 1000)
                TideHelper.note("tide: \(link.translator.id): blocked (another program used the channel)")
            case "unsupported":
                link.phase = .unsupported
                TideHelper.note("tide: \(link.translator.id): the keyboard speaks another tide major")
            default:
                link.phase = .error
            }
            reportStatus()
        case .state(let status):
            if status == "no-lease" {
                // The lease ran out (the keyboard was busy, or the Mac slept): start again.
                link.phase = .connecting
                link.take = []
                link.lastSentAt = .distantPast
                reportStatus()
            }
        case .key(let code, let down, let mods, _, let dropped):
            if dropped { TideHelper.note("tide: \(link.translator.id): events were dropped") }
            // A key bada doesn't take reaches macOS too, and is read there: only taken keys run here.
            if down {
                guard link.phase == .held, link.take.contains(code) else { return }
                link.down.insert(code)
                emit(["key": ["code": code, "down": true, "mods": mods.names]])
            } else if link.down.remove(code) != nil {
                emit(["key": ["code": code, "down": false, "mods": mods.names]])
            }
        case .other:
            break
        }
    }
}
