/* secondscreen-sender — Mac capture server for the HP TouchPad receiver.
 *
 * ScreenCaptureKit → letterboxed 1024×768 JPEG → framed 'J' messages over
 * TCP :5959 with latest-frame-wins; receiver 'T' touch messages → CGEvent
 * mouse injection. Protocol: ../receiver/PROTOCOL.md
 *
 * Permissions (System Settings → Privacy & Security), both attributed to
 * the app that launched this process (your terminal):
 *   - Screen & System Audio Recording  (capture)
 *   - Accessibility                    (touch → mouse injection)
 */
import AppKit
import ApplicationServices
import CoreGraphics
import CVDShim
import Darwin
import Foundation

struct Options {
    var port: UInt16 = 5959
    var fps = 25
    var quality = 0.6
    var mirrorDisplay: Int?  // nil = virtual second screen (the default)
    var dryRun = false
    var listDisplays = false
    var checkPermissions = false
    var autolaunch = true
}

func usage() -> Never {
    print("""
    usage: secondscreen-sender [options]
    Creates a virtual 1024x768 second monitor and streams it (default), or
    mirrors an existing display with --mirror.
      --port N          listen port (default 5959)
      --fps N           capture frame-rate cap (default 25)
      --quality Q       JPEG quality 0.0-1.0 (default 0.6)
      --mirror          mirror the main display instead of extending
      --display N       mirror display index N (see --list-displays)
      --no-autolaunch   don't start the receiver on a USB-connected TouchPad
      --dry-run         log touch/key injection instead of performing it
      --list-displays   list capturable displays and exit
      --check-permissions  trigger/report permission state and exit
    """)
    exit(2)
}

func parseArgs() -> Options {
    var o = Options()
    var it = CommandLine.arguments.dropFirst().makeIterator()
    func value(_ flag: String) -> String {
        guard let v = it.next() else {
            print("missing value for \(flag)")
            usage()
        }
        return v
    }
    while let a = it.next() {
        switch a {
        case "--port": o.port = UInt16(value(a)) ?? o.port
        case "--fps": o.fps = max(1, Int(value(a)) ?? o.fps)
        case "--quality": o.quality = min(1, max(0, Double(value(a)) ?? o.quality))
        case "--mirror": o.mirrorDisplay = 0
        case "--display": o.mirrorDisplay = Int(value(a)) ?? 0  // legacy alias for --mirror N
        case "--no-autolaunch": o.autolaunch = false
        case "--dry-run": o.dryRun = true
        case "--list-displays": o.listDisplays = true
        case "--check-permissions": o.checkPermissions = true
        default: usage()
        }
    }
    return o
}

func lanAddresses() -> [String] {
    var result = [String]()
    var ifap: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifap) == 0 else { return result }
    defer { freeifaddrs(ifap) }
    var p = ifap
    while let cur = p?.pointee {
        if let sa = cur.ifa_addr, sa.pointee.sa_family == sa_family_t(AF_INET),
           cur.ifa_flags & UInt32(IFF_LOOPBACK) == 0,
           cur.ifa_flags & UInt32(IFF_UP) != 0 {
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host,
                           socklen_t(NI_MAXHOST), nil, 0, NI_NUMERICHOST) == 0 {
                result.append(String(cString: host))
            }
        }
        p = cur.ifa_next
    }
    return result
}

let opts = parseArgs()
signal(SIGPIPE, SIG_IGN)

// Finder-launched (no tty): keep a log file instead of losing stdout
if isatty(1) == 0 {
    let logPath = ("~/Library/Logs/webOSSecondScreen.log" as NSString).expandingTildeInPath
    freopen(logPath, "a", stdout)
    freopen(logPath, "a", stderr)
    setvbuf(stdout, nil, _IONBF, 0)
    log("=== webOS Second Screen starting ===")
}

