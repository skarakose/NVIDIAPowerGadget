// Copyright (C) 2026 Serhat
// SPDX-License-Identifier: GPL-3.0-or-later
import Cocoa
import NVFanCore

private struct FanCurvePoint {
    var temperature: Int
    var percent: Int
}

private struct StoredFanCurve: Codable {
    var points: [FanCurvePoint]
}

extension FanCurvePoint: Codable {}

private final class StatusItemView: NSView {
    var temperature = 0
    var fanRPMs: [Int] = []

    override var intrinsicContentSize: NSSize {
        NSSize(width: 108, height: 22)
    }

    override func draw(_ dirtyRect: NSRect) {
        let font = NSFont.systemFont(ofSize: 11, weight: .medium)
        let valueFont = NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .semibold)
        let color = NSColor.labelColor
        let temp = NSAttributedString(
            string: "\(temperature)°C",
            attributes: [.font: font, .foregroundColor: color]
        )
        drawVerticallyCentered(temp, in: NSRect(x: 0, y: 0, width: 43, height: bounds.height))

        let values = fanRPMs.isEmpty ? ["--"] : fanRPMs.prefix(2).map(String.init)
        for (index, value) in values.enumerated() {
            let string = NSAttributedString(
                string: value,
                attributes: [.font: valueFont, .foregroundColor: color]
            )
            let size = string.size()
            string.draw(at: NSPoint(
                x: 56 - size.width / 2,
                y: values.count == 1
                    ? (bounds.height - size.height) / 2
                    : (index == 0 ? bounds.height / 2 : 1)
            ))
        }

        let unit = NSAttributedString(
            string: "RPM",
            attributes: [.font: NSFont.systemFont(ofSize: 9, weight: .medium),
                         .foregroundColor: color]
        )
        let unitSize = unit.size()
        unit.draw(at: NSPoint(
            x: 76,
            y: (bounds.height - unitSize.height) / 2
        ))
    }

    private func drawVerticallyCentered(_ text: NSAttributedString, in rect: NSRect) {
        let size = text.size()
        text.draw(at: NSPoint(
            x: rect.midX - size.width / 2,
            y: rect.midY - size.height / 2
        ))
    }
}

private final class CurveViewController: NSViewController, NSTextFieldDelegate {
    var onPointChanged: ((Int, Int) -> Void)?
    var onPointEdited: ((Int, Int, Int) -> Void)?
    var onAddPoint: (() -> Void)?
    var onRemovePoint: ((Int) -> Void)?
    var points: [FanCurvePoint] = []
    private var sliders: [NSSlider] = []
    private var temperatureFields: [NSTextField] = []
    private var percentFields: [NSTextField] = []
    private var removeButtons: [NSButton] = []
    private let pointsContainer = NSView(frame: NSRect(x: 0, y: 40, width: 320, height: 202))

