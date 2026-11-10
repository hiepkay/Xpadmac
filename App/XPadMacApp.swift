//
//  XPadMacApp.swift
//  XPadMac
//
//  Host app for the XPadDriver DriverKit extension: installs / removes the
//  driver, shows what GameController sees, and lists the matching USB devices.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import SwiftUI

@main
struct XPadMacApp: App {
    // App-lifetime models, shared by every section.
    @State private var extensionManager = ExtensionManager()
    @State private var controllerMonitor = ControllerMonitor()
    @State private var pollingRateMeter = PollingRateMeter()
    @State private var diagnostics = USBDiagnosticsModel()

    var body: some Scene {
        Window("XPadMac", id: "main") {
            ContentView(extensionManager: extensionManager,
                        controllerMonitor: controllerMonitor,
                        pollingRateMeter: pollingRateMeter,
                        diagnostics: diagnostics)
        }
        .defaultSize(width: 1020, height: 700)
    }
}