// --- permissions: preflight, and trigger the system prompts if missing ---
let screenOK = CGPreflightScreenCaptureAccess()
if !screenOK {
    CGRequestScreenCaptureAccess()  // async system prompt; registers us in System Settings
}
let axPromptKey = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
let axOK = AXIsProcessTrustedWithOptions([axPromptKey: true] as CFDictionary)

// bundled = double-clicked .app with its own TCC identity; otherwise the
// grants belong to the terminal app hosting this process
let isBundled = Bundle.main.bundleIdentifier?.hasPrefix("org.webosarchive.secondscreen") ?? false
let grantee = isBundled ? "webOS Second Screen" : "the app that runs this process — your terminal"

print("permissions:")
print("  screen recording: \(screenOK ? "OK" : "MISSING — grant in System Settings → Privacy & Security → Screen & System Audio Recording")")
print("  accessibility:    \(axOK ? "OK" : "MISSING — grant in System Settings → Privacy & Security → Accessibility")")
if !screenOK || !axOK {
    print("  (grant to \(grantee), then relaunch)")
}

if opts.checkPermissions {
    exit(screenOK && axOK ? 0 : 1)
}

if opts.listDisplays {
    guard screenOK else { exit(1) }
    do {
        for (i, d) in try CaptureEngine.shareableDisplays().enumerated() {
            print("  [\(i)] display \(d.displayID): \(d.width)x\(d.height) at (\(Int(d.frame.minX)),\(Int(d.frame.minY)))")
        }
    } catch {
        print("cannot list displays: \(error)")
        exit(1)
    }
    exit(0)
}

guard screenOK else {
    print("cannot capture without Screen Recording permission; exiting")
    if isatty(1) == 0 {
        // Finder launch: dying silently is hostile — explain and open Settings
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let alert = NSAlert()
        alert.messageText = "Screen Recording permission needed"
        alert.informativeText = """
        webOS Second Screen streams a virtual display to your TouchPad, \
        which requires the Screen Recording permission (and Accessibility, \
        for touch control).

        Enable "webOS Second Screen" in System Settings, then launch the \
        app again.
        """
        alert.addButton(withTitle: "Open System Settings")
        alert.addButton(withTitle: "Quit")
        if alert.runModal() == .alertFirstButtonReturn,
           let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
            NSWorkspace.shared.open(url)
        }
    }
    exit(1)
}
if !axOK {
    print("warning: touch injection will silently do nothing until Accessibility is granted")
}

let ips = lanAddresses().filter { $0.hasPrefix("192.168.") || $0.hasPrefix("10.") || $0.hasPrefix("172.") }
if let ip = ips.first {
    print("point the TouchPad here:  echo \"host=\(ip)\" > /media/internal/secondscreen.conf")
    if ips.count > 1 { print("(all LAN addresses: \(ips.joined(separator: ", ")))") }
}

let target: CaptureTarget
var virtualDisplay: CGVirtualDisplay?  // must stay alive: releasing unplugs the monitor
if let idx = opts.mirrorDisplay {
    target = .index(idx)
    log("mirroring display index \(idx)")
} else {
    do {
        let vd = try createVirtualDisplay(width: 1024, height: 768, refreshHz: 30)
        virtualDisplay = vd
        target = .id(vd.displayID)
        log("virtual display \(vd.displayID) online — arrange it in System Settings → Displays")
    } catch {
        log("virtual display failed (\(error)); falling back to mirroring display 0")
        target = .index(0)
    }
}
_ = virtualDisplay

if opts.autolaunch {
    let ip = ips.first
    let port = opts.port
    Thread { autolaunchReceiver(hostIP: ip, port: port) }.start()
}

// server on a background thread; main thread hosts the menu-bar item
let serverThread = Thread {
    runServer(port: opts.port, fps: opts.fps, quality: opts.quality,
              target: target, dryRun: opts.dryRun)
}
serverThread.stackSize = 1 << 21
serverThread.start()

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
StatusUI.shared.start()
StatusUI.shared.setState("Waiting for TouchPad…")
app.run()
