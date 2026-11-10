//
//  ControllerMonitor.swift
//  XPadMac
//
//  Live view of what the GameController framework sees: the controllers it
//  lists, their extended-gamepad input, and a rumble test through Core Haptics.
//  If XPadMac's driver works, a GIP or XInput pad shows up here as an
//  "Xbox Wireless Controller".
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import Combine
import CoreHaptics
import Foundation
import GameController
import Observation

@MainActor
@Observable
final class ControllerMonitor {
    /// Connected controllers, in the order GameController lists them.
    private(set) var controllers: [ConnectedController] = []

    @ObservationIgnored private var cancellables = Set<AnyCancellable>()

    init() {
        // Keep receiving input while System Settings or a game is frontmost.
        GCController.shouldMonitorBackgroundEvents = true

        let center = NotificationCenter.default
        Publishers.Merge(center.publisher(for: .GCControllerDidConnect),
                         center.publisher(for: .GCControllerDidDisconnect))
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reload() }
            .store(in: &cancellables)

        reload()
    }

    /// Syncs `controllers` with GCController.controllers(), keeping the
    /// models (and their input handlers) of controllers still connected.
    func reload() {
        let current = GCController.controllers()
        for old in controllers where !current.contains(where: { $0 === old.controller }) {
            old.detach()
        }
        controllers = current.map { controller in
            controllers.first { $0.controller === controller } ?? ConnectedController(controller: controller)
        }
    }
}

/// One controller as GameController presents it.
@MainActor
@Observable
final class ConnectedController: Identifiable {
    enum RumbleState: Equatable {
        case idle
        /// The step playing now ("Left motor"...), its number and the count.
        case playing(step: String, index: Int, count: Int)
        case failed(String)

        var isPlaying: Bool {
            if case .playing = self { return true }
            return false
        }
    }

    nonisolated let id: ObjectIdentifier
    let controller: GCController
    let name: String
    let category: String
    let profile: String

    /// The latest extended-gamepad input, or nil for controllers without one.
    private(set) var pad: PadSnapshot?
    private(set) var rumbleState: RumbleState = .idle

    var supportsRumble: Bool { controller.haptics != nil }

    @ObservationIgnored private var hapticEngine: CHHapticEngine?
    @ObservationIgnored private var rumbleTask: Task<Void, Never>?

    /// How long each step of the rumble test buzzes, and the pause between steps.
    private static let rumbleStepDuration: TimeInterval = 1.0
    private static let rumbleStepGap: TimeInterval = 0.25

    /// The rumble test, in order: each motor on its own, then all of them.
    private static let rumbleSteps: [(locality: GCHapticsLocality, name: String)] = [
        (.leftHandle, String(localized: "Left motor")),
        (.rightHandle, String(localized: "Right motor")),
        (.leftTrigger, String(localized: "Left trigger")),
        (.rightTrigger, String(localized: "Right trigger")),
        (.all, String(localized: "All")),
    ]

    init(controller: GCController) {
        id = ObjectIdentifier(controller)
        self.controller = controller
        name = controller.vendorName ?? String(localized: "Unnamed controller")
        category = controller.productCategory
        profile = Self.describeProfile(of: controller)

        // Handlers run on handlerQueue; main is the default, made explicit
        // because the handler below touches main-actor state.
        controller.handlerQueue = .main
        if let gamepad = controller.extendedGamepad {
            // macOS keeps the Home (Xbox) button for a system gesture and
            // hands it only to the frontmost app that takes it over
            // (gamecontrollerd: "active process wants HOME button"), as with
            // a real Xbox controller. .alwaysReceive only "enables the system
            // gesture" and does not count; .disabled does. Take it, so the
            // live view shows it.
            gamepad.buttonHome?.preferredSystemGestureState = .disabled
            pad = PadSnapshot(gamepad)
            gamepad.valueChangedHandler = { [weak self] gamepad, _ in
                self?.pad = PadSnapshot(gamepad)
            }
        }
    }

    /// Stops listening; called when the controller disconnects.
    func detach() {
        controller.extendedGamepad?.valueChangedHandler = nil
        rumbleTask?.cancel()
        stopRumble()
    }

