// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "AudioGraphExample",
    platforms: [
        .macOS(.v10_15)
    ],
    targets: [
        .executableTarget(
            name: "AudioGraphExample",
            dependencies: ["AudioGraph"],
            linkerSettings: [
                .linkedLibrary("audiograph"),
                .unsafeFlags(["-L", "./audiograph"])
            ]
        ),
        .systemLibrary(
            name: "AudioGraph",
            path: "audiograph"
        )
    ]
)