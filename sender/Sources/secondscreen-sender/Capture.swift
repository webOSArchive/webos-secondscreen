/* ScreenCaptureKit capture: display frames scaled (aspect-fit) into a
 * ≤1024×768 BGRA pixel buffer, delivered latest-frame-wins through a
 * 1-slot mailbox (see receiver/PROTOCOL.md). */
import Foundation
import CoreMedia
import CoreVideo
import ScreenCaptureKit

enum MailboxItem {
    case frame(CVPixelBuffer)
    case timeout
    case closed
}

/// 1-slot mailbox: the capture callback overwrites, the sender drains.
/// Stale frames are dropped here rather than queueing into TCP backpressure.
final class FrameMailbox {
    private let cond = NSCondition()
    private var buffer: CVPixelBuffer?
    private var closed = false

    func put(_ pb: CVPixelBuffer) {
        cond.lock()
        buffer = pb
        cond.signal()
        cond.unlock()
    }

    func close() {
        cond.lock()
        closed = true
        cond.signal()
        cond.unlock()
    }

    func reopen() {
        cond.lock()
        closed = false
        buffer = nil
        cond.unlock()
    }

    func take(timeout: TimeInterval) -> MailboxItem {
        cond.lock()
        defer { cond.unlock() }
        let deadline = Date(timeIntervalSinceNow: timeout)
        while buffer == nil && !closed {
            if !cond.wait(until: deadline) { return .timeout }
        }
        if closed { return .closed }
        let b = buffer!
        buffer = nil
        return .frame(b)
    }
}

enum CaptureError: Error, CustomStringConvertible {
    case shareableContent(String)
    case noDisplay(Int, available: Int)
    case displayGone(CGDirectDisplayID)

    var description: String {
        switch self {
        case .shareableContent(let msg):
            return "cannot enumerate displays: \(msg) (Screen Recording permission missing?)"
        case .noDisplay(let idx, let n):
            return "display index \(idx) out of range (\(n) available)"
        case .displayGone(let id):
            return "display \(id) is no longer available"
        }
    }
}

enum CaptureTarget {
    case index(Int)                 // mirror an existing display
    case id(CGDirectDisplayID)      // the virtual second screen
}

final class CaptureEngine: NSObject, SCStreamOutput, SCStreamDelegate {
    let mailbox = FrameMailbox()
    private(set) var display: SCDisplay!
    private(set) var outW = 0
    private(set) var outH = 0

    private var stream: SCStream?
    private let queue = DispatchQueue(label: "secondscreen.capture")
    private let fps: Int
    private let target: CaptureTarget

    init(fps: Int, target: CaptureTarget) {
        self.fps = fps
        self.target = target
    }

    static func shareableDisplays() throws -> [SCDisplay] {
        var content: SCShareableContent?
        var err: Error?
        let sem = DispatchSemaphore(value: 0)
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { c, e in
            content = c
            err = e
            sem.signal()
        }
        sem.wait()
        guard let content else {
            throw CaptureError.shareableContent(err?.localizedDescription ?? "unknown error")
        }
        return content.displays
    }

    func start() throws {
        let displays = try Self.shareableDisplays()
        switch target {
        case .index(let idx):
            guard idx < displays.count else {
                throw CaptureError.noDisplay(idx, available: displays.count)
            }
            display = displays[idx]
            // aspect-fit within the TouchPad's 1024x768; the encoder letterboxes
            let dw = Double(display.width), dh = Double(display.height)
            let scale = min(1024.0 / dw, 768.0 / dh)
            outW = min(1024, Int((dw * scale).rounded()) & ~1)
            outH = min(768, Int((dh * scale).rounded()) & ~1)
        case .id(let did):
            guard let d = displays.first(where: { $0.displayID == did }) else {
                throw CaptureError.displayGone(did)
            }
            display = d
            // our virtual display IS 1024x768 — hard-code rather than trust
            // SCDisplay's report, which can lag mode changes on fast restarts
            outW = 1024
            outH = 768
        }

        let cfg = SCStreamConfiguration()
        cfg.width = outW
        cfg.height = outH
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        cfg.queueDepth = 6
        cfg.showsCursor = true

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let s = SCStream(filter: filter, configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)

        var err: Error?
        let sem = DispatchSemaphore(value: 0)
        s.startCapture { e in
            err = e
            sem.signal()
        }
        sem.wait()
        if let err { throw err }
        stream = s
        mailbox.reopen()
    }

    func stop() {
        mailbox.close()
        guard let s = stream else { return }
        stream = nil
        let sem = DispatchSemaphore(value: 0)
        s.stopCapture { _ in sem.signal() }
        sem.wait()
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen, sb.isValid,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(
                  sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let statusRaw = attachments.first?[.status] as? Int,
              SCFrameStatus(rawValue: statusRaw) == .complete,
              let pb = CMSampleBufferGetImageBuffer(sb)
        else { return }
        mailbox.put(pb)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("capture stopped: \(error.localizedDescription)")
        mailbox.close()
    }
}
