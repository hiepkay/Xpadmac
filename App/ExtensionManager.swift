//
//  ExtensionManager.swift
//  XPadMac
//
//  Installs, removes and reports the state of the XPadDriver DriverKit
//  extension that ships inside the app at Contents/Library/SystemExtensions.
//  All work goes through OSSystemExtensionManager; requests are created with
//  the main queue so every delegate callback arrives on the main thread.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import AppKit
import Combine
import Foundation
import Observation
import SystemExtensions

/// What macOS reports about our driver extension.
nonisolated enum DriverStatus: Equatable, Sendable {
    /// A properties request is on its way.
    case checking
    /// macOS knows no extension with our identifier.
    case notInstalled
    /// Activated, but the user still has to allow it in System Settings.
    case awaitingApproval
    /// Activated and enabled: the driver matches controllers.
    case enabled(version: String)
    /// Installed, but turned off in System Settings.
    case disabled(version: String)
    /// Deactivated; it goes away for good at the next restart.
    case uninstalling
    /// The state could not be read (for example an unsigned build).
    case unknown(reason: String)
}

/// A one-line note for the user about the last thing that happened.
nonisolated struct StatusMessage: Equatable, Sendable {
    enum Kind: Sendable { case info, success, warning, error }

    let kind: Kind
    let text: String
    /// Optional technical detail (the system's own error text, an error code).
    var detail: String?
}

/// Where the running app bundle lives. macOS activates system extensions
/// only for apps inside /Applications.
nonisolated struct AppLocation: Sendable {
    let bundleURL: URL

    var path: String { bundleURL.path(percentEncoded: false) }

    /// True when the app sits in /Applications (subfolders are accepted too).
    var isInApplicationsFolder: Bool { path.hasPrefix("/Applications/") }

    /// True when Gatekeeper runs the app from a randomised read-only copy
    /// (App Translocation), which also blocks activation.
    var isTranslocated: Bool { path.contains("/AppTranslocation/") }
}

@MainActor
@Observable
final class ExtensionManager {
    /// The operation the user started and that has not finished yet.
    enum Operation: Sendable { case installing, uninstalling }

    /// The dext's bundle identifier: always the app's identifier + ".driver"
    /// (project.yml builds it as $(BUNDLE_ID_PREFIX)xpadmac.driver).
    let extensionIdentifier: String
    let appLocation: AppLocation
    /// The version of the dext inside this app, "short (build)" as macOS
    /// reports installed ones; nil if its Info.plist cannot be read.
    let bundledVersion: String?

    private(set) var status: DriverStatus = .checking
    private(set) var operation: Operation?
    private(set) var message: StatusMessage?
    /// Set once macOS says a request completes only after a restart.
    private(set) var needsRestart = false

    /// Delegates of requests still in flight. OSSystemExtensionRequest only
    /// holds its delegate weakly, so we keep them alive until they finish.
    @ObservationIgnored private var inFlight: [UUID: RequestDelegate] = [:]
    /// The activation / deactivation request behind `operation`. Older
    /// requests (e.g. one still waiting for approval) may report later.
    @ObservationIgnored private var operationToken: UUID?
    /// When the properties request in flight was sent (nil: none in flight).
    @ObservationIgnored private var propertiesRequestSentAt: Date?
    @ObservationIgnored private var propertiesRefreshQueued = false
    /// Versions macOS last reported as installed (not uninstalling).
    @ObservationIgnored private var installedVersions: Set<String> = []
    /// The activation request sent while a driver of the bundled version was
    /// already installed: macOS finishes it without replacing anything.
    @ObservationIgnored private var sameVersionActivation: UUID?
    @ObservationIgnored private var cancellables = Set<AnyCancellable>()

    /// Used when the app runs without a bundle identifier (a bare binary).
    private static let fallbackAppIdentifier = "com.example.xpadmac"
    /// A properties request that has not answered by then is presumed lost.
    private static let propertiesTimeout: TimeInterval = 10

    init(bundle: Bundle = .main) {
        let appIdentifier = bundle.bundleIdentifier ?? Self.fallbackAppIdentifier
        extensionIdentifier = appIdentifier + ".driver"
        appLocation = AppLocation(bundleURL: bundle.bundleURL.resolvingSymlinksInPath())
        bundledVersion = Self.bundledDriverVersion(in: bundle, identifier: extensionIdentifier)

        // The user approves or toggles the driver in System Settings; re-read
        // the state whenever they come back to XPadMac.
        NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)
            .sink { [weak self] _ in self?.refreshStatus() }
            .store(in: &cancellables)

