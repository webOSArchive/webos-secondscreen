/* TCP server: one client at a time, framed messages per PROTOCOL.md.
 * Capture runs only while a client is connected. */
import AppKit
import Darwin
import Foundation

// Signaled by an NSWorkspace wake observer so the accept-loop backoff wait
// (below) can cut short immediately on wake instead of finishing out
// whatever was left of the original interval — Thread.sleep doesn't count
// time spent in actual system suspend.
private let wakeSemaphore = DispatchSemaphore(value: 0)

private func installWakeObserver() {
    DispatchQueue.main.async {
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main
        ) { _ in wakeSemaphore.signal() }
    }
}

let PROTO_VERSION: UInt8 = 2

/// How often we ask a client to send its `P` heartbeat, and how long we let
/// one go quiet before dropping the session. Both only ever apply to a client
/// that has actually sent a heartbeat — see `clientQuiet` in ClientSession.
let CLIENT_PING_S: UInt8 = 3
let CLIENT_SILENCE_S: Double = 10

func log(_ msg: String) {
    let t = DateFormatter()
    t.dateFormat = "HH:mm:ss"
    print("[\(t.string(from: Date()))] \(msg)")
    fflush(stdout)
}

/// Reads exactly `n` bytes. The client socket carries a short `SO_RCVTIMEO`,
/// so each expiry is a chance to ask `giveUp()` whether this client has gone
/// quiet for too long to still be there; until something arms that check it
/// says no and this blocks indefinitely, exactly as it always did.
private func readFull(_ fd: Int32, _ n: Int, _ giveUp: () -> Bool) -> [UInt8]? {
    var buf = [UInt8](repeating: 0, count: n)
    var got = 0
    let ok = buf.withUnsafeMutableBytes { raw -> Bool in
        while got < n {
            let r = recv(fd, raw.baseAddress!.advanced(by: got), n - got, 0)
            if r < 0 && errno == EINTR { continue }
            if r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) {
                if giveUp() { return false }
                continue
            }
            if r <= 0 { return false }
            got += r
        }
        return true
    }
    return ok ? buf : nil
}

private func sendAll(_ fd: Int32, _ data: Data) -> Bool {
    data.withUnsafeBytes { raw -> Bool in
        var off = 0
        while off < raw.count {
            let n = send(fd, raw.baseAddress!.advanced(by: off), raw.count - off, 0)
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { return false }
            off += n
        }
        return true
    }
}

private func sendMsg(_ fd: Int32, _ type: UInt8, _ payload: Data) -> Bool {
    var msg = Data(capacity: payload.count + 5)
    msg.append(type)
    let n = UInt32(payload.count)
    msg.append(contentsOf: [UInt8(n >> 24 & 0xff), UInt8(n >> 16 & 0xff),
                            UInt8(n >> 8 & 0xff), UInt8(n & 0xff)])
    msg.append(payload)
    return sendAll(fd, msg)
}

/// True when the peer has already closed: a zero-length read at the very head
/// of the stream, or a socket that is already in error. EWOULDBLOCK (nothing
/// has arrived yet) and any real byte both mean it is still there — a live
/// receiver's `H` hello is sent immediately on connect, so anything readable
/// this early is a client, not leftovers.
private func peerGone(_ fd: Int32) -> Bool {
    let flags = fcntl(fd, F_GETFL, 0)
    guard flags >= 0, fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0 else { return false }
    defer { _ = fcntl(fd, F_SETFL, flags) }   // the session wants it blocking
    var b: UInt8 = 0
    let n = recv(fd, &b, 1, MSG_PEEK)
    if n == 0 { return true }
    if n > 0 { return false }
    return errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR
}

/// Outcome of a session, for the caller's accept-loop backoff decision. The
/// distinction that matters there is *which end* ended it: capture going away
/// under us is the sender's problem to back off over, a client hanging up is
/// not.
struct SessionResult {
    let framesSent: Int
    let captureEnded: Bool
}

