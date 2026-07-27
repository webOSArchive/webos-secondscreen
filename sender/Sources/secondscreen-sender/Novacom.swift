/* USB auto-launch: if a TouchPad is connected over USB (novacom), point
 * its receiver config at this Mac and start the receiver app if it isn't
 * already running.
 *
 * Hard-won webOS rules (PLAN.md): pipe commands to `/bin/sh` stdin —
 * novacom arg passing after `--` is unreliable and luna-send does nothing
 * from a novacom-piped shell. Launching the PDK binary directly from a
 * shell is the proven path (wake lock, touch, GL all work). */
import Foundation

private let receiverAppDirs = [
    "/media/cryptofs/apps/usr/palm/applications/org.webosarchive.secondscreen",
]

private func findNovacom() -> String? {
    // Finder-launched apps have a bare PATH; check the usual homes
    let candidates = ["/usr/local/bin/novacom", "/opt/homebrew/bin/novacom",
                      "/opt/nova/bin/novacom", "/usr/bin/novacom"]
    return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
}

private func runProcess(_ path: String, _ args: [String], stdin input: String?,
                        timeout: TimeInterval) -> String? {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    let outPipe = Pipe()
    p.standardOutput = outPipe
    p.standardError = outPipe
    if let input {
        let inPipe = Pipe()
        p.standardInput = inPipe
        do { try p.run() } catch { return nil }
        inPipe.fileHandleForWriting.write(input.data(using: .utf8)!)
        inPipe.fileHandleForWriting.closeFile()
    } else {
        do { try p.run() } catch { return nil }
    }
    let killer = DispatchWorkItem { if p.isRunning { p.terminate() } }
    DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: killer)
    let data = outPipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    killer.cancel()
    guard p.terminationStatus == 0 else { return nil }
    return String(data: data, encoding: .utf8)
}

/// Best-effort, never blocks startup for long, never fatal.
func autolaunchReceiver(hostIP: String?, port: UInt16) {
    guard let novacom = findNovacom() else {
        log("autolaunch: novacom not installed, skipping USB check")
        return
    }
    guard let list = runProcess(novacom, ["-l"], stdin: nil, timeout: 5),
          list.contains("usb") else {
        log("autolaunch: no TouchPad on USB")
        return
    }
    let device = list.split(separator: "\n").first.map(String.init) ?? "?"
    log("autolaunch: TouchPad on USB (\(device))")

    // Always refresh the conf override with our current IP — the receiver
    // binary's built-in default points at the dev VM, so a stale/missing
    // conf dials the wrong machine. A receiver that is already running is
    // not restarted (it may be a dev session); it reads the new conf on
    // its next launch.
    // Rewrite only host=/port=, preserving any other keys the user has
    // added by hand (saver_secs=, idle_secs=, future settings).
    var conf = ""
    if let ip = hostIP {
        conf = """
        grep -v '^host=' /media/internal/secondscreen.conf 2>/dev/null | grep -v '^port=' > /tmp/ss.conf
        echo "host=\(ip)" >> /tmp/ss.conf
        echo "port=\(port)" >> /tmp/ss.conf
        mv /tmp/ss.conf /media/internal/secondscreen.conf
        echo "conf -> \(ip):\(port)"
        """
    }
    let dirList = receiverAppDirs.joined(separator: " ")
    let script = """
    \(conf)
    if killall -0 secondscreen 2>/dev/null; then
      echo "receiver already running (not restarted)"
    else
      launched=0
      for d in \(dirList); do
        if [ -x "$d/secondscreen" ]; then
          cd "$d" && ./secondscreen >/dev/null 2>&1 &
          echo "receiver launched from $d"
          launched=1
          break
        fi
      done
      [ $launched = 0 ] && echo "receiver not installed"
    fi
    """
    if let out = runProcess(novacom, ["run", "file:///bin/sh"],
                            stdin: script, timeout: 10) {
        log("autolaunch: \(out.trimmingCharacters(in: .whitespacesAndNewlines))")
    } else {
        log("autolaunch: novacom shell failed (device busy?)")
    }
}
