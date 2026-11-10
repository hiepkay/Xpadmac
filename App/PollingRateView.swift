//
//  PollingRateView.swift
//  XPadMac
//
//  The Polling rate section: pick a HID gamepad, start a measurement and
//  watch how many input reports per second reach macOS, with the interval
//  statistics and a rolling chart.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import Charts
import SwiftUI

struct PollingRateView: View {
    let meter: PollingRateMeter

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                SectionHeader(
                    title: "Polling Rate",
                    subtitle: "Input reports per second that a HID device sends to macOS. For a device tagged XPadMac this is the rate after the driver: exactly what games and GameController receive. An untagged device shows the raw rate, before the driver (or a controller that does not go through XPadMac).")

                if let message = meter.message {
                    MessageBanner(message: message)
                }

                HowToMeasureCard()

                DeviceListCard(meter: meter)

                if let device = meter.selectedDevice {
                    MeasurementCard(meter: meter, device: device)
                }
            }
            .padding(24)
        }
        .navigationTitle("Polling Rate")
        // Listing and measuring only while the section is on screen; leaving
        // it stops the measurement and closes the device.
        .onAppear { meter.activate() }
        .onDisappear { meter.deactivate() }
    }
}

// MARK: - How to measure

private struct HowToMeasureCard: View {
    var body: some View {
        Card(title: "How to Measure", systemImage: "info.circle", tint: .blue) {
            VStack(alignment: .leading, spacing: 6) {
                bullet("Click \"Start\", then **keep moving both analog sticks** for a few seconds. A controller left alone may send nothing, so the number will be low or zero.")
                bullet("A GIP controller (through a dock / 2.4 GHz receiver, or by cable in GIP mode) sends reports only when its state changes; a silence longer than \(Int(PollingAccumulator.pauseThreshold)) ms counts as a \"pause\" and is left out of the intervals between reports.")
                bullet("Xbox 360 (XInput) controller: XPadMac declares the ReportInterval of the controller's endpoint (the \"Declared\" tile). Measure to find the real rate.")
                bullet("A device tagged **XPadMac** is the Xbox controller the driver creates: the measured number is the rate games get after the driver. An untagged device (a controller XPadMac does not drive) gives its own raw rate.")
                bullet("The measured number is the rate reaching macOS, not the highest rate the controller supports. The device is opened shared (not seized), so games keep using the controller normally while you measure.")
            }
            .font(.callout)
        }
    }

    private func bullet(_ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(verbatim: "•").foregroundStyle(.secondary)
            Text(text).fixedSize(horizontal: false, vertical: true)
        }
    }
}

// MARK: - Device list

private struct DeviceListCard: View {
    let meter: PollingRateMeter

    var body: some View {
        Card(title: "Devices", systemImage: "gamecontroller", tint: .accentColor) {
            if meter.devices.isEmpty {
                Group {
                    if meter.isListing {
                        Text("No HID gamepad or joystick found. Plug in or connect a controller; the list updates on its own.")
                    } else {
                        Text("Preparing the list of HID devices…")
                    }
                }
                .foregroundStyle(.secondary)
            } else {
                VStack(spacing: 6) {
                    ForEach(meter.devices) { device in
                        DeviceRow(device: device, isSelected: device.id == meter.selectedID) {
                            meter.selectedID = device.id
                        }
                    }
                }
            }
        }
    }
}

private struct DeviceRow: View {
    let device: HIDGamepadInfo
    let isSelected: Bool
    let select: () -> Void

    var body: some View {
        Button(action: select) {
            HStack(spacing: 10) {
                Image(systemName: isSelected ? "largecircle.fill.circle" : "circle")
                    .foregroundStyle(isSelected ? Color.accentColor : Color.secondary)
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text(device.name).fontWeight(.medium)
                        if device.isXPad {
                            Text("XPadMac")
                                .font(.caption2.weight(.semibold))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1)
                                .foregroundStyle(.white)
                                .background(Color.accentColor, in: Capsule())
                        }
                    }
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .contentShape(Rectangle())
            .background(isSelected ? Color.accentColor.opacity(0.12) : Color.clear,
                        in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
        .buttonStyle(.plain)
    }

    private var subtitle: String {
        var parts = [device.vidPid, device.kind]
        if let transport = device.transport { parts.append(transport) }
        if let source = device.xpadSource { parts.append(String(localized: "source: \(source)")) }
        return parts.joined(separator: " · ")
    }
}

// MARK: - Measurement

private struct MeasurementCard: View {
    let meter: PollingRateMeter
    let device: HIDGamepadInfo

    private var stats: PollingStats { meter.stats }
    private var isMeasuring: Bool { meter.state == .measuring }
    private var isDisconnected: Bool { meter.isSelectedDisconnected }

    var body: some View {
        Card(verbatim: isDisconnected ? String(localized: "\(device.name) (disconnected)") : device.name,
             systemImage: "speedometer", tint: isMeasuring ? .green : (isDisconnected ? .orange : nil)) {
            controls

            if isDisconnected {
                Label("This controller has disconnected. These are the final results of the measurement; click \"Reset\" or choose another device to measure again.",
                      systemImage: "exclamationmark.triangle")
                    .font(.callout)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }

            identity

            HStack(spacing: 12) {
                StatTile(title: "Current (1 second)", value: PollingFormat.hz(isMeasuring || stats.totalReports > 0 ? stats.currentHz : nil),
                         emphasized: true)
                StatTile(title: "Average", value: PollingFormat.hz(stats.averageHz),
                         note: String(localized: "1000 / mean interval"))
                StatTile(title: "Peak", value: PollingFormat.hz(stats.peakHz),
                         note: String(localized: "highest 1-second window"))
                StatTile(title: "Declared", value: PollingFormat.hz(device.declaredHz),
                         note: device.reportIntervalMicroseconds.map { "ReportInterval \($0) µs" } ?? String(localized: "no ReportInterval"))
            }

            intervals

            RateChart(history: meter.history, declaredHz: device.declaredHz, peakHz: stats.peakHz)
        }
    }

