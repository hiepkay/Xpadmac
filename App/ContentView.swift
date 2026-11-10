//
//  ContentView.swift
//  XPadMac
//
//  Main window: a sidebar with the sections (Driver, Controllers, Polling
//  rate, Diagnostics) and the selected section on the right.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import SwiftUI

/// The sections of the main window.
enum AppSection: String, CaseIterable, Identifiable, Hashable {
    case driver, controllers, pollingRate, diagnostics

    var id: Self { self }

    var title: String {
        switch self {
        case .driver: String(localized: "Driver")
        case .controllers: String(localized: "Controllers")
        case .pollingRate: String(localized: "Polling Rate")
        case .diagnostics: String(localized: "USB Diagnostics")
        }
    }

    var systemImage: String {
        switch self {
        case .driver: "puzzlepiece.extension"
        case .controllers: "gamecontroller"
        case .pollingRate: "speedometer"
        case .diagnostics: "stethoscope"
        }
    }
}

struct ContentView: View {
    let extensionManager: ExtensionManager
    let controllerMonitor: ControllerMonitor
    let pollingRateMeter: PollingRateMeter
    let diagnostics: USBDiagnosticsModel

    @State private var selection: AppSection? = .driver

    var body: some View {
        NavigationSplitView {
            List(AppSection.allCases, selection: $selection) { section in
                NavigationLink(value: section) {
                    Label(section.title, systemImage: section.systemImage)
                }
                .badge(badge(for: section))
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 210)
        } detail: {
            switch selection ?? .driver {
            case .driver:
                DriverView(manager: extensionManager)
            case .controllers:
                ControllersView(monitor: controllerMonitor)
            case .pollingRate:
                PollingRateView(meter: pollingRateMeter)
            case .diagnostics:
                DiagnosticsView(model: diagnostics)
            }
        }
        .frame(minWidth: 920, minHeight: 560)
        .toolbar {
            // The driver's on/off switch, reachable from every section.
            ToolbarItem(placement: .primaryAction) {
                DriverToggle(manager: extensionManager, showsLabel: true)
            }
        }
    }

    /// Small counters in the sidebar (0 hides the badge).
    private func badge(for section: AppSection) -> Int {
        switch section {
        case .driver: 0
        case .controllers: controllerMonitor.controllers.count
        case .pollingRate: 0
        case .diagnostics: diagnostics.devices.count
        }
    }
}

// MARK: - Shared building blocks

/// A titled, rounded panel used by every section.
struct Card<Content: View>: View {
    let title: Text
    var systemImage: String?
    /// Colour of the title icon (secondary when nil).
    var tint: Color?
    @ViewBuilder var content: Content

    /// A fixed title, looked up in Localizable.xcstrings.
    init(title: LocalizedStringKey, systemImage: String? = nil, tint: Color? = nil,
         @ViewBuilder content: () -> Content) {
        self.init(title: Text(title), systemImage: systemImage, tint: tint, content: content)
    }

    /// A title shown as it is (a device name, or a string already localized).
    init(verbatim title: String, systemImage: String? = nil, tint: Color? = nil,
         @ViewBuilder content: () -> Content) {
        self.init(title: Text(verbatim: title), systemImage: systemImage, tint: tint, content: content)
    }

    private init(title: Text, systemImage: String?, tint: Color?, @ViewBuilder content: () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.tint = tint
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let systemImage {
                Label {
                    title
                } icon: {
                    Image(systemName: systemImage).foregroundStyle(tint ?? Color.secondary)
                }
                .font(.headline)
            } else {
                title.font(.headline)
            }
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(.separator))
    }
}

/// A section header: big title and an explanatory line.
struct SectionHeader: View {
    let title: LocalizedStringKey
    let subtitle: LocalizedStringKey

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.largeTitle.weight(.semibold))
            Text(subtitle).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A coloured banner for a StatusMessage.
struct MessageBanner: View {
    let message: StatusMessage

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: symbol).foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 4) {
                Text(message.text)
                if let detail = message.detail {
                    Text(detail).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(12)
        .background(tint.opacity(0.12), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private var symbol: String {
        switch message.kind {
        case .info: "info.circle.fill"
        case .success: "checkmark.circle.fill"
        case .warning: "exclamationmark.triangle.fill"
        case .error: "xmark.octagon.fill"
        }
    }

    private var tint: Color {
        switch message.kind {
        case .info: .blue
        case .success: .green
        case .warning: .orange
        case .error: .red
        }
    }
}
