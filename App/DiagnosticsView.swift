//
//  DiagnosticsView.swift
//  XPadMac
//
//  The Diagnostics section: Xbox-protocol (GIP / XInput) USB devices from
//  the IORegistry, the driver classes attached to each, and
//  the HID identity they end up publishing.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import SwiftUI

struct DiagnosticsView: View {
    let model: USBDiagnosticsModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack(alignment: .top) {
                    SectionHeader(
                        title: "USB Diagnostics",
                        subtitle: "Xbox controllers on USB (GIP and XInput, from Microsoft and the vendors XPadMac supports), and the drivers attached to each device.")
                    Button {
                        model.refresh()
                    } label: {
                        Label("Refresh", systemImage: "arrow.clockwise")
                    }
                    .controlSize(.large)
                    .disabled(model.isScanning)
                }

                scanInfo

                if model.devices.isEmpty {
                    ContentUnavailableView {
                        Label("No Matching Devices", systemImage: "cable.connector")
                    } description: {
                        Text("No Xbox controller found on USB. Connect a controller by cable or through a dock / receiver, then click \"Refresh\".")
                    }
                    .frame(maxWidth: .infinity, minHeight: 240)
                } else {
                    ForEach(model.devices) { device in
                        USBDeviceCard(device: device)
                    }
                }
            }
            .padding(24)
        }
        .navigationTitle("USB Diagnostics")
    }

    private var scanInfo: some View {
        HStack(spacing: 6) {
            if model.isScanning {
                ProgressView().controlSize(.small)
                Text("Scanning the IORegistry…")
            } else if let lastScan = model.lastScan {
                Text("Updated at \(lastScan.formatted(date: .omitted, time: .standard)).")
            }
            if model.watchesHotplug {
                Text("Refreshes on its own when a USB device is plugged in or removed.")
            }
        }
        .font(.callout)
        .foregroundStyle(.secondary)
    }
}

/// One USB device: identity, configuration, drivers, and the registry subtree.
private struct USBDeviceCard: View {
    let device: USBDeviceReport