    override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 300))
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        let title = NSTextField(labelWithString: "NVIDIA Fan Curve")
        title.font = NSFont.systemFont(ofSize: 15, weight: .semibold)
        title.frame = NSRect(x: 20, y: 270, width: 280, height: 20)
        view.addSubview(title)

        let temperatureHeader = NSTextField(labelWithString: "Temp °C")
        temperatureHeader.font = NSFont.systemFont(ofSize: 10, weight: .semibold)
        temperatureHeader.alignment = .center
        temperatureHeader.frame = NSRect(x: 12, y: 242, width: 54, height: 16)
        view.addSubview(temperatureHeader)

        let percentHeader = NSTextField(labelWithString: "Fan (%)")
        percentHeader.font = NSFont.systemFont(ofSize: 10, weight: .semibold)
        percentHeader.alignment = .center
        percentHeader.frame = NSRect(x: 212, y: 242, width: 52, height: 16)
        view.addSubview(percentHeader)
        view.addSubview(pointsContainer)

        let add = NSButton(title: "+ Add point", target: self, action: #selector(addPoint))
        add.bezelStyle = .rounded
        add.frame = NSRect(x: 20, y: 16, width: 100, height: 24)
        view.addSubview(add)
        rebuildPoints()
    }

    private func rebuildPoints() {
        pointsContainer.subviews.forEach { $0.removeFromSuperview() }
        sliders.removeAll()
        temperatureFields.removeAll()
        percentFields.removeAll()
        removeButtons.removeAll()

        let availableHeight = max(30, Int(pointsContainer.bounds.height))
        let rowHeight = max(28, min(38, availableHeight / max(points.count, 1)))
        for (index, point) in points.enumerated() {
            let y = Int(pointsContainer.bounds.height) - (index + 1) * rowHeight
            let row = NSView(frame: NSRect(x: 0, y: y, width: Int(pointsContainer.bounds.width), height: rowHeight))
            pointsContainer.addSubview(row)

            let temperature = editableField(value: "\(point.temperature)", placeholder: "°C", tag: index,
                                            identifier: "temperature")
            temperature.stringValue = "\(point.temperature)°C"
            temperature.frame = NSRect(x: 20, y: 2, width: 38, height: 20)
            row.addSubview(temperature)
            temperatureFields.append(temperature)

            let slider = NSSlider(value: Double(point.percent), minValue: 30, maxValue: 100,
                                  target: self, action: #selector(sliderChanged(_:)))
            slider.tag = index
            slider.frame = NSRect(x: 66, y: 0, width: 145, height: 20)
            row.addSubview(slider)
            sliders.append(slider)

            let percent = editableField(value: "\(point.percent)", placeholder: "%", tag: index,
                                        identifier: "percent")
            percent.stringValue = "\(point.percent)%"
            percent.frame = NSRect(x: 218, y: 2, width: 38, height: 20)
            row.addSubview(percent)
            percentFields.append(percent)

            let remove = NSButton(title: "×", target: self, action: #selector(removePoint(_:)))
            remove.bezelStyle = .smallSquare
            remove.isBordered = false
            remove.attributedTitle = NSAttributedString(
                string: "×",
                attributes: [
                    .font: NSFont.systemFont(ofSize: 16, weight: .bold),
                    .foregroundColor: NSColor.systemRed
                ]
            )
            remove.tag = index
            remove.frame = NSRect(x: 270, y: 0, width: 24, height: 24)
            row.addSubview(remove)
            removeButtons.append(remove)
        }
        pointsContainer.needsLayout = true
        pointsContainer.needsDisplay = true
        view.needsLayout = true
        view.needsDisplay = true
        pointsContainer.displayIfNeeded()
        view.displayIfNeeded()
        view.window?.displayIfNeeded()
    }

    func updatePoints(_ points: [FanCurvePoint]) {
        self.points = points
        rebuildPoints()
    }

    private func editableField(value: String, placeholder: String, tag: Int,
                               identifier: String) -> NSTextField {
        let field = NSTextField(string: value)
        field.alignment = .center
        field.placeholderString = placeholder
        field.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        field.isEditable = true
        field.isBordered = true
        field.bezelStyle = .roundedBezel
        field.tag = tag
        field.identifier = NSUserInterfaceItemIdentifier(identifier)
        field.delegate = self
        return field
    }

    @objc private func sliderChanged(_ sender: NSSlider) {
        let percent = Int(sender.integerValue)
        points[sender.tag].percent = percent
        if sender.tag < percentFields.count {
            percentFields[sender.tag].stringValue = "\(percent)%"
        }
        onPointChanged?(sender.tag, percent)
    }

    func controlTextDidEndEditing(_ notification: Notification) {
        guard let field = notification.object as? NSTextField,
              field.tag >= 0, field.tag < points.count,
              let value = Int(field.stringValue.filter(\.isNumber)) else {
            return
        }

        if field.identifier?.rawValue == "percent", (30...100).contains(value) {
            points[field.tag].percent = value
        } else if field.identifier?.rawValue == "temperature", (0...120).contains(value) {
            points[field.tag].temperature = value
        } else {
            updatePoints(points)
            return
        }
        onPointEdited?(field.tag, points[field.tag].temperature, points[field.tag].percent)
    }

    @objc private func addPoint() {
        onAddPoint?()
    }

    @objc private func removePoint(_ sender: NSButton) {
        onRemovePoint?(sender.tag)
    }
}

final class NVIDIAStatusBarController: NSObject, NSPopoverDelegate {
    private let statusItem: NSStatusItem
    private let statusView = StatusItemView()
    private let popover = NSPopover()
    private let curveController = CurveViewController()
    private var timer: Timer?
    private var points: [FanCurvePoint] = []

    override init() {
        statusItem = NSStatusBar.system.statusItem(withLength: 108)
        super.init()
        points = loadPoints()
        statusItem.button?.addSubview(statusView)
        statusView.frame = statusItem.button?.bounds ?? .zero
        statusView.autoresizingMask = [.width, .height]
        statusItem.button?.action = #selector(togglePopover)
        statusItem.button?.target = self

        curveController.points = points
        curveController.onPointChanged = { [weak self] index, percent in
            self?.points[index].percent = percent
            self?.savePoints()
        }
        curveController.onPointEdited = { [weak self] index, temperature, percent in
            guard let self, index < self.points.count else { return }
            self.points[index] = FanCurvePoint(temperature: temperature, percent: percent)
            self.points.sort { $0.temperature < $1.temperature }
            self.savePoints()
            self.curveController.updatePoints(self.points)
        }
        curveController.onAddPoint = { [weak self] in
            guard let self else { return }
            let lastTemperature = self.points.last?.temperature ?? 40
            let nextTemperature = min(lastTemperature + 10, 120)
            self.points.append(FanCurvePoint(temperature: nextTemperature, percent: 50))
            self.points.sort { $0.temperature < $1.temperature }
            self.savePoints()
            self.curveController.updatePoints(self.points)
        }
        curveController.onRemovePoint = { [weak self] index in
            guard let self, self.points.indices.contains(index) else { return }
            self.points.remove(at: index)
            self.savePoints()
            self.curveController.updatePoints(self.points)
        }
        popover.contentViewController = curveController
        popover.behavior = .transient
        popover.delegate = self

        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        refresh()
    }

    deinit {
        timer?.invalidate()
    }

    @objc private func togglePopover() {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(nil)
        } else {
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        }
    }

    private func refresh() {
        DispatchQueue.global(qos: .utility).async { [weak self] in
            var status = nvfan_status_t()
            let result = nvfan_read_status(&status)
            guard result == 0 else { return }

            let rpms = (0..<NVFAN_MAX_COOLERS).compactMap { index -> Int? in
                let rpm = nvfan_status_fan_rpm(&status, UInt32(index))
                return rpm > 0 ? Int(rpm) : nil
            }
            DispatchQueue.main.async {
                self?.statusView.temperature = Int(status.temperatureC)
                self?.statusView.fanRPMs = rpms
                self?.statusView.needsDisplay = true
            }
        }
    }

    private var configURL: URL {
        URL(fileURLWithPath: "/Library/Application Support/NVIDIAPowerGadget/curve.json")
    }

    private func loadPoints() -> [FanCurvePoint] {
        do {
            let data = try Data(contentsOf: configURL)
            let stored = try JSONDecoder().decode(StoredFanCurve.self, from: data)
            return stored.points.sorted { $0.temperature < $1.temperature }
        } catch CocoaError.fileReadNoSuchFile {
            return []
        } catch {
            NSLog("NVIDIA Power Gadget: could not read fan curve: \(error)")
            return []
        }
    }

    private func savePoints() {
        do {
            let directory = configURL.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(StoredFanCurve(points: points))
            try data.write(to: configURL, options: .atomic)
        } catch {
            NSLog("NVIDIA Power Gadget: could not save fan curve: \(error)")
        }
    }
}
