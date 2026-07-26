/* TCP server: one client at a time, framed messages per PROTOCOL.md.
 * Capture runs only while a client is connected. */
import Darwin
import Foundation

func log(_ msg: String) {
    let t = DateFormatter()
    t.dateFormat = "HH:mm:ss"
    print("[\(t.string(from: Date()))] \(msg)")
    fflush(stdout)
}

private func readFull(_ fd: Int32, _ n: Int) -> [UInt8]? {
    var buf = [UInt8](repeating: 0, count: n)
    var got = 0
    let ok = buf.withUnsafeMutableBytes { raw -> Bool in
        while got < n {
            let r = recv(fd, raw.baseAddress!.advanced(by: got), n - got, 0)
            if r < 0 && errno == EINTR { continue }
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

final class ClientSession {
    private let fd: Int32
    private let capture: CaptureEngine
    private let encoder: JPEGEncoder
    private let injector: Injector

    init(fd: Int32, capture: CaptureEngine, encoder: JPEGEncoder, injector: Injector) {
        self.fd = fd
        self.capture = capture
        self.encoder = encoder
        self.injector = injector
    }

    /// Reader thread: touch/key/hello messages from the receiver.
    private func readLoop() {
        while true {
            guard let hdr = readFull(fd, 5) else { break }
            let len = Int(hdr[1]) << 24 | Int(hdr[2]) << 16 | Int(hdr[3]) << 8 | Int(hdr[4])
            if len > 1 << 20 {
                log("client: bogus payload length \(len), dropping")
                break
            }
            var payload = [UInt8]()
            if len > 0 {
                guard let p = readFull(fd, len) else { break }
                payload = p
            }
            switch hdr[0] {
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

    func run() {
        let reader = Thread { self.readLoop() }  // strong: keeps the session alive until recv unblocks
        reader.name = "client-reader"
        reader.start()

        var sent = 0
        var encodeMS = 0.0
        var bytes = 0
        var t0 = Date()
        loop: while true {
            switch capture.mailbox.take(timeout: 3.0) {
            case .closed:
                log("capture ended")
                break loop
            case .timeout:
                // screen is static (SCK sends nothing) — keepalive doubles
                // as dead-client detection
                if !sendMsg(fd, UInt8(ascii: "P"), Data()) { break loop }
            case .frame(let pb):
                let te = Date()
                guard let jpeg = encoder.encode(pb) else { continue }
                encodeMS += Date().timeIntervalSince(te) * 1000
                if !sendMsg(fd, UInt8(ascii: "J"), jpeg) { break loop }
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
    guard bound == 0, listen(sfd, 1) == 0 else {
        log("fatal: bind/listen on :\(port): \(String(cString: strerror(errno)))")
        exit(1)
    }
    log("listening on :\(port) (\(fps) fps cap, quality \(quality)\(dryRun ? ", DRY-RUN injection" : ""))")

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
        log("client connected from \(String(cString: ip))")
        StatusUI.shared.setState("Streaming to \(String(cString: ip))")

        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        // keep the kernel send queue short (~2 frames) so backpressure reaches
        // the latest-frame-wins mailbox instead of autotuning into MBs of latency
        var sndbuf: Int32 = 128 * 1024
        setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))

        let capture = CaptureEngine(fps: fps, target: target)
        do {
            try capture.start()
            log("capturing \(capture.display.width)x\(capture.display.height) → \(capture.outW)x\(capture.outH)")
            let injector = Injector(displayID: capture.display.displayID,
                                    contentW: capture.outW, contentH: capture.outH,
                                    dryRun: dryRun)
            ClientSession(fd: cfd, capture: capture, encoder: encoder,
                          injector: injector).run()
            capture.stop()
        } catch {
            log("capture start failed: \(error)")
        }
        close(cfd)
        log("waiting for next client")
        StatusUI.shared.setState("Waiting for TouchPad…")
    }
}