        refreshStatus()
    }

    // MARK: - User actions

    /// The on/off switch's position: on while the driver is activated and
    /// enabled, or activated and waiting for the user's approval.
    var isOn: Bool {
        switch status {
        case .enabled, .awaitingApproval: true
        case .checking, .notInstalled, .disabled, .uninstalling, .unknown: false
        }
    }

    /// The switch can move unless a request is running or the state is
    /// still being read.
    var canToggle: Bool { operation == nil && status != .checking }

    /// Turns the driver on: activates it. A driver that is installed but
    /// switched off in System Settings can only be switched on there (no
    /// API does it), so that pane is opened instead.
    func turnOn() {
        if case .disabled = status {
            message = StatusMessage(
                kind: .warning,
                text: String(localized: "The driver is installed but turned off in System Settings, and macOS lets you turn it back on only there: General › Login Items & Extensions › Driver Extensions › XPadMac."))
            openSystemSettings()
            return
        }
        install()
    }

    /// Turns the driver off: deactivates it. macOS removes it (at once, or
    /// at the next restart if it cannot stop it now), and the controller is
    /// left to macOS's own drivers until it is turned on again.
    func turnOff() {
        uninstall()
    }

    /// Asks macOS to activate (or replace) the embedded driver.
    func install() {
        guard operation == nil else { return }
        operation = .installing
        message = StatusMessage(kind: .info, text: String(localized: "Sending the request to turn on (install) the driver…"))
        let sameVersion = bundledVersion.map(installedVersions.contains) ?? false
        let token = submit(.activate)
        operationToken = token
        sameVersionActivation = sameVersion ? token : nil
    }

    /// Asks macOS to deactivate the driver.
    func uninstall() {
        guard operation == nil else { return }
        operation = .uninstalling
        message = StatusMessage(kind: .info, text: String(localized: "Sending the request to turn off (remove) the driver…"))
        operationToken = submit(.deactivate)
    }

    /// Re-reads the driver's state (properties request, macOS 12+).
    func refreshStatus() {
        if let sentAt = propertiesRequestSentAt, Date().timeIntervalSince(sentAt) < Self.propertiesTimeout {
            // A result already on its way may predate the change we care
            // about: ask again once it lands.
            propertiesRefreshQueued = true
            return
        }
        propertiesRequestSentAt = Date()
        if case .unknown = status { status = .checking }
        submit(.properties)
    }

    /// Opens System Settings › General › Login Items & Extensions.
    func openSystemSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") else { return }
        NSWorkspace.shared.open(url)
    }

    /// Shows the running app in Finder, so the user can drag it to /Applications.
    func revealAppInFinder() {
        NSWorkspace.shared.activateFileViewerSelecting([appLocation.bundleURL])
    }

    func openApplicationsFolder() {
        NSWorkspace.shared.open(URL(filePath: "/Applications", directoryHint: .isDirectory))
    }

    // MARK: - Requests

    /// Submits a request and returns the token its callbacks report under.
    @discardableResult
    private func submit(_ kind: RequestKind) -> UUID {
        let request: OSSystemExtensionRequest
        switch kind {
        case .activate:
            request = .activationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        case .deactivate:
            request = .deactivationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        case .properties:
            request = .propertiesRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        }
        let token = UUID()
        let delegate = RequestDelegate { [weak self] event in
            self?.handle(event, kind: kind, token: token)
        }
        inFlight[token] = delegate
        request.delegate = delegate
        OSSystemExtensionManager.shared.submitRequest(request)
        return token
    }

    private func handle(_ event: RequestEvent, kind: RequestKind, token: UUID) {
        if kind == .properties {
            handlePropertiesEvent(event, token: token)
            return
        }
        let isCurrent = token == operationToken

        switch event {
        case .needsUserApproval:
            // The request stays pending (keep its delegate) until the user
            // allows the driver in System Settings or the app quits; the UI
            // is free again meanwhile.
            if isCurrent { operation = nil }
            status = .awaitingApproval
            message = StatusMessage(
                kind: .warning,
                text: String(localized: "macOS needs your approval for the driver. Click \"Open System Settings\", go to Login Items & Extensions › Driver Extensions and turn on XPadMac."))

        case .finished(let restartRequired):
            inFlight[token] = nil
            if isCurrent { endOperation() }
            if restartRequired { needsRestart = true }
            if kind == .activate {
                let sameVersion = token == sameVersionActivation
                if sameVersion { sameVersionActivation = nil }
                if restartRequired {
                    message = StatusMessage(kind: .warning, text: String(localized: "The driver starts working after you restart your Mac."))
                } else if sameVersion, let version = bundledVersion {
                    // sysextd: "activateDecision found existing entry of same
                    // version". It reports success and leaves the running
                    // driver in place, so a rebuilt driver with an unchanged
                    // version would never load.
                    message = StatusMessage(
                        kind: .warning,
                        text: String(localized: "Driver version \(version) is already installed, so macOS keeps the running copy and does not replace it. If you just changed the driver, rebuild it with make build (every build gets a new version number), run make install, then reinstall."))
                } else {
                    message = StatusMessage(kind: .success, text: String(localized: "The driver is on. Unplug and replug the controller (or the dock)."))
                }
            } else {
                message = restartRequired
                    ? StatusMessage(kind: .warning, text: String(localized: "The driver turns off for good after you restart your Mac, because it is still driving a device. Next time, unplug the dock or controller before turning it off and no restart is needed."))
                    : StatusMessage(kind: .success, text: String(localized: "The driver is off. Unplug and replug the controller so macOS takes it back the default way."))
            }
            refreshStatus()

        case .failed(let failure):
            inFlight[token] = nil
            if isCurrent { endOperation() }
            if token == sameVersionActivation { sameVersionActivation = nil }
            // A request replaced by a newer one (Install clicked again while
            // waiting for approval, say) is not news: the newer one reports.
            if failure.isSuperseded { return }
            message = StatusMessage(kind: .error, text: Self.explain(failure), detail: failure.detail)
            refreshStatus()

        case .foundProperties:
            break   // only sent for properties requests
        }
    }

    private func endOperation() {
        operation = nil
        operationToken = nil
    }

    private func handlePropertiesEvent(_ event: RequestEvent, token: UUID) {
        switch event {
        case .foundProperties(let extensions):
            status = Self.status(from: extensions)
            installedVersions = Set(extensions.filter { !$0.isUninstalling }.map(\.version))
        case .failed(let failure):
            status = .unknown(reason: Self.explain(failure))
            installedVersions = []
            propertiesRefreshQueued = false
        case .needsUserApproval, .finished:
            // Not the answer we wait for; foundProperties or a failure follows.
            return
        }
        inFlight[token] = nil
        propertiesRequestSentAt = nil
        if propertiesRefreshQueued {
            propertiesRefreshQueued = false
            refreshStatus()
        }
    }

    /// "short (build)" of the dext in Contents/Library/SystemExtensions.
    private static func bundledDriverVersion(in bundle: Bundle, identifier: String) -> String? {
        let plistURL = bundle.bundleURL
            .appending(path: "Contents/Library/SystemExtensions/\(identifier).dext/Info.plist", directoryHint: .notDirectory)
        guard let data = try? Data(contentsOf: plistURL),
              let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let short = info["CFBundleShortVersionString"] as? String,
              let build = info["CFBundleVersion"] as? String else { return nil }
        return ExtensionSnapshot.versionString(short: short, build: build)
    }

    /// Reduces every version macOS knows of (an old one may still be
    /// uninstalling next to a new one) to one state.
    private static func status(from extensions: [ExtensionSnapshot]) -> DriverStatus {
        let live = extensions.filter { !$0.isUninstalling }
        if let enabled = live.first(where: \.isEnabled) {
            return .enabled(version: enabled.version)
        }
        if live.contains(where: \.isAwaitingUserApproval) {
            return .awaitingApproval
        }
        if let installed = live.first {
            return .disabled(version: installed.version)
        }
        return extensions.isEmpty ? .notInstalled : .uninstalling
    }

    /// A localized explanation for an OSSystemExtensionErrorDomain error.
    private static func explain(_ failure: RequestFailure) -> String {
        guard failure.domain == OSSystemExtensionErrorDomain,
              let code = OSSystemExtensionError.Code(rawValue: failure.code) else {
            return String(localized: "Error: \(failure.description)")
        }
        switch code {
        case .unknown:
            return String(localized: "macOS reported an unknown error while handling the driver.")
        case .missingEntitlement:
            return String(localized: "The app lacks the entitlement to install a system extension. This build is not signed with an Apple Developer account.")
        case .unsupportedParentBundleLocation:
            return String(localized: "XPadMac must be in the /Applications folder before macOS installs the driver.")
        case .extensionNotFound:
            return String(localized: "The driver was not found in the app bundle (Contents/Library/SystemExtensions).")
        case .extensionMissingIdentifier:
            return String(localized: "The driver in the app bundle has no bundle identifier.")
        case .duplicateExtensionIdentifer:
            return String(localized: "The app bundle contains several drivers with the same bundle identifier.")
        case .unknownExtensionCategory:
            return String(localized: "macOS does not recognize this kind of system extension.")
        case .codeSignatureInvalid:
            return String(localized: "The driver's code signature is invalid. It must be signed with an Apple Development certificate and a profile that includes this Mac's UDID.")
        case .validationFailed:
            return String(localized: "macOS rejected the driver during validation (signature, entitlements or Info.plist).")
        case .forbiddenBySystemPolicy:
            return String(localized: "System policy (MDM or security settings) does not allow this driver.")
        case .requestCanceled:
            return String(localized: "The request was canceled.")
        case .requestSuperseded:
            return String(localized: "The request was replaced by a newer one.")
        case .authorizationRequired:
            return String(localized: "Administrator rights are needed for this action.")
        @unknown default:
            return String(localized: "System extension error (code \(failure.code)): \(failure.description)")
        }
    }
}

