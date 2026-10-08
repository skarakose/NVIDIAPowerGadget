// swift-tools-version: 5.7
// Copyright (C) 2026 Serhat
// SPDX-License-Identifier: GPL-3.0-or-later
import PackageDescription

let package = Package(
    name: "NVIDIAPowerGadget",
    platforms: [.macOS(.v10_13)],
    products: [
        .executable(name: "NVIDIAPowerGadget", targets: ["NVIDIAPowerGadget"])
    ],
    targets: [
        .target(
            name: "NVFanCore",
            path: ".",
            sources: ["nvfan.c"],
            publicHeadersPath: ".",
            cSettings: [.define("NVFAN_LIBRARY")],
            linkerSettings: [
                .linkedFramework("IOKit"),
                .linkedFramework("CoreFoundation")
            ]
        ),
        .executableTarget(
            name: "NVIDIAPowerGadget",
            dependencies: ["NVFanCore"],
            path: "App"
        ),
        .executableTarget(
            name: "fanctld",
            dependencies: ["NVFanCore"],
            path: "Daemon"
        )
    ]
)
