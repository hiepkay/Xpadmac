//
//  ControllersView.swift
//  XPadMac
//
//  The Controllers section: every controller GameController lists, with a
//  live view of its extended-gamepad input and a rumble test.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import SwiftUI

struct ControllersView: View {
    let monitor: ControllerMonitor

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                SectionHeader(
                    title: "Controllers",
                    subtitle: "The controllers GameController (the macOS game framework) sees. While the driver works, an Xbox One (GIP) or Xbox 360 (XInput) controller shows up here as an Xbox Wireless Controller.")

                if monitor.controllers.isEmpty {
                    ContentUnavailableView {
                        Label("No Controllers", systemImage: "gamecontroller")
                    } description: {
                        Text("Plug in a controller. If it still does not show up, open \"USB Diagnostics\" to see which driver has the device.")
                    }
                    .frame(maxWidth: .infinity, minHeight: 260)
                } else {
                    ForEach(monitor.controllers) { controller in
                        ControllerCard(controller: controller)
                    }
                }
            }
            .padding(24)
        }
        .navigationTitle("Controllers")
    }
}

/// One controller: identity, live input and the rumble test.
private struct ControllerCard: View {
    let controller: ConnectedController

    var body: some View {
        Card(verbatim: controller.name, systemImage: "gamecontroller.fill", tint: .accentColor) {
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 4) {
                GridRow {
                    Text("Category").foregroundStyle(.secondary)
                    Text(controller.category)
                }
                GridRow {
                    Text("Profile").foregroundStyle(.secondary)
                    Text(controller.profile)
                }
            }

            if let pad = controller.pad {
                GamepadView(pad: pad)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            } else {
                Text("This controller has no extended gamepad profile, so there is no live view.")
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 12) {
                Button {
                    controller.rumbleTest()
                } label: {
                    Label("Test Rumble", systemImage: "waveform")
                }
                .disabled(!controller.supportsRumble || controller.rumbleState.isPlaying)
                .help("Rumbles the left motor, right motor, left trigger, right trigger, then all of them, 1 second each")

                switch controller.rumbleState {
                case .idle:
                    if !controller.supportsRumble {
                        Text("GameController reports no rumble support for this controller.")
                            .foregroundStyle(.secondary)
                    }
                case .playing(let step, let index, let count):
                    ProgressView().controlSize(.small)
                    Text("Rumbling: \(step) (\(index)/\(count))").foregroundStyle(.secondary)
                case .failed(let reason):
                    Label(reason, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
            }
        }
    }
}

// MARK: - Gamepad drawing

/// An Xbox-style layout where every control lights up while it is used.
struct GamepadView: View {
    let pad: PadSnapshot

    var body: some View {
        VStack(spacing: 14) {
            HStack(alignment: .bottom) {
                VStack(spacing: 8) {
                    TriggerBar(label: "LT", value: pad.leftTrigger)
                    ButtonCap(label: "LB", pressed: pad.isPressed(.leftShoulder), width: 72)
                }
                Spacer(minLength: 40)
                VStack(spacing: 8) {
                    TriggerBar(label: "RT", value: pad.rightTrigger)
                    ButtonCap(label: "RB", pressed: pad.isPressed(.rightShoulder), width: 72)
                }
            }

            HStack(alignment: .center, spacing: 20) {
                StickView(label: "L", value: pad.leftStick,
                          clicked: pad.isPressed(.leftStick), clickable: pad.has(.leftStick))
                DPadView(pad: pad)
                VStack(spacing: 10) {
                    HStack(spacing: 8) {
                        ButtonCap(label: "View", pressed: pad.isPressed(.view), available: pad.has(.view))
                        ButtonCap(label: "Xbox", pressed: pad.isPressed(.home), available: pad.has(.home))
                        ButtonCap(label: "Menu", pressed: pad.isPressed(.menu), available: pad.has(.menu))
                    }
                    ButtonCap(label: "Share", pressed: pad.isPressed(.share), available: pad.has(.share))
                }
                StickView(label: "R", value: pad.rightStick,
                          clicked: pad.isPressed(.rightStick), clickable: pad.has(.rightStick))
                FaceButtons(pad: pad)
            }
        }
        .padding(18)
        .frame(maxWidth: 720)
        .background(.background, in: RoundedRectangle(cornerRadius: 28, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 28, style: .continuous).strokeBorder(.separator))
    }
}

/// A labelled key that fills with colour while pressed.
private struct ButtonCap: View {
    let label: String
    let pressed: Bool
    var available = true
    var width: CGFloat = 48
    var color: Color = .accentColor

    var body: some View {
        Text(verbatim: label)
            .font(.caption.weight(.semibold))
            .lineLimit(1)
            .frame(width: width, height: 26)
            .foregroundStyle(pressed ? Color.white : Color.primary)
            .background(pressed ? color : Color.secondary.opacity(0.15),
                        in: Capsule())
            .opacity(available ? 1 : 0.35)
            .help(available ? label : String(localized: "\(label): this controller has no such button"))
    }
}

/// A trigger's travel as a filling bar, with the percentage.
private struct TriggerBar: View {
    let label: String
    let value: Float

