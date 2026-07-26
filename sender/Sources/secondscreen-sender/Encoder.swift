/* BGRA pixel buffer → baseline JPEG, letterboxed onto a black 1024×768
 * canvas so the receiver (which stretches every frame fullscreen) never
 * distorts the aspect ratio. */
import CoreGraphics
import CoreVideo
import Foundation
import ImageIO
import UniformTypeIdentifiers

final class JPEGEncoder {
    static let canvasW = 1024
    static let canvasH = 768

    private let quality: Double
    private let canvas: CGContext

    init?(quality: Double) {
        self.quality = quality
        guard let c = CGContext(
            data: nil, width: Self.canvasW, height: Self.canvasH,
            bitsPerComponent: 8, bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        c.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
        c.interpolationQuality = .none  // capture is already scaled; 1:1 blit
        canvas = c
    }

    func encode(_ pb: CVPixelBuffer) -> Data? {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        let w = CVPixelBufferGetWidth(pb)
        let h = CVPixelBufferGetHeight(pb)
        var frame: CGImage?
        if let base = CVPixelBufferGetBaseAddress(pb),
           let src = CGContext(
               data: base, width: w, height: h, bitsPerComponent: 8,
               bytesPerRow: CVPixelBufferGetBytesPerRow(pb),
               space: CGColorSpaceCreateDeviceRGB(),
               bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                   | CGBitmapInfo.byteOrder32Little.rawValue) {
            frame = src.makeImage()  // copies; safe to unlock after
        }
        CVPixelBufferUnlockBaseAddress(pb, .readOnly)
        guard var img = frame else { return nil }

        if w != Self.canvasW || h != Self.canvasH {
            canvas.fill(CGRect(x: 0, y: 0, width: Self.canvasW, height: Self.canvasH))
            canvas.draw(img, in: CGRect(x: (Self.canvasW - w) / 2,
                                        y: (Self.canvasH - h) / 2,
                                        width: w, height: h))
            guard let composed = canvas.makeImage() else { return nil }
            img = composed
        }

        let out = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(
            out, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, img, [
            kCGImageDestinationLossyCompressionQuality: quality,
        ] as CFDictionary)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return out as Data
    }
}
