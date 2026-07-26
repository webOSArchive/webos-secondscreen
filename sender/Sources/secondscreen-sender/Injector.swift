/* Touch/key → CGEvent injection. Coordinates arrive in the TouchPad's
 * 1024×768 space; map through the letterbox content rect back to the
 * captured display's global bounds.
 *
 * webOS SDL quirk (PROTOCOL.md): a `move` arrives immediately before
 * every `down` — moves are position-only, press/release happen strictly
 * on down/up, so the quirk is harmless here. */
import CoreGraphics
import Foundation

final class Injector {
    private let displayID: CGDirectDisplayID
    private let contentRect: CGRect   // content within the 1024×768 canvas
    private let src = CGEventSource(stateID: .hidSystemState)
    private let dryRun: Bool

    private var buttonDown = false
    private var lastClickTime: TimeInterval = 0
    private var lastClickPos = CGPoint.zero
    private var clickCount: Int64 = 1

    init(displayID: CGDirectDisplayID, contentW: Int, contentH: Int, dryRun: Bool) {
        self.displayID = displayID
        contentRect = CGRect(x: Double(JPEGEncoder.canvasW - contentW) / 2,
                             y: Double(JPEGEncoder.canvasH - contentH) / 2,
                             width: Double(contentW), height: Double(contentH))
        self.dryRun = dryRun
    }

    private func map(_ x: Int, _ y: Int) -> CGPoint {
        // looked up per event (cheap): the user can rearrange the display
        // in System Settings mid-session, which moves its global origin
        let displayFrame = CGDisplayBounds(displayID)
        let fx = min(max((Double(x) - contentRect.minX) / contentRect.width, 0), 1)
        let fy = min(max((Double(y) - contentRect.minY) / contentRect.height, 0), 1)
        // clamp just inside the far edges so events stay on this display
        return CGPoint(x: displayFrame.minX + min(fx * displayFrame.width, displayFrame.width - 1),
                       y: displayFrame.minY + min(fy * displayFrame.height, displayFrame.height - 1))
    }

    func touch(finger: Int, action: Int, x: Int, y: Int) {
        guard finger == 0 else { return }  // finger 0 drives the mouse; gestures are Phase 2
        guard !CGDisplayBounds(displayID).isEmpty else { return }  // display vanished
        let p = map(x, y)
        switch action {
        case 1:  // move: position only
            post(buttonDown ? .leftMouseDragged : .mouseMoved, at: p,
                 click: buttonDown ? clickCount : 0)
        case 0:  // down
            let now = Date().timeIntervalSince1970
            if now - lastClickTime < 0.4,
               hypot(p.x - lastClickPos.x, p.y - lastClickPos.y) < 10 {
                clickCount += 1  // double/triple tap
            } else {
                clickCount = 1
            }
            lastClickTime = now
            lastClickPos = p
            buttonDown = true
            post(.leftMouseDown, at: p, click: clickCount)
        case 2:  // up
            buttonDown = false
            post(.leftMouseUp, at: p, click: clickCount)
        default:
            break
        }
    }

    private func post(_ type: CGEventType, at p: CGPoint, click: Int64) {
        if dryRun {
            log("inject \(type.rawValue) at (\(Int(p.x)),\(Int(p.y))) click=\(click)")
            return
        }
        guard let e = CGEvent(mouseEventSource: src, mouseType: type,
                              mouseCursorPosition: p, mouseButton: .left) else { return }
        if click > 0 { e.setIntegerValueField(.mouseEventClickState, value: click) }
        e.post(tap: .cghidEventTap)
    }

    // SDL 1.2 keysym → mac virtual keycode for non-printables
    private static let specialKeys: [Int: CGKeyCode] = [
        8: 51,    // backspace → delete
        9: 48,    // tab
        13: 36,   // return
        27: 53,   // escape (receiver uses it to quit; here for completeness)
        127: 117, // delete → forward delete
        273: 126, // up
        274: 125, // down
        275: 124, // right
        276: 123, // left
        278: 115, // home
        279: 119, // end
        280: 116, // page up
        281: 121, // page down
    ]

    func key(sym: Int, down: Bool) {
        if dryRun {
            log("inject key sym=\(sym) \(down ? "down" : "up")")
            return
        }
        if let code = Self.specialKeys[sym] {
            CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: down)?
                .post(tap: .cghidEventTap)
        } else if sym >= 32, sym < 127 {
            // printable ASCII: type via unicode string, no keycode mapping needed
            guard let e = CGEvent(keyboardEventSource: src, virtualKey: 0,
                                  keyDown: down) else { return }
            var ch = [UniChar(sym)]
            e.keyboardSetUnicodeString(stringLength: 1, unicodeString: &ch)
            e.post(tap: .cghidEventTap)
        }
    }
}
