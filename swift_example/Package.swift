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
            cSettings: [
                .headerSearchPath("audiograph")
            ],
            linkerSettings: [
                .linkedLibrary("audiograph"),
                .unsafeFlags(["-L", "./audiograph", "-Xlinker", "-rpath", "-Xlinker", "./audiograph"])
            ]
        ),
        .systemLibrary(
            name: "AudioGraph",
            path: "audiograph"
        )
    ]
)