final class ClientSession {
    private let fd: Int32
    private let capture: CaptureEngine
    private let encoder: JPEGEncoder
    private let injector: Injector
    // frames go out from run() while the discovery reply goes out from
    // readLoop() — without this the two send()s could interleave and
    // shred the framing
    private let sendLock = NSLock()
    // Both touched only by the reader thread (readLoop and the giveUp check
    // it passes into readFull both run there), so they need no lock.
    private var lastClientRx = Date()
    private var heartbeatArmed = false

    private func sendFramed(_ type: UInt8, _ payload: Data) -> Bool {
        sendLock.lock()
        defer { sendLock.unlock() }
        return sendMsg(fd, type, payload)
    }

    init(fd: Int32, capture: CaptureEngine, encoder: JPEGEncoder, injector: Injector) {
        self.fd = fd
        self.capture = capture
        self.encoder = encoder
        self.injector = injector
    }

    /// True once a client that *does* send heartbeats has stopped. Never true
    /// for a client that has sent none: a pre-v2 receiver is silent on the
    /// back-channel unless the user touches the screen, and dropping those
    /// would be a regression, so the check arms itself on the first `P` and
    /// stays disarmed forever otherwise.
    private func clientQuiet() -> Bool {
        guard heartbeatArmed else { return false }
        guard Date().timeIntervalSince(lastClientRx) > CLIENT_SILENCE_S else { return false }
        log("client silent for \(Int(CLIENT_SILENCE_S))s, dropping the link")
        return true
    }

    /// Reader thread: touch/key/hello messages from the receiver.
    private func readLoop() {
        while true {
            guard let hdr = readFull(fd, 5, clientQuiet) else { break }
            let len = Int(hdr[1]) << 24 | Int(hdr[2]) << 16 | Int(hdr[3]) << 8 | Int(hdr[4])
            if len > 1 << 20 {
                log("client: bogus payload length \(len), dropping")
                break
            }
            var payload = [UInt8]()
            if len > 0 {
                guard let p = readFull(fd, len, clientQuiet) else { break }
                payload = p
            }
            lastClientRx = Date()
            switch hdr[0] {
            case UInt8(ascii: "P"):
                // Heartbeat from a v2 receiver. The first one is what tells us
                // this client will keep speaking, so its silence becomes
                // meaningful — that is the whole point of the message.
                if !heartbeatArmed {
                    heartbeatArmed = true
                    log("client sends heartbeats; dead-client detection armed")
                }
            case UInt8(ascii: "Q"):
                // Discovery probe: a receiver that found us by sweeping the
                // subnet asking us to prove we're a second-screen sender.
                guard payload.count >= 4,
                      Array(payload[0..<4]) == Array("SSCR".utf8) else {
                    log("client: discovery probe with bad magic, ignoring")
                    break
                }
                let name = Host.current().localizedName ?? "Mac"
                let nameBytes = Array(name.utf8.prefix(63))
                var reply = Data("SSND".utf8)
                reply.append(PROTO_VERSION)
                reply.append(UInt8(nameBytes.count))
                reply.append(contentsOf: nameBytes)
                _ = sendFramed(UInt8(ascii: "Y"), reply)
                // Re-advertise behind the reply. A sweeping receiver reads
                // this socket with its own handshake loop until the `Y`
                // arrives, which means it has already eaten the advert we
                // sent on connect — and it adopts this socket rather than
                // redialling, so without a second one that whole session
                // would run without a heartbeat.
                _ = sendAdvert()
                log("discovery probe answered (\(name))")
            case UInt8(ascii: "H") where payload.count >= 5:
                let w = Int(payload[0]) << 8 | Int(payload[1])
                let h = Int(payload[2]) << 8 | Int(payload[3])
                log("client hello: \(w)x\(h) protocol v\(payload[4])")
            case UInt8(ascii: "T") where payload.count >= 6:
                injector.touch(finger: Int(payload[0]), action: Int(payload[1]),
                               x: Int(payload[2]) << 8 | Int(payload[3]),
                               y: Int(payload[4]) << 8 | Int(payload[5]))
            case UInt8(ascii: "K") where payload.count >= 3:
                injector.key(sym: Int(payload[0]) << 8 | Int(payload[1]),
                             down: payload[2] != 0)
            default:
                log("client: unknown message '\(Character(UnicodeScalar(hdr[0])))' len=\(len)")
            }
        }
        // wake the sender loop if it's blocked on a quiet screen
        shutdown(fd, SHUT_RDWR)
    }

