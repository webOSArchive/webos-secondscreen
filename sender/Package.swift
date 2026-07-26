// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "secondscreen-sender",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "CVDShim", path: "Sources/CVDShim"),
        .executableTarget(
            name: "secondscreen-sender",
            dependencies: ["CVDShim"],
            path: "Sources/secondscreen-sender",
            swiftSettings: [.swiftLanguageMode(.v5)],
            linkerSettings: [.linkedFramework("CoreGraphics")]
        ),
    ]
)
