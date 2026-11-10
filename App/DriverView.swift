//
//  DriverView.swift
//  XPadMac
//
//  The Driver section: state of the DriverKit extension, its on/off switch,
//  Reinstall / update, the /Applications check and a shortcut to System
//  Settings. DriverToggle is the switch, also shown in the window toolbar.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import SwiftUI

struct DriverView: View {
    let manager: ExtensionManager

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                SectionHeader(
                    title: "Driver",
                    subtitle: "The XPadMac driver makes Xbox One (GIP) and Xbox 360 (XInput) controllers that macOS ignores show up as an Xbox Wireless Controller.")

                statusCard

                if !manager.appLocation.isInApplicationsFolder || manager.appLocation.isTranslocated {
                    locationWarning
                }

                if let message = manager.message {
                    MessageBanner(message: message)
                }

                if manager.needsRestart {
                    MessageBanner(message: StatusMessage(
                        kind: .warning,
                        text: String(localized: "Restart your Mac to finish the driver change.")))
                }

                actions
                steps
                details
            }
            .padding(24)
            .frame(maxWidth: 760, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle("Driver")
    }

    // MARK: Status

    private var statusCard: some View {
        Card(title: "Status") {
            HStack(spacing: 14) {
                if manager.status == .checking {
                    ProgressView().controlSize(.small).frame(width: 28)
                } else {
                    Image(systemName: manager.status.symbol)
                        .font(.system(size: 26))
                        .foregroundStyle(manager.status.tint)
                        .frame(width: 28)
                }
                VStack(alignment: .leading, spacing: 3) {
                    Text(manager.status.title).font(.title3.weight(.medium))
                    if let explanation = manager.status.explanation {
                        Text(explanation).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
                if let operation = manager.operation {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.small)
                        Text(operation == .installing ? "Turning on…" : "Turning off…").foregroundStyle(.secondary)
                    }
                }
                DriverToggle(manager: manager)
                    .controlSize(.large)
            }
        }
    }

    private var locationWarning: some View {
        Card(title: "The app is not in /Applications", systemImage: "exclamationmark.triangle.fill", tint: .orange) {
            if manager.appLocation.isTranslocated {
                Text("macOS is running XPadMac from a temporary copy (App Translocation). Drag XPadMac.app into the Applications folder in Finder, then open it from there.")
            } else {
                Text("macOS installs the driver only when XPadMac.app is in the /Applications folder. Copy the app there, then open it again.")
            }
            Text(manager.appLocation.path)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            HStack {
                Button("Show in Finder") { manager.revealAppInFinder() }
                Button("Open Applications Folder") { manager.openApplicationsFolder() }
            }
        }
    }

    // MARK: Actions

    private var actions: some View {
        HStack(spacing: 10) {
            Button {
                manager.install()
            } label: {
                if isInstalled {
                    Label("Reinstall / Update Driver", systemImage: "arrow.down.circle")
                } else {
                    Label("Install Driver", systemImage: "arrow.down.circle")
                }
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(manager.operation != nil)
            .help("Install the driver in this app on this Mac, or replace the installed one with it")

            Button {
                manager.openSystemSettings()
            } label: {
                Label("Open System Settings", systemImage: "gearshape")
            }
            .controlSize(.large)

            Spacer(minLength: 0)

            Button {
                manager.refreshStatus()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .controlSize(.large)
            .help("Read the driver's state from macOS again")
        }
    }

    private var isInstalled: Bool {
        switch manager.status {
        case .enabled, .disabled, .awaitingApproval: true
        default: false
        }
    }

    // MARK: Guide

    private var steps: some View {
        Card(title: "How to Install", systemImage: "list.number") {
            VStack(alignment: .leading, spacing: 8) {
                step(1, "Copy XPadMac.app to the /Applications folder and open the app from there.")
                step(2, "Turn on the driver switch (or click \"Install Driver\").")
                step(3, "When macOS asks, click \"Open System Settings\", go to General › Login Items & Extensions › Driver Extensions and turn on XPadMac (an administrator password is needed).")
                step(4, "Unplug and replug the controller, then open \"Controllers\": it should be listed as an Xbox Wireless Controller.")
            }
        }
    }

    private func step(_ number: Int, _ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(verbatim: "\(number).").monospacedDigit().foregroundStyle(.secondary)
            Text(text).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var details: some View {
        Card(title: "Technical Details", systemImage: "info.circle") {
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 6) {
                GridRow {
                    Text("Driver identifier").foregroundStyle(.secondary)
                    Text(manager.extensionIdentifier).font(.body.monospaced()).textSelection(.enabled)
                }
                GridRow {
                    Text("Version in this app").foregroundStyle(.secondary)
                    Text(manager.bundledVersion ?? String(localized: "unreadable")).font(.body.monospaced()).textSelection(.enabled)
                }
                GridRow {
                    Text("App location").foregroundStyle(.secondary)
                    Text(manager.appLocation.path).font(.body.monospaced()).textSelection(.enabled)
                }
            }
        }
    }
}

// MARK: - Presentation of DriverStatus

extension DriverStatus {
    var title: String {
        switch self {
        case .checking: String(localized: "Checking…")
        case .notInstalled: String(localized: "Not installed")
        case .awaitingApproval: String(localized: "Waiting for your approval")
        case .enabled: String(localized: "Installed and running")
        case .disabled: String(localized: "Installed but turned off")
        case .uninstalling: String(localized: "Uninstalling")
        case .unknown: String(localized: "Could not read the state")
        }
    }

    var explanation: String? {
        switch self {
        case .checking:
            nil
        case .notInstalled:
            String(localized: "Turn on the switch on the right (or click \"Install Driver\") to get started.")
        case .awaitingApproval:
            String(localized: "Open System Settings › General › Login Items & Extensions › Driver Extensions and turn on XPadMac.")
        case .enabled(let version):
            String(localized: "Version \(version). Plug in a controller and it is ready to use.")
        case .disabled(let version):
            String(localized: "Version \(version). Turn XPadMac back on in System Settings › Login Items & Extensions.")
        case .uninstalling:
            String(localized: "The driver is removed for good after you restart your Mac.")
        case .unknown(let reason):
            reason
        }
    }

    var symbol: String {
        switch self {
        case .checking: "hourglass"
        case .notInstalled: "xmark.circle"
        case .awaitingApproval: "hand.raised.circle.fill"
        case .enabled: "checkmark.circle.fill"
        case .disabled: "pause.circle.fill"
        case .uninstalling: "trash.circle"
        case .unknown: "questionmark.circle"
        }
    }

    var tint: Color {
        switch self {
        case .checking, .uninstalling: .secondary
        case .notInstalled: .secondary
        case .awaitingApproval, .disabled: .orange
        case .enabled: .green
        case .unknown: .red
        }
    }
}

// MARK: - The on/off switch

/// The driver's on/off switch. On activates the driver; off deactivates it
/// (macOS removes it, at once or at the next restart), after a confirmation.
struct DriverToggle: View {
    let manager: ExtensionManager
    /// Shows "Driver" next to the switch (the toolbar) or nothing (the card).
    var showsLabel = false

    @State private var confirmingOff = false

    var body: some View {
        Toggle(isOn: Binding(
            get: { manager.isOn },
            set: { on in
                if on {
                    manager.turnOn()
                } else {
                    confirmingOff = true
                }
            })) {
            Text("Driver")
        }
        .toggleStyle(.switch)
        .labelsHidden(!showsLabel)
        .disabled(!manager.canToggle)
        .help(manager.isOn ? Text("The XPadMac driver is on. Switch to turn it off.")
                           : Text("The XPadMac driver is off. Switch to turn it on."))
        .confirmationDialog("Turn off the XPadMac driver?", isPresented: $confirmingOff) {
            Button("Turn Off Driver", role: .destructive) { manager.turnOff() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("To turn it off at once without restarting, unplug the dock or controller first: macOS can remove the driver only while no device is using it.\n\nThe controller goes back to how macOS handles it on its own and no longer shows up as an Xbox Wireless Controller. Turn it back on at any time (macOS may ask for approval again).")
        }
    }
}

private extension View {
    /// labelsHidden() only when asked.
    @ViewBuilder
    func labelsHidden(_ hidden: Bool) -> some View {
        if hidden { labelsHidden() } else { self }
    }
}
