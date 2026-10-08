// Copyright (C) 2026 Serhat
// SPDX-License-Identifier: GPL-3.0-or-later
import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusBar: NVIDIAStatusBarController!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        statusBar = NVIDIAStatusBarController()
    }
}

let application = NSApplication.shared
let delegate = AppDelegate()
application.delegate = delegate
application.run()