    var body: some View {
        Card(verbatim: device.productName, systemImage: "cable.connector", tint: verdictTint) {
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 6) {
                row("VID:PID", device.vidPid, monospaced: true)
                if let vendor = device.vendorName {
                    row("Manufacturer", vendor)
                }
                if let location = device.locationID {
                    row("Location (locationID)", String(format: "0x%08X", location), monospaced: true)
                }
                row("USB configuration", device.configuration.map { "bConfigurationValue \($0)" } ?? String(localized: "Not configured"))
                row("Drivers", driverSummary)
                if let hid = device.hidIdentity {
                    row("HID device", hidSummary(hid), monospaced: false)
                }
                if let source = device.xpadSource {
                    row("XPadSource", source, monospaced: true)
                }
                if !device.clientProcesses.isEmpty {
                    row("Opened by", device.clientProcesses.joined(separator: ", "))
                }
            }

            if let hint {
                Label(hint, systemImage: verdictSymbol)
                    .foregroundStyle(verdictTint)
                    .fixedSize(horizontal: false, vertical: true)
            }

            DisclosureGroup("IORegistry tree (\(device.nodes.count) entries)") {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(device.nodes) { node in
                        RegistryNodeRow(node: node)
                    }
                }
                .padding(.top, 6)
                .textSelection(.enabled)
            }
        }
    }

    private func row(_ title: LocalizedStringKey, _ value: String, monospaced: Bool = false) -> some View {
        GridRow {
            Text(title).foregroundStyle(.secondary)
            Text(value)
                .font(monospaced ? .body.monospaced() : .body)
                .textSelection(.enabled)
        }
    }

    /// Driver classes in attach order, repeated ones collapsed ("… ×4").
    private var driverSummary: String {
        var order: [RegistryNode] = []
        var counts: [String: Int] = [:]
        for node in device.drivers {
            if counts[node.displayClass] == nil { order.append(node) }
            counts[node.displayClass, default: 0] += 1
        }
        guard !order.isEmpty else { return String(localized: "None") }
        return order.map { node in
            let count = counts[node.displayClass] ?? 1
            let name = count > 1 ? "\(node.displayClass) ×\(count)" : node.displayClass
            switch node.origin {
            case .xpad: return "\(name) (XPadMac)"
            case .apple: return "\(name) (Apple)"
            case .thirdParty: return name
            }
        }
        .joined(separator: ", ")
    }

    private func hidSummary(_ hid: HIDIdentity) -> String {
        [hid.vidPid, hid.transport, hid.product].compactMap { $0 }.joined(separator: " · ")
    }

    private var verdictSymbol: String {
        switch device.verdict {
        case .xpad: "checkmark.circle.fill"
        case .apple: "info.circle.fill"
        case .thirdParty: "puzzlepiece.extension"
        case .none: "exclamationmark.triangle.fill"
        }
    }

    private var verdictTint: Color {
        switch device.verdict {
        case .xpad: .green
        case .apple: .blue
        case .thirdParty: .purple
        case .none: .orange
        }
    }

    /// What this device's state means for the user, when we can tell.
    private var hint: String? {
        let microsoft = device.vendorID == 0x045E
        let xinput = device.nodes.contains {
            guard let i = $0.interface else { return false }
            return i.interfaceClass == 0xFF && i.subclass == 0x5D && i.interfaceProtocol == 0x01
        }
        let xpad = device.verdict == .xpad

        if xpad {
            return String(localized: "The XPadMac driver is driving this device and presents it to macOS as an Xbox Wireless Controller.")
        }
        if xinput && !microsoft {
            return String(localized: "Xbox 360 (XInput): macOS has no driver for the FF/5D/01 interface. Install the XPadMac driver (XPad360Gamepad), then unplug and replug the controller so it shows up as an Xbox Wireless Controller.")
        }
        if !microsoft && device.verdict == .none {
            return device.configuration == nil
                ? String(localized: "GIP (Xbox) mode: macOS has no driver for this controller, so it is not configured yet. The XPadMac driver will take it.")
                : String(localized: "GIP (Xbox) mode: the device is configured but no driver has taken it.")
        }
        if microsoft && Self.appleXboxPIDs.contains(device.productID) {
            return String(localized: "macOS supports this Xbox controller on its own; XPadMac leaves it alone.")
        }
        if microsoft && Self.xpadXboxPIDs.contains(device.productID) {
            return String(localized: "An early Xbox One controller that macOS does not support on its own; the XPadMac driver will take it.")
        }
        if device.verdict == .none {
            return String(localized: "No driver has taken this device.")
        }
        return nil
    }

    /// Microsoft PIDs Apple's XboxGamepad.dext drives.
    private static let appleXboxPIDs: Set<Int> = [0x028E, 0x02EA, 0x0B00, 0x0B12]
    /// Microsoft PIDs XPadMac's GIP personality matches (Xbox One 2013/2015, Elite 1).
    private static let xpadXboxPIDs: Set<Int> = [0x02D1, 0x02DD, 0x02E3]
}

/// One line of the registry subtree, indented by depth.
private struct RegistryNodeRow: View {
    let node: RegistryNode

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(node.isUSBClient ? "\(node.className) — \(node.name)" : node.className)
                .font(.caption.monospaced())
            if let userClass = node.userClass {
                Text(userClass)
                    .font(.caption.monospaced().weight(.semibold))
                    .foregroundStyle(node.origin == .xpad ? Color.green : Color.accentColor)
            }
            if let interface = node.interface {
                Text(interface.label + (interface.isGIP ? " GIP" : interface.isXInput ? " XInput" : ""))
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            if let hid = node.hid {
                Text(hid.vidPid).font(.caption.monospaced()).foregroundStyle(.secondary)
            }
            if node.isDriver {
                Text("driver")
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 5)
                    .background(Color.secondary.opacity(0.15), in: Capsule())
            }
        }
        .padding(.leading, CGFloat(node.depth - 1) * 14)
    }
}