    /// What we are, and what we want from the client. A pre-v2 receiver drains
    /// the payload by length and ignores the type (every released one does,
    /// back to the first), so this costs it nothing; only a receiver that
    /// understands `V` starts heartbeating, which is why an old one never sees
    /// an unknown-message storm from us.
    private func sendAdvert() -> Bool {
        sendFramed(UInt8(ascii: "V"), Data([PROTO_VERSION, CLIENT_PING_S]))
    }

    @discardableResult
    func run() -> SessionResult {
        _ = sendAdvert()          // before any frame

        let reader = Thread { self.readLoop() }  // strong: keeps the session alive until recv unblocks
        reader.name = "client-reader"
        reader.start()

        var sent = 0
        var encodeMS = 0.0
        var bytes = 0
        var captureEnded = false
        var t0 = Date()
        loop: while true {
            switch capture.mailbox.take(timeout: 3.0) {
            case .closed:
                log("capture ended")
                captureEnded = true
                break loop
            case .timeout:
                // screen is static (SCK sends nothing) — keepalive doubles
                // as dead-client detection
                if !sendFramed(UInt8(ascii: "P"), Data()) { break loop }
            case .frame(let pb):
                let te = Date()
                guard let jpeg = encoder.encode(pb) else { continue }
                encodeMS += Date().timeIntervalSince(te) * 1000
                if !sendFramed(UInt8(ascii: "J"), jpeg) { break loop }
                sent += 1
                bytes += jpeg.count
                if sent % 100 == 0 {
                    let dt = Date().timeIntervalSince(t0)
                    let mbps = Double(bytes) * 8 / dt / 1_000_000
                    log(String(format: "%d frames: %.1f fps, encode %.1f ms/frame, %.1f Mbps (last %d KB)",
                               sent, 100 / dt, encodeMS / 100, mbps, jpeg.count / 1024))
                    encodeMS = 0
                    bytes = 0
                    t0 = Date()
                }
            }
        }
        shutdown(fd, SHUT_RDWR)  // unblock the reader thread if it's still in recv
        log("client dropped (\(sent) frames sent)")
        return SessionResult(framesSent: sent, captureEnded: captureEnded)
    }
}