    private var controls: some View {
        HStack(spacing: 12) {
            if isMeasuring {
                Button {
                    meter.stop()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
                .keyboardShortcut(.cancelAction)
            } else {
                Button {
                    meter.start()
                } label: {
                    Label("Start", systemImage: "play.fill")
                }
                .keyboardShortcut(.defaultAction)
                .disabled(isDisconnected)
            }
            Button {
                meter.reset()
            } label: {
                Label("Reset", systemImage: "arrow.counterclockwise")
            }
            .disabled(stats.totalReports == 0 && meter.history.isEmpty)

            if isMeasuring {
                ProgressView().controlSize(.small)
                Text("Measuring… keep moving both analog sticks.").foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
            Text("\(stats.totalReports) reports · \(PollingFormat.duration(stats.elapsed))")
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
        }
        .controlSize(.large)
    }

    private var identity: some View {
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 4) {
            row("VID:PID", device.vidPid)
            row("Transport", device.transport ?? "—")
            row("XPadSource", device.xpadSource ?? String(localized: "— (not through XPadMac)"))
            if !stats.reportIDs.isEmpty {
                row("Report ID", stats.reportIDs.map { "\($0.id): \($0.count)" }.joined(separator: ", "))
            }
            if stats.usedFallbackClock {
                row("Clock", String(localized: "some reports had no IOKit timestamp; mach_absolute_time() at arrival was used"))
            }
        }
        .font(.callout)
    }

    private var intervals: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Intervals between reports").font(.subheadline.weight(.semibold))
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 18, verticalSpacing: 4) {
                GridRow {
                    ForEach(Self.intervalColumns, id: \.self) {
                        Text(verbatim: $0).foregroundStyle(.secondary)
                    }
                }
                .font(.caption)
                GridRow {
                    Text(PollingFormat.ms(stats.minInterval))
                    Text(PollingFormat.ms(stats.meanInterval))
                    Text(PollingFormat.ms(stats.medianInterval))
                    Text(PollingFormat.ms(stats.p99Interval))
                    Text(PollingFormat.ms(stats.maxInterval))
                    Text(PollingFormat.ms(stats.jitter))
                }
                .font(.body.monospacedDigit())
            }
            Text("\(stats.intervalCount) intervals · \(stats.pauses) pauses over \(Int(PollingAccumulator.pauseThreshold)) ms (left out). Median and p99 use 0.01 ms bins (below 20 ms) and 1 ms bins (above 20 ms).")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    /// Column titles of the interval table.
    private static let intervalColumns = [
        String(localized: "Min"), String(localized: "Mean"), String(localized: "Median"),
        "p99", String(localized: "Max"), String(localized: "Jitter"),
    ]

    private func row(_ label: LocalizedStringKey, _ value: String) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(value).textSelection(.enabled)
        }
    }
}

private struct StatTile: View {
    let title: LocalizedStringKey
    let value: String
    var note: String?
    var emphasized = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value)
                .font(emphasized ? .title.weight(.semibold).monospacedDigit() : .title2.monospacedDigit())
                .foregroundStyle(emphasized ? Color.accentColor : Color.primary)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            if let note {
                Text(note).font(.caption2).foregroundStyle(.tertiary).lineLimit(2)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(.separator))
    }
}

/// Current Hz over the last 30 s, with the declared rate as a dashed line.
private struct RateChart: View {
    let history: [RateSample]
    let declaredHz: Double?
    let peakHz: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Hz over time (last 30 seconds)").font(.subheadline.weight(.semibold))
            Chart {
                ForEach(history) { sample in
                    LineMark(x: .value("Time", sample.time), y: .value("Hz", sample.hz))
                        .interpolationMethod(.linear)
                }
                if let declaredHz {
                    RuleMark(y: .value("Declared", declaredHz))
                        .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 4]))
                        .foregroundStyle(.secondary)
                        .annotation(position: .top, alignment: .leading) {
                            Text("declared \(PollingFormat.hz(declaredHz))")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                }
            }
            .chartXScale(domain: xDomain)
            .chartYScale(domain: 0...yMax)
            .chartXAxisLabel("seconds")
            .chartYAxisLabel("Hz")
            .frame(height: 180)
        }
    }

    private var xDomain: ClosedRange<Double> {
        let end = max(history.last?.time ?? 0, PollingRateMeter.historyDuration)
        return (end - PollingRateMeter.historyDuration)...end
    }

    private var yMax: Double {
        let top = max(history.map(\.hz).max() ?? 0, peakHz ?? 0, declaredHz ?? 0, 10)
        // A round ceiling a little above the highest value.
        let step: Double = top > 200 ? 100 : 10
        return ((top * 1.1) / step).rounded(.up) * step
    }
}

// MARK: - Formatting

private enum PollingFormat {
    static func hz(_ value: Double?) -> String {
        guard let value, value.isFinite else { return "—" }
        return value >= 100 ? String(format: "%.0f Hz", value) : String(format: "%.1f Hz", value)
    }

    static func ms(_ value: Double?) -> String {
        guard let value, value.isFinite else { return "—" }
        return String(format: "%.2f ms", value)
    }

    static func duration(_ seconds: Double) -> String {
        String(format: "%.1f s", seconds)
    }
}