    /// Plays the rumble test: left motor, right motor, left trigger, right
    /// trigger, then all, 1 s each. A locality the controller does not
    /// report is skipped.
    func rumbleTest() {
        guard !rumbleState.isPlaying else { return }
        guard let haptics = controller.haptics else {
            rumbleState = .failed(String(localized: "This controller does not support rumble through GameController."))
            return
        }
        let supported = haptics.supportedLocalities
        let steps = Self.rumbleSteps.filter { $0.locality == .all || supported.contains($0.locality) }
        rumbleTask = Task { [weak self] in
            for (index, step) in steps.enumerated() {
                guard let self, !Task.isCancelled else { return }
                if let reason = self.playRumbleStep(haptics: haptics, locality: step.locality) {
                    self.stopRumble()
                    self.rumbleState = .failed("\(step.name): \(reason)")
                    return
                }
                self.rumbleState = .playing(step: step.name, index: index + 1, count: steps.count)
                try? await Task.sleep(for: .seconds(Self.rumbleStepDuration))
                let engine = self.hapticEngine
                self.hapticEngine = nil
                try? await engine?.stop()
                try? await Task.sleep(for: .seconds(Self.rumbleStepGap))
            }
            self?.stopRumble()
        }
    }

    /// Starts one continuous 1 s haptic on `locality`; nil, or why it failed.
    private func playRumbleStep(haptics: GCDeviceHaptics, locality: GCHapticsLocality) -> String? {
        guard let engine = haptics.createEngine(withLocality: locality) else {
            return String(localized: "could not create a haptic engine.")
        }
        do {
            try engine.start()
            let event = CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: 1.0),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.5),
                ],
                relativeTime: 0,
                duration: Self.rumbleStepDuration)
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            let player = try engine.makePlayer(with: pattern)
            try player.start(atTime: CHHapticTimeImmediate)
        } catch {
            engine.stop(completionHandler: nil)
            return error.localizedDescription
        }
        hapticEngine = engine       // kept alive while it plays
        return nil
    }

    private func stopRumble() {
        hapticEngine?.stop(completionHandler: nil)
        hapticEngine = nil
        rumbleTask = nil
        if rumbleState.isPlaying { rumbleState = .idle }
    }

    private static func describeProfile(of controller: GCController) -> String {
        guard let gamepad = controller.extendedGamepad else {
            return controller.microGamepad != nil ? String(localized: "Micro gamepad") : String(localized: "No gamepad profile")
        }
        switch gamepad {
        case is GCXboxGamepad: return "Xbox (GCXboxGamepad)"
        case is GCDualSenseGamepad: return "DualSense (GCDualSenseGamepad)"
        case is GCDualShockGamepad: return "DualShock 4 (GCDualShockGamepad)"
        default: return String(localized: "Extended gamepad")
        }
    }
}

/// Buttons the live view shows, named after the Xbox layout.
nonisolated enum PadButton: Hashable, Sendable, CaseIterable {
    case a, b, x, y
    case leftShoulder, rightShoulder
    case leftStick, rightStick
    case view, menu, home, share
    case up, down, left, right
}

/// A copy of an extended gamepad's input at one moment.
nonisolated struct PadSnapshot: Equatable, Sendable {
    /// Buttons held down.
    var pressed: Set<PadButton> = []
    /// Buttons this controller has (View, Home, Share and the stick clicks are optional).
    var available: Set<PadButton> = []
    /// Sticks: -1...1, y up-positive (GameController's convention).
    var leftStick: SIMD2<Float> = .zero
    var rightStick: SIMD2<Float> = .zero
    /// Triggers: 0...1.
    var leftTrigger: Float = 0
    var rightTrigger: Float = 0

    init(_ gamepad: GCExtendedGamepad) {
        let buttons: [(PadButton, GCControllerButtonInput?)] = [
            (.a, gamepad.buttonA), (.b, gamepad.buttonB), (.x, gamepad.buttonX), (.y, gamepad.buttonY),
            (.leftShoulder, gamepad.leftShoulder), (.rightShoulder, gamepad.rightShoulder),
            (.leftStick, gamepad.leftThumbstickButton), (.rightStick, gamepad.rightThumbstickButton),
            (.view, gamepad.buttonOptions), (.menu, gamepad.buttonMenu), (.home, gamepad.buttonHome),
            (.share, (gamepad as? GCXboxGamepad)?.buttonShare),
            (.up, gamepad.dpad.up), (.down, gamepad.dpad.down),
            (.left, gamepad.dpad.left), (.right, gamepad.dpad.right),
        ]
        for (button, input) in buttons {
            guard let input else { continue }
            available.insert(button)
            if input.isPressed { pressed.insert(button) }
        }
        leftStick = SIMD2(gamepad.leftThumbstick.xAxis.value, gamepad.leftThumbstick.yAxis.value)
        rightStick = SIMD2(gamepad.rightThumbstick.xAxis.value, gamepad.rightThumbstick.yAxis.value)
        leftTrigger = gamepad.leftTrigger.value
        rightTrigger = gamepad.rightTrigger.value
    }

    func isPressed(_ button: PadButton) -> Bool { pressed.contains(button) }
    func has(_ button: PadButton) -> Bool { available.contains(button) }
}
