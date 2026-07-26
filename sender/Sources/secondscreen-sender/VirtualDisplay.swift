/* Creates a 1024×768 virtual monitor via the private CGVirtualDisplay
 * API. The OS treats it as a real second display (arrangeable in System
 * Settings, windows land on it) and it becomes the capture source — a
 * true extended desktop, not a mirror.
 *
 * Keep the returned CGVirtualDisplay alive: releasing it unplugs the
 * monitor. Stable vendor/product/serial let macOS remember the user's
 * arrangement across runs. */
import CVDShim
import Foundation
import ScreenCaptureKit

enum VirtualDisplayError: Error, CustomStringConvertible {
    case initFailed
    case applySettingsFailed
    case notCapturable

    var description: String {
        switch self {
        case .initFailed:
            return "CGVirtualDisplay init returned nil (private API changed?)"
        case .applySettingsFailed:
            return "CGVirtualDisplay applySettings failed (private API changed?)"
        case .notCapturable:
            return "virtual display never appeared in ScreenCaptureKit content"
        }
    }
}

func createVirtualDisplay(width: UInt32, height: UInt32,
                          refreshHz: Double) throws -> CGVirtualDisplay {
    let desc = CGVirtualDisplayDescriptor()
    desc.queue = DispatchQueue(label: "secondscreen.virtualdisplay")
    desc.name = "webOS Second Screen"
    desc.maxPixelsWide = width
    desc.maxPixelsHigh = height
    // HP TouchPad panel: 9.7" 4:3
    desc.sizeInMillimeters = CGSize(width: 197, height: 148)
    desc.vendorID = 0x4A57   // "JW"
    desc.productID = 0x7095  // TouchPad's webOS version, why not
    desc.serialNum = 1
    // sRGB primaries
    desc.redPrimary = CGPoint(x: 0.640, y: 0.330)
    desc.greenPrimary = CGPoint(x: 0.300, y: 0.600)
    desc.bluePrimary = CGPoint(x: 0.150, y: 0.060)
    desc.whitePoint = CGPoint(x: 0.3127, y: 0.3290)
    desc.terminationHandler = { _, _ in
        log("virtual display terminated by the system")
    }

    guard let display = CGVirtualDisplay(descriptor: desc) else {
        throw VirtualDisplayError.initFailed
    }
    let settings = CGVirtualDisplaySettings()
    settings.hiDPI = 0
    settings.modes = [CGVirtualDisplayMode(width: width, height: height,
                                           refreshRate: refreshHz)]
    guard display.apply(settings) else {
        throw VirtualDisplayError.applySettingsFailed
    }

    // WindowServer can bring a rapidly re-created display (same identity as
    // one still tearing down) up in a clamped mode (seen once: 768×768).
    // Verify the geometry and re-apply until it sticks.
    for attempt in 0..<8 {
        let b = CGDisplayBounds(display.displayID)
        if Int(b.width) == Int(width) && Int(b.height) == Int(height) { break }
        log("virtual display came up \(Int(b.width))x\(Int(b.height)), re-applying \(width)x\(height) (attempt \(attempt + 1))")
        if attempt >= 2 { _ = display.apply(settings) }
        Thread.sleep(forTimeInterval: 0.5)
    }
    let final = CGDisplayBounds(display.displayID)
    if Int(final.width) != Int(width) || Int(final.height) != Int(height) {
        log("warning: virtual display stuck at \(Int(final.width))x\(Int(final.height)); streaming will letterbox/stretch")
    }

    // the new display takes a moment to reach ScreenCaptureKit's content
    for _ in 0..<20 {
        if let displays = try? CaptureEngine.shareableDisplays(),
           displays.contains(where: { $0.displayID == display.displayID }) {
            return display
        }
        Thread.sleep(forTimeInterval: 0.25)
    }
    throw VirtualDisplayError.notCapturable
}
