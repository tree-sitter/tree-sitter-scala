// swift-tools-version:5.3
import PackageDescription

let package = Package(
    name: "TreeSitterScala",
    products: [
        .library(name: "TreeSitterScala", targets: ["TreeSitterScala"]),
    ],
    dependencies: [
        .package(url: "https://github.com/ChimeHQ/SwiftTreeSitter", from: "0.8.0"),
    ],
    targets: [
        .target(
            name: "TreeSitterScala",
            dependencies: [],
            path: ".",
            sources: [
                "src/parser.c",
                "src/scanner.c",
            ],
            resources: [
                .copy("queries")
            ],
            publicHeadersPath: "bindings/swift",
            cSettings: [.headerSearchPath("src")]
        ),
        .testTarget(
            name: "TreeSitterScalaTests",
            dependencies: [
                "SwiftTreeSitter",
                "TreeSitterScala",
            ],
            path: "bindings/swift/TreeSitterScalaTests"
        )
    ],
    cLanguageStandard: .c11
)
