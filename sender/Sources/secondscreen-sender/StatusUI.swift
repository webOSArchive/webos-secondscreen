/* Menu-bar status item: gives the bundled .app a visible face, live
 * connection state, and a way to quit. The server threads report state
 * via setState(); everything UI happens on the main thread. */
import AppKit

final class StatusUI: NSObject {
    static let shared = StatusUI()
    private var item: NSStatusItem?
    private var stateItem: NSMenuItem?
    private var updateURL: URL?
    private var started = false

    func start() {
        started = true
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let img = NSImage(systemSymbolName: "display",
                             accessibilityDescription: "webOS Second Screen") {
            img.isTemplate = true
            item.button?.image = img
        } else {
            item.button?.title = "TP"
        }

        let menu = NSMenu()
        menu.autoenablesItems = false
        let title = NSMenuItem(title: "webOS Second Screen", action: nil, keyEquivalent: "")
        title.isEnabled = false
        let state = NSMenuItem(title: "Starting…", action: nil, keyEquivalent: "")
        state.isEnabled = false
        menu.addItem(title)
        menu.addItem(state)
        menu.addItem(.separator())
        let arrange = NSMenuItem(title: "Arrange Displays…",
                                 action: #selector(openDisplaySettings), keyEquivalent: "")
        arrange.target = self
        menu.addItem(arrange)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit",
                                action: #selector(NSApplication.terminate(_:)),
                                keyEquivalent: "q"))
        item.menu = menu
        self.item = item
        self.stateItem = state
    }

    /// Safe from any thread. Adds an "Update Available" item linking to
    /// the GitHub release page (once; later calls just refresh the URL).
    func showUpdateAvailable(version: String, url: URL) {
        guard started else { return }
        DispatchQueue.main.async {
            let firstTime = self.updateURL == nil
            self.updateURL = url
            guard firstTime, let menu = self.item?.menu else { return }
            let mi = NSMenuItem(title: "Update Available: \(version)…",
                                action: #selector(self.openUpdatePage), keyEquivalent: "")
            mi.target = self
            // below the connection-state line (title, state, …)
            menu.insertItem(mi, at: 2)
        }
    }

    @objc private func openUpdatePage() {
        if let url = updateURL { NSWorkspace.shared.open(url) }
    }

    @objc private func openDisplaySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Displays-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    /// Safe from any thread, no-op when running without the status item.
    func setState(_ text: String) {
        guard started else { return }
        DispatchQueue.main.async { self.stateItem?.title = text }
    }
}
