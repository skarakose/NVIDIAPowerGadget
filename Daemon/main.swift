// Copyright (C) 2026 Serhat
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import NVFanCore
import Darwin

// fanctld — fan-curve daemon running as root.
//
// Since driver 2.0.0, fan writes (COOLER_SET) require administrator on the
// RM side (os_is_administrator now reads the real uid). So a root
// LaunchDaemon owns the writes; the menu-bar app only monitors and writes
// the curve to the shared config. The config deliberately lives outside
// user homes, under /Library/Application Support (installed root:admin
// 775, so writing requires admin).

private struct FanCurvePoint: Codable {
    var temperature: Int
    var percent: Int
}

private struct StoredFanCurve: Codable {
    var points: [FanCurvePoint]
}

/// Shared curve config. Outside user homes: the daemon runs as root and
/// reads the setting from the system location.
private func sharedConfigURL() -> URL {
    URL(fileURLWithPath: "/Library/Application Support/NVIDIAPowerGadget/curve.json")
}

private func loadPoints() -> [FanCurvePoint] {
    guard let data = try? Data(contentsOf: sharedConfigURL()),
          let stored = try? JSONDecoder().decode(StoredFanCurve.self, from: data)
    else { return [] }
    return stored.points.sorted { $0.temperature < $1.temperature }
}

private func speed(for temperature: Int, points: [FanCurvePoint]) -> Int? {
    guard !points.isEmpty else { return nil }
    if temperature <= points[0].temperature { return points[0].percent }
    if temperature >= points[points.count - 1].temperature { return points[points.count - 1].percent }
    var selected = points[0].percent
    for point in points where temperature >= point.temperature {
        selected = point.percent
    }
    return selected
}

private func readTemperature() -> Int? {
    var status = nvfan_status_t()
    guard nvfan_read_status(&status) == 0 else { return nil }
    return Int(status.temperatureC)
}

private var lastApplied = -1

private func tick() {
    guard let temp = readTemperature() else { return }
    guard let target = speed(for: temp, points: loadPoints()) else { return }
    guard target != lastApplied else { return }
    if nvfan_set_fan_percent(Int32(target)) == 0 {
        lastApplied = target
        NSLog("fanctld: %d°C -> %d%%", temp, target)
    } else {
        NSLog("fanctld: set to %d%% failed", target)
    }
}

while true {
    autoreleasepool { tick() }
    Thread.sleep(forTimeInterval: 2.0)
}