func runServer(port: UInt16, fps: Int, quality: Double, target: CaptureTarget, dryRun: Bool) -> Never {
    guard let encoder = JPEGEncoder(quality: quality) else {
        log("fatal: cannot create JPEG encoder")
        exit(1)
    }

    let sfd = socket(AF_INET, SOCK_STREAM, 0)
    guard sfd >= 0 else {
        log("fatal: socket: \(String(cString: strerror(errno)))")
        exit(1)
    }
    var one: Int32 = 1
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))

    var addr = sockaddr_in()
    addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = port.bigEndian
    addr.sin_addr.s_addr = INADDR_ANY
    let bound = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(sfd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
    }
    // Backlog 4, not 1: still one client at a time (the accept loop below is
    // what enforces that), but a receiver redialling while we are still busy
    // with the link it is replacing gets queued instead of having its SYN
    // dropped — which costs it a full dial timeout and a pointless subnet
    // sweep before it can try again. Anything stale in the queue is dropped
    // in a microsecond by the peerGone() check.
    guard bound == 0, listen(sfd, 4) == 0 else {
        log("fatal: bind/listen on :\(port): \(String(cString: strerror(errno)))")
        exit(1)
    }
    log("listening on :\(port) (\(fps) fps cap, quality \(quality)\(dryRun ? ", DRY-RUN injection" : ""))")
    installWakeObserver()

    // Backoff for the accept-and-drop churn: when capture.start() fails
    // (e.g. the virtual display vanished under display sleep), the kernel
    // will happily keep completing handshakes forever. Slow down accept()
    // instead of hammering a display that isn't coming back this second.
    let backoffSchedule: [Double] = [2, 4, 8, 16, 30]
    var consecutiveUnproductive = 0

    while true {
        var peer = sockaddr_in()
        var peerLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        let cfd = withUnsafeMutablePointer(to: &peer) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                accept(sfd, $0, &peerLen)
            }
        }
        guard cfd >= 0 else { continue }
        var ip = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &peer.sin_addr, &ip, socklen_t(INET_ADDRSTRLEN))

        // While the previous session was running, the kernel completed this
        // handshake out of the listen backlog on its own. The receiver drops
        // a socket that says nothing within a few seconds, so by the time we
        // get here it has usually given up and reconnected — and standing up
        // a whole capture just to fail the first send() puts that fresh
        // connection behind a display start/stop for nothing.
        if peerGone(cfd) {
            log("stale queued client from \(String(cString: ip)); dropping")
            close(cfd)
            continue
        }
        log("client connected from \(String(cString: ip))")

        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        // A peer that vanishes without a FIN/RST (WiFi drop, dead air) leaves
        // send()/recv() blocked indefinitely without this — the P-ping "dead
        // client detection" only works if a failed send() ever surfaces.
        // Keepalive probes force that within ~14s instead of however long the
        // OS takes to give up retransmitting on its own.
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, socklen_t(MemoryLayout<Int32>.size))
        var keepIdle: Int32 = 5
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPALIVE, &keepIdle, socklen_t(MemoryLayout<Int32>.size))
        var keepInterval: Int32 = 3
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, socklen_t(MemoryLayout<Int32>.size))
        var keepCount: Int32 = 3
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, socklen_t(MemoryLayout<Int32>.size))
        // keep the kernel send queue short (~2 frames) so backpressure reaches
        // the latest-frame-wins mailbox instead of autotuning into MBs of latency
        var sndbuf: Int32 = 128 * 1024
        setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        // Not a deadline — readFull() loops straight over an expiry. It exists
        // so the reader thread surfaces once a second to check a heartbeating
        // client for silence, instead of sitting in recv() forever.
        var rcvtimeo = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, socklen_t(MemoryLayout<timeval>.size))

        let capture = CaptureEngine(fps: fps, target: target)
        var result: SessionResult? = nil
        do {
            try capture.start()
            log("capturing \(capture.display.width)x\(capture.display.height) → \(capture.outW)x\(capture.outH)")
            StatusUI.shared.setState("Streaming to \(String(cString: ip))")
            let injector = Injector(displayID: capture.display.displayID,
                                    contentW: capture.outW, contentH: capture.outH,
                                    dryRun: dryRun)
            result = ClientSession(fd: cfd, capture: capture, encoder: encoder,
                                    injector: injector).run()
            capture.stop()
        } catch {
            log("capture start failed: \(error)")
        }
        close(cfd)

        // Back off only for a capture that will not produce anything: start()
        // threw, or capture ended under us without ever handing over a frame.
        // A session that ends because the *client* went away is a different
        // animal and must not count — a sweep's discovery probe and, far more
        // often, the receiver's own reconnect arriving while we were still busy
        // with the link it replaces both end with zero frames. Counting those
        // stopped accept() at exactly the moment somebody was trying to come
        // back, and the two ends then backed off past each other for a minute.
        let unproductive = result.map { $0.captureEnded && $0.framesSent == 0 } ?? true
        consecutiveUnproductive = unproductive ? consecutiveUnproductive + 1 : 0

        log("waiting for next client")
        if unproductive {
            let wait = backoffSchedule[min(consecutiveUnproductive, backoffSchedule.count) - 1]
            log("capture unavailable (\(consecutiveUnproductive) in a row); waiting \(wait)s before accepting")
            StatusUI.shared.setState("Capture unavailable — retrying in \(Int(wait))s")
            // Drain any stale signal left over from a previous wait, then block for
            // either the full backoff or a wake, whichever comes first.
            while wakeSemaphore.wait(timeout: .now()) == .success {}
            if wakeSemaphore.wait(timeout: .now() + wait) == .success {
                log("woke from sleep; resuming accept loop immediately")
            }
        } else {
            StatusUI.shared.setState("Waiting for TouchPad…")
        }
    }
}
