import CoreServices
import Foundation

/// Watches the settings folder (the standard, §10.1): the settings page and the command line write
/// files there and never talk to the input method, so it reloads when a file changes.
final class FolderWatcher {
    private var stream: FSEventStreamRef?
    private let onChange: () -> Void
    private var pending = false
    /// Files the input method writes itself, and the command line's undo history: not settings.
    private let ignored: (String) -> Bool

    /// `onChange` is called on the main queue, a moment after the last change of a burst.
    init?(_ folder: URL, ignoring ignored: @escaping (String) -> Bool, onChange: @escaping () -> Void) {
        self.onChange = onChange
        self.ignored = ignored
        var context = FSEventStreamContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(), retain: nil, release: nil, copyDescription: nil)
        let callback: FSEventStreamCallback = { _, info, count, paths, _, _ in
            guard let info else { return }
            let watcher = Unmanaged<FolderWatcher>.fromOpaque(info).takeUnretainedValue()
            let paths = unsafeBitCast(paths, to: NSArray.self) as? [String] ?? []
            watcher.changed(Array(paths.prefix(count)))
        }
        let flags = UInt32(kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagNoDefer)
        guard let stream = FSEventStreamCreate(kCFAllocatorDefault, callback, &context, [folder.path] as CFArray,
                                               FSEventStreamEventId(kFSEventStreamEventIdSinceNow), 0.1, flags) else { return nil }
        self.stream = stream
        FSEventStreamSetDispatchQueue(stream, DispatchQueue.main)
        FSEventStreamStart(stream)
    }

    deinit { stop() }

    func stop() {
        guard let stream else { return }
        FSEventStreamStop(stream)
        FSEventStreamInvalidate(stream)
        FSEventStreamRelease(stream)
        self.stream = nil
    }

    private func changed(_ paths: [String]) {
        guard paths.contains(where: { !ignored($0) }), !pending else { return }
        pending = true
        // A writer's temporary file and its rename come as several events: one reload for them all.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            guard let self else { return }
            self.pending = false
            self.onChange()
        }
    }

    /// What the input method leaves out when it watches its own folder: the state it publishes
    /// itself, the undo history, and temporary files on their way to a rename.
    static func isNotSettings(_ path: String) -> Bool {
        let name = (path as NSString).lastPathComponent
        return name == "state.json" || name.hasPrefix(".") || name.hasSuffix(".tmp") || path.contains("/history/") || name == "history"
    }
}