    var body: some View {
        HStack(spacing: 8) {
            Text(verbatim: label).font(.caption.weight(.semibold)).frame(width: 22, alignment: .leading)
            ZStack(alignment: .leading) {
                Capsule().fill(Color.secondary.opacity(0.15))
                Capsule()
                    .fill(Color.accentColor)
                    .frame(width: 120 * CGFloat(min(max(value, 0), 1)))
            }
            .frame(width: 120, height: 10)
            Text(verbatim: "\(Int((value * 100).rounded()))%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(width: 38, alignment: .trailing)
        }
    }
}

/// A thumbstick: a dot inside its circle of travel, filled while clicked.
private struct StickView: View {
    let label: String
    let value: SIMD2<Float>
    let clicked: Bool
    let clickable: Bool

    private let size: CGFloat = 86
    private let dot: CGFloat = 18

    var body: some View {
        VStack(spacing: 6) {
            ZStack {
                Circle().fill(Color.secondary.opacity(0.12))
                Circle().strokeBorder(clicked ? Color.accentColor : Color.secondary.opacity(0.4),
                                      lineWidth: clicked ? 3 : 1)
                Path { path in
                    path.move(to: CGPoint(x: size / 2, y: 8))
                    path.addLine(to: CGPoint(x: size / 2, y: size - 8))
                    path.move(to: CGPoint(x: 8, y: size / 2))
                    path.addLine(to: CGPoint(x: size - 8, y: size / 2))
                }
                .stroke(Color.secondary.opacity(0.25), lineWidth: 1)
                Circle()
                    .fill(clicked ? Color.accentColor : Color.primary.opacity(0.75))
                    .frame(width: dot, height: dot)
                    // GameController's y is up-positive; the screen's is down.
                    .offset(x: CGFloat(value.x) * (size - dot) / 2,
                            y: -CGFloat(value.y) * (size - dot) / 2)
            }
            .frame(width: size, height: size)
            VStack(spacing: 1) {
                Text(String(format: "%@x %+.2f", label, value.x))
                Text(String(format: "%@y %+.2f", label, value.y))
            }
            .font(.caption2.monospacedDigit())
            .foregroundStyle(.secondary)
            if !clickable {
                Text("no stick click").font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }
}

/// The D-pad as a plus of four keys.
private struct DPadView: View {
    let pad: PadSnapshot

    var body: some View {
        Grid(horizontalSpacing: 2, verticalSpacing: 2) {
            GridRow {
                Color.clear.frame(width: 26, height: 26)
                key(.up, "arrowtriangle.up.fill")
                Color.clear.frame(width: 26, height: 26)
            }
            GridRow {
                key(.left, "arrowtriangle.left.fill")
                Color.secondary.opacity(0.15).frame(width: 26, height: 26)
                key(.right, "arrowtriangle.right.fill")
            }
            GridRow {
                Color.clear.frame(width: 26, height: 26)
                key(.down, "arrowtriangle.down.fill")
                Color.clear.frame(width: 26, height: 26)
            }
        }
    }

    private func key(_ button: PadButton, _ symbol: String) -> some View {
        let pressed = pad.isPressed(button)
        return Image(systemName: symbol)
            .font(.system(size: 11))
            .foregroundStyle(pressed ? Color.white : Color.secondary)
            .frame(width: 26, height: 26)
            .background(pressed ? Color.accentColor : Color.secondary.opacity(0.15),
                        in: RoundedRectangle(cornerRadius: 5, style: .continuous))
    }
}

/// A / B / X / Y in their Xbox colours and diamond layout.
private struct FaceButtons: View {
    let pad: PadSnapshot

    var body: some View {
        Grid(horizontalSpacing: 4, verticalSpacing: 4) {
            GridRow {
                Color.clear.frame(width: 32, height: 32)
                face("Y", .y, .yellow)
                Color.clear.frame(width: 32, height: 32)
            }
            GridRow {
                face("X", .x, .blue)
                Color.clear.frame(width: 32, height: 32)
                face("B", .b, .red)
            }
            GridRow {
                Color.clear.frame(width: 32, height: 32)
                face("A", .a, .green)
                Color.clear.frame(width: 32, height: 32)
            }
        }
    }

    private func face(_ label: String, _ button: PadButton, _ color: Color) -> some View {
        let pressed = pad.isPressed(button)
        return Text(verbatim: label)
            .font(.callout.weight(.bold))
            .frame(width: 32, height: 32)
            .foregroundStyle(pressed ? Color.white : color)
            .background(pressed ? color : color.opacity(0.15), in: Circle())
            .overlay(Circle().strokeBorder(color.opacity(pressed ? 0 : 0.6)))
    }
}