// MARK: - Delegate bridge

/// Which kind of request a delegate serves.
nonisolated enum RequestKind: Sendable { case activate, deactivate, properties }

/// A request outcome, reduced to Sendable values so it can cross to the main actor.
nonisolated enum RequestEvent: Sendable {
    case needsUserApproval
    case finished(restartRequired: Bool)
    case failed(RequestFailure)
    case foundProperties([ExtensionSnapshot])
}

nonisolated struct RequestFailure: Sendable {
    let domain: String
    let code: Int
    let description: String

    init(_ error: any Error) {
        let error = error as NSError
        domain = error.domain
        code = error.code
        description = error.localizedDescription
    }

    var isSuperseded: Bool {
        domain == OSSystemExtensionErrorDomain && code == OSSystemExtensionError.Code.requestSuperseded.rawValue
    }

    /// The system's text plus the code, for the "details" line.
    var detail: String { "\(description) [\(domain) \(code)]" }
}

/// What a properties request says about one installed version.
nonisolated struct ExtensionSnapshot: Sendable {
    let version: String
    let isEnabled: Bool
    let isAwaitingUserApproval: Bool
    let isUninstalling: Bool

    /// How a version is shown, and compared with the bundled one.
    static func versionString(short: String, build: String) -> String {
        "\(short) (\(build))"
    }

    init(_ properties: OSSystemExtensionProperties) {
        version = Self.versionString(short: properties.bundleShortVersion, build: properties.bundleVersion)
        isEnabled = properties.isEnabled
        isAwaitingUserApproval = properties.isAwaitingUserApproval
        isUninstalling = properties.isUninstalling
    }
}

/// Receives one request's callbacks and forwards them to the main actor.
nonisolated final class RequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    private let onEvent: @MainActor (RequestEvent) -> Void

    init(onEvent: @escaping @MainActor (RequestEvent) -> Void) {
        self.onEvent = onEvent
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        // Asked only when the installed driver has another version
        // (CFBundleVersion or CFBundleShortVersionString): then the copy in
        // this app always replaces it, upgrade or downgrade. One of the
        // same version is kept as it is and the request just finishes;
        // `make build` gives every build its own CFBundleVersion, and
        // ExtensionManager warns when the versions match.
        .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        deliver(.needsUserApproval)
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        deliver(.finished(restartRequired: result == .willCompleteAfterReboot))
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: any Error) {
        deliver(.failed(RequestFailure(error)))
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        deliver(.foundProperties(properties.map(ExtensionSnapshot.init)))
    }

    private func deliver(_ event: RequestEvent) {
        let onEvent = onEvent
        if Thread.isMainThread {
            // Requests are created with queue: .main, so this is the normal path.
            MainActor.assumeIsolated { onEvent(event) }
        } else {
            Task { @MainActor in onEvent(event) }
        }
    }
}
