//
//  PollingRateMeter.swift
//  XPadMac
//
//  Measures how often a HID game controller's input reports reach macOS.
//  IOHIDManager lists the gamepads and joysticks (it only enumerates, it
//  opens nothing); the selected one is opened with kIOHIDOptionsTypeNone
//  (shared, not seized: games and GameController keep working) and an
//  input-report callback on the main run loop counts every report with
//  the timestamp IOKit stamped it with. Statistics are aggregated in place
//  (fixed-size histogram, running sums) and published at 10 Hz.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import CoreFoundation
import Darwin
import Foundation
import IOKit
import IOKit.hid
import Observation

// MARK: - Device list

/// One HID gamepad / joystick IOHIDManager reports.
nonisolated struct HIDGamepadInfo: Identifiable, Hashable, Sendable {
    /// IORegistry entry ID of the IOHIDDevice service.
    let id: UInt64
    let name: String
    let manufacturer: String?
    let vendorID: Int
    let productID: Int
    let transport: String?
    /// Set on the HID devices XPadMac's driver publishes.
    let xpadSource: String?
    /// The device's declared report interval ("ReportInterval"), in µs.
    let reportIntervalMicroseconds: Int?
    let maxInputReportSize: Int?
    /// Primary usage (Generic Desktop: 4 joystick, 5 gamepad).
    let primaryUsagePage: Int?
    let primaryUsage: Int?

    var vidPid: String { String(format: "%04X:%04X", vendorID, productID) }

    /// The Xbox Wireless Controller XPadMac's driver publishes.
    var isXPad: Bool { vendorID == 0x045E && productID == 0x0B13 && xpadSource != nil }

    /// "Gamepad" / "Joystick" from the primary usage.
    var kind: String {
        guard primaryUsagePage == 1 else { return "HID" }
        switch primaryUsage {
        case 5: return "Gamepad"
        case 4: return "Joystick"
        default: return "HID"
        }
    }

    /// The declared rate in Hz, from ReportInterval.
    var declaredHz: Double? {
        guard let interval = reportIntervalMicroseconds, interval > 0 else { return nil }
        return 1_000_000 / Double(interval)
    }
}

// MARK: - Published statistics

/// What the view shows, recomputed about ten times a second.
nonisolated struct PollingStats: Equatable, Sendable {
    /// Input reports since Start (or the last Reset).
    var totalReports = 0
    /// Seconds since Start (or the last Reset).
    var elapsed: Double = 0
    /// Reports in the last second (sliding 1 s window).
    var currentHz: Double = 0
    /// 1000 / mean interval: the rate while the pad is sending (pauses excluded).
    var averageHz: Double?
    /// Highest full 1 s window of the session.
    var peakHz: Double?
    /// Intervals between consecutive reports, in ms (pauses excluded).
    var minInterval: Double?
    var meanInterval: Double?
    var medianInterval: Double?
    var p99Interval: Double?
    var maxInterval: Double?
    /// Standard deviation of the intervals, in ms.
    var jitter: Double?
    /// Intervals counted in the statistics above.
    var intervalCount = 0
    /// Silences longer than `PollingAccumulator.pauseThreshold` (not counted as intervals).
    var pauses = 0
    /// Report IDs seen, with their counts, in ID order.
    var reportIDs: [ReportIDCount] = []
    /// True when some report came without an IOKit timestamp and
    /// mach_absolute_time() at delivery was used instead.
    var usedFallbackClock = false

    nonisolated struct ReportIDCount: Hashable, Sendable {
        let id: Int
        let count: Int
    }
}

/// One point of the rolling Hz chart.
nonisolated struct RateSample: Identifiable, Hashable, Sendable {
    /// Seconds since Start; also the identity (strictly increasing).
    let time: Double
    let hz: Double

    var id: Double { time }
}

// MARK: - Meter

@MainActor
@Observable
final class PollingRateMeter {
    enum State: Equatable {
        case idle
        case measuring
    }

    /// Gamepads and joysticks, XPadMac's first.
    private(set) var devices: [HIDGamepadInfo] = []
    /// The device to measure. Changing it stops a measurement in progress
    /// and clears the numbers (they belonged to the previous device).
    var selectedID: UInt64? {
        didSet {
            guard selectedID != oldValue else { return }
            if disconnectedDevice?.id != selectedID { disconnectedDevice = nil }
            guard session?.deviceID != selectedID else { return }
            stop()
            resetStatistics()
        }
    }
    /// The selected device after it was unplugged, kept (with its final
    /// numbers) until the user picks another device or presses Reset.
    private(set) var disconnectedDevice: HIDGamepadInfo?
    private(set) var state: State = .idle
    private(set) var stats = PollingStats()
    /// Current Hz, ten points a second, for the last `historyDuration` seconds.
    private(set) var history: [RateSample] = []
    private(set) var message: StatusMessage?
    /// True while the section is on screen and IOHIDManager lists devices.
    private(set) var isListing = false

    var selectedDevice: HIDGamepadInfo? {
        if let device = devices.first(where: { $0.id == selectedID }) { return device }
        if let disconnectedDevice, disconnectedDevice.id == selectedID { return disconnectedDevice }
        return nil
    }

    /// True when the selected device is one that was unplugged.
    var isSelectedDisconnected: Bool {
        selectedID != nil && disconnectedDevice?.id == selectedID && !devices.contains { $0.id == selectedID }
    }

    /// Seconds of history the chart shows.
    static let historyDuration: Double = 30
    private static let publishInterval: Duration = .milliseconds(100)
    /// Shown when the device being measured goes away.
    private static var disconnectedWhileMeasuring: String {
        String(localized: "The controller being measured has disconnected. Measuring stopped; the final results are still shown below.")
    }

    @ObservationIgnored private var manager: IOHIDManager?
    @ObservationIgnored private var session: PollingSession?
    @ObservationIgnored private var publishTask: Task<Void, Never>?
    /// (time, report count) at each publish tick of the last second and a bit.
    @ObservationIgnored private var windowSamples: [(ticks: UInt64, count: Int)] = []

    // MARK: Lifetime

    /// Starts listing devices; called when the section appears.
    func activate() {
        guard manager == nil else { return }
        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let matching: [[String: Int]] = [
            [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_GamePad],
            [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_Joystick],
        ]
        IOHIDManagerSetDeviceMatchingMultiple(manager, matching as CFArray)
        // The meter outlives the manager (deactivate() unschedules it before
        // dropping it), so an unretained context is safe.
        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(manager, pollingDeviceMatched, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, pollingDeviceRemoved, context)
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        // The manager is never opened: it only enumerates. Only the device
        // being measured is opened, on its own (see PollingSession).
        self.manager = manager
        isListing = true
        reloadDevices()
    }

    /// Stops measuring and listing; called when the section disappears.
    func deactivate() {
        stop()
        if let manager {
            IOHIDManagerRegisterDeviceMatchingCallback(manager, nil, nil)
            IOHIDManagerRegisterDeviceRemovalCallback(manager, nil, nil)
            IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        }
        manager = nil
        isListing = false
    }

    // MARK: Device list

    /// Re-reads the manager's device set. `removed` (the address of an
    /// IOHIDDevice being removed) is left out even if still in the set.
    fileprivate func reloadDevices(removed: UInt? = nil) {
        guard let manager else {
            devices = []
            return
        }
        let set = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> ?? []
        var list: [HIDGamepadInfo] = []
        for device in set where UInt(bitPattern: Unmanaged.passUnretained(device).toOpaque()) != removed {
            if let info = HIDProperties.info(for: device) { list.append(info) }
        }
        list.sort {
            if $0.isXPad != $1.isXPad { return $0.isXPad }
            return ($0.name, $0.id) < ($1.name, $1.id)
        }
        // Read before `devices` changes: the selected device's last known info.
        let previous = selectedDevice
        devices = list

        if let selectedID, !list.contains(where: { $0.id == selectedID }) {
            let hasResults = state == .measuring || stats.totalReports > 0 || !history.isEmpty
            if hasResults, let previous {
                // Keep the device and its numbers on screen: stop() publishes
                // the final ones; the selection stays, so nothing is reset and
                // no other device is selected in its place.
                if state == .measuring {
                    message = StatusMessage(kind: .warning, text: Self.disconnectedWhileMeasuring)
                }
                stop()
                disconnectedDevice = previous
            } else {
                self.selectedID = nil  // stops the session
            }
        } else if let selectedID, disconnectedDevice?.id == selectedID {
            disconnectedDevice = nil  // it is back
        }
        if selectedID == nil {
            selectedID = list.first?.id
        }
    }

    /// The manager's IOHIDDevice for a registry entry ID.
    private func managerDevice(for id: UInt64) -> IOHIDDevice? {
        guard let manager, let set = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else { return nil }
        return set.first { HIDProperties.entryID(of: $0) == id }
    }

    // MARK: Measurement

    func start() {
        guard state == .idle else { return }
        guard !isSelectedDisconnected else {
            message = StatusMessage(kind: .warning, text: String(localized: "This controller has disconnected. Plug it back in or choose another controller in the list."))
            return
        }
        guard let info = selectedDevice, let source = managerDevice(for: info.id) else {
            message = StatusMessage(kind: .warning, text: String(localized: "Choose a controller in the list."))
            return
        }
        switch PollingSession.open(source: source, info: info, accumulator: PollingAccumulator(), onRemoved: { [weak self] in self?.sessionDeviceRemoved() }) {
        case .success(let session):
            self.session = session
            message = nil
            state = .measuring
            resetStatistics()
            publishTask = Task { [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(for: Self.publishInterval)
                    guard !Task.isCancelled, let self else { return }
                    self.publish()
                }
            }
        case .failure(let failure):
            message = failure.message
        }
    }

    func stop() {
        publishTask?.cancel()
        publishTask = nil
        if let session {
            // A last update so the numbers on screen are the final ones.
            publish(from: session.accumulator)
            session.close()
        }
        session = nil
        state = .idle
    }

    /// Clears the statistics; the measurement (if any) goes on. For an
    /// unplugged device it also lets it go and selects a connected one.
    func reset() {
        if isSelectedDisconnected {
            disconnectedDevice = nil
            message = nil
            selectedID = devices.first?.id
            resetStatistics()
            return
        }
        session?.accumulator.reset()
        resetStatistics()
    }

    private func resetStatistics() {
        stats = PollingStats()
        history = []
        windowSamples = []
    }

    private func sessionDeviceRemoved() {
        guard state == .measuring else { return }
        // Keep what was measured: stop() publishes the final numbers, and
        // reloadDevices() keeps the device on screen once the manager drops it
        // (the numbers count as results, so the selection is not cleared).
        stop()
        message = StatusMessage(kind: .warning, text: Self.disconnectedWhileMeasuring)
        reloadDevices()
    }

    // MARK: Publishing (10 Hz)

    private func publish() {
        guard let session else { return }
        publish(from: session.accumulator)
    }

    private func publish(from accumulator: PollingAccumulator) {
        let now = mach_absolute_time()
        let count = accumulator.count

        // Sliding 1 s window from the per-tick samples: reports between the
        // oldest sample at least 1 s old (or the start) and now.
        if windowSamples.isEmpty { windowSamples.append((accumulator.startTicks, 0)) }
        windowSamples.append((now, count))
        let oneSecond = PollingClock.ticks(seconds: 1)
        while windowSamples.count > 2, now &- windowSamples[1].ticks >= oneSecond {
            windowSamples.removeFirst()
        }
        let origin = windowSamples[0]
        let span = PollingClock.seconds(now &- origin.ticks)
        let currentHz = span > 0 ? Double(count - origin.count) / span : 0

        var stats = accumulator.snapshot(now: now)
        stats.currentHz = currentHz
        // Only full windows count for the peak; a partial first window would
        // overstate a burst.
        if span >= 0.95 {
            stats.peakHz = max(self.stats.peakHz ?? 0, currentHz)
        } else {
            stats.peakHz = self.stats.peakHz
        }
        if stats != self.stats { self.stats = stats }

        let sample = RateSample(time: stats.elapsed, hz: currentHz)
        if let last = history.last, last.time >= sample.time { return }
        history.append(sample)
        let cutoff = sample.time - Self.historyDuration
        if let firstKept = history.firstIndex(where: { $0.time >= cutoff }), firstKept > 0 {
            history.removeFirst(firstKept)
        }
    }

    // MARK: IOHIDManager callbacks (main run loop)

    fileprivate func deviceRemoved(address: UInt) {
        reloadDevices(removed: address)
        // The manager may still hold the device while it calls back: sync
        // again once it has let go.
        Task { [weak self] in self?.reloadDevices() }
    }
}

// The C callbacks run on the main run loop (where everything is scheduled).
// They capture nothing: the context pointer leads back to the object.

private nonisolated func pollingDeviceMatched(_ context: UnsafeMutableRawPointer?, _ result: IOReturn,
                                              _ sender: UnsafeMutableRawPointer?, _ device: IOHIDDevice) {
    guard let context else { return }
    let meter = Unmanaged<PollingRateMeter>.fromOpaque(context).takeUnretainedValue()
    MainActor.assumeIsolated { meter.reloadDevices() }
}

private nonisolated func pollingDeviceRemoved(_ context: UnsafeMutableRawPointer?, _ result: IOReturn,
                                              _ sender: UnsafeMutableRawPointer?, _ device: IOHIDDevice) {
    guard let context else { return }
    let meter = Unmanaged<PollingRateMeter>.fromOpaque(context).takeUnretainedValue()
    let address = UInt(bitPattern: Unmanaged.passUnretained(device).toOpaque())
    MainActor.assumeIsolated { meter.deviceRemoved(address: address) }
}

// MARK: - Session

/// One open device being measured. It has its own IOHIDDevice (created from
/// the same service, not the manager's) so that scheduling, opening and the
/// callbacks belong to the session alone.
///
/// Not Sendable and not actor-isolated: the meter (main actor) owns it and
/// uses it only there, and its deinit can then release the device and the
/// report buffer itself.
nonisolated final class PollingSession {
    struct Failure: Error {
        let message: StatusMessage
    }

    let deviceID: UInt64
    /// Main-actor isolated; the callbacks reach it on the main run loop.
    let accumulator: PollingAccumulator
    /// Called (on the main actor, after the callback returned) when the
    /// device goes away.
    let onRemoved: @MainActor () -> Void

    /// Reports the connection's queue holds (about 256 ms at 1000 Hz).
    private static let reportBufferCount = 256

    private let device: IOHIDDevice
    private let buffer: UnsafeMutablePointer<UInt8>
    private let bufferLength: Int
    private var isOpen = true

    private init(device: IOHIDDevice, deviceID: UInt64, bufferLength: Int, accumulator: PollingAccumulator,
                 onRemoved: @escaping @MainActor () -> Void) {
        self.device = device
        self.deviceID = deviceID
        self.bufferLength = bufferLength
        self.accumulator = accumulator
        buffer = .allocate(capacity: bufferLength)
        buffer.initialize(repeating: 0, count: bufferLength)
        self.onRemoved = onRemoved
    }

    deinit {
        close()
        buffer.deallocate()
    }

    /// Opens the device (shared, not seized) and starts counting its input
    /// reports into `accumulator`. Call on the main thread: everything is
    /// scheduled on the main run loop.
    static func open(source: IOHIDDevice, info: HIDGamepadInfo, accumulator: PollingAccumulator,
                     onRemoved: @escaping @MainActor () -> Void) -> Result<PollingSession, Failure> {
        let service = IOHIDDeviceGetService(source)
        guard service != IO_OBJECT_NULL, let device = IOHIDDeviceCreate(kCFAllocatorDefault, service) else {
            return .failure(Failure(message: StatusMessage(kind: .error, text: String(localized: "Could not connect to the controller."),
                                                           detail: String(localized: "IOHIDDeviceCreate failed."))))
        }
        let result = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else {
            return .failure(Failure(message: openFailureMessage(result)))
        }
        // A deeper report queue for this connection (IOHIDKeys.h: count ×
        // entry size, capped at 131072 bytes), so a main-thread stall of a
        // few hundred ms at 1000 Hz does not drop reports. Set before the
        // callback is registered; the entry size is left to IOKit.
        IOHIDDeviceSetProperty(device, kIOHIDMaxReportBufferCountKey as CFString,
                               reportBufferCount as CFNumber)

        // Room for the largest input report (+1 for the report ID byte).
        let length = max((info.maxInputReportSize ?? 0) + 1, 64)
        let session = PollingSession(device: device, deviceID: info.id, bufferLength: length,
                                     accumulator: accumulator, onRemoved: onRemoved)
        // close() (also run by deinit) unregisters the callbacks before the
        // session can go away, so an unretained context is safe.
        let context = Unmanaged.passUnretained(session).toOpaque()
        // Common modes: reports keep coming while a menu or a drag tracks
        // the mouse. Reports that wait in the queue keep their IOKit
        // timestamps, so a late delivery does not skew the intervals; a
        // report dropped from a full queue does (two gaps become one), so
        // while the main thread is busy max and p99 are upper bounds.
        IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        IOHIDDeviceRegisterRemovalCallback(device, pollingSessionDeviceRemoved, context)
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(device, session.buffer, length,
                                                            pollingInputReport, context)
        return .success(session)
    }

    /// Stops the callbacks and closes the device. Idempotent.
    func close() {
        guard isOpen else { return }
        isOpen = false
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(device, buffer, bufferLength, nil, nil)
        IOHIDDeviceRegisterRemovalCallback(device, nil, nil)
        IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
        IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    private static func openFailureMessage(_ result: IOReturn) -> StatusMessage {
        let code = String(format: "IOHIDDeviceOpen: 0x%08X", UInt32(bitPattern: result))
        switch result {
        case kIOReturnNotPermitted:
            return StatusMessage(
                kind: .error,
                text: String(localized: "macOS does not let XPadMac open this device (kIOReturnNotPermitted). A controller normally needs no permission, but a device that also declares a keyboard or mouse needs Input Monitoring: go to System Settings › Privacy & Security › Input Monitoring, turn on XPadMac, then reopen the app."),
                detail: code)
        case kIOReturnExclusiveAccess:
            return StatusMessage(
                kind: .error,
                text: String(localized: "Another app has exclusive access to this device (kIOReturnExclusiveAccess). Quit that app and try again."),
                detail: code)
        default:
            return StatusMessage(kind: .error, text: String(localized: "Could not open the controller to measure it."), detail: code)
        }
    }
}

private nonisolated func pollingInputReport(_ context: UnsafeMutableRawPointer?, _ result: IOReturn,
                                            _ sender: UnsafeMutableRawPointer?, _ type: IOHIDReportType,
                                            _ reportID: UInt32, _ report: UnsafeMutablePointer<UInt8>,
                                            _ reportLength: CFIndex, _ timeStamp: UInt64) {
    guard let context, result == kIOReturnSuccess else { return }
    let accumulator = Unmanaged<PollingSession>.fromOpaque(context).takeUnretainedValue().accumulator
    MainActor.assumeIsolated { accumulator.record(timestamp: timeStamp, reportID: reportID) }
}

private nonisolated func pollingSessionDeviceRemoved(_ context: UnsafeMutableRawPointer?, _ result: IOReturn,
                                                     _ sender: UnsafeMutableRawPointer?) {
    guard let context else { return }
    let onRemoved = Unmanaged<PollingSession>.fromOpaque(context).takeUnretainedValue().onRemoved
    // Leave IOKit's callback before the device is torn down.
    Task { @MainActor in onRemoved() }
}

// MARK: - Accumulator (hot path, up to ~1000 calls a second)

/// Constant-time, allocation-free aggregation of report timestamps.
@MainActor
final class PollingAccumulator {
    /// A silence longer than this is a pause (GIP pads send only on change),
    /// not an interval: it would swamp the mean and the jitter.
    static let pauseThreshold: Double = 250  // ms

    /// Histogram: 0.01 ms bins up to 20 ms, then 1 ms bins up to the pause
    /// threshold. Bins are centred on multiples of their width (bin k holds
    /// k × width ± width / 2), so evenly spaced reports read back exactly.
    private static let fineBin = 0.01
    private static let fineLimit = 20.0
    private static let fineBins = Int(fineLimit / fineBin)  // 2000
    private static let coarseBins = Int(pauseThreshold - fineLimit) + 1  // 231

    private(set) var count = 0
    private(set) var startTicks: UInt64 = mach_absolute_time()
    private var lastTicks: UInt64 = 0
    private var intervals = 0
    private var mean = 0.0  // ms (Welford)
    private var m2 = 0.0
    private var minimum = Double.infinity
    private var maximum = 0.0
    private var pauses = 0
    private var usedFallbackClock = false
    private var histogram = [Int](repeating: 0, count: fineBins + coarseBins)
    private var idCounts = [Int](repeating: 0, count: 256)

    func reset() {
        count = 0
        startTicks = mach_absolute_time()
        lastTicks = 0
        intervals = 0
        mean = 0
        m2 = 0
        minimum = .infinity
        maximum = 0
        pauses = 0
        usedFallbackClock = false
        for index in histogram.indices { histogram[index] = 0 }
        for index in idCounts.indices { idCounts[index] = 0 }
    }

    /// One input report. `timestamp` is IOKit's (mach absolute time); 0
    /// means none was given, and the time of delivery is used instead.
    func record(timestamp: UInt64, reportID: UInt32) {
        var ticks = timestamp
        if ticks == 0 {
            ticks = mach_absolute_time()
            usedFallbackClock = true
        }
        count += 1
        idCounts[Int(reportID & 0xFF)] += 1
        defer { if ticks >= lastTicks { lastTicks = ticks } }
        guard lastTicks != 0, ticks >= lastTicks else { return }

        let ms = PollingClock.milliseconds(ticks - lastTicks)
        guard ms <= Self.pauseThreshold else {
            pauses += 1
            return
        }
        intervals += 1
        let delta = ms - mean
        mean += delta / Double(intervals)
        m2 += delta * (ms - mean)
        if ms < minimum { minimum = ms }
        if ms > maximum { maximum = ms }
        // Fine bins 0..<2000 cover [0, 19.995) ms; coarse bin 0 is 20 ms.
        let fine = Int((ms / Self.fineBin).rounded())
        let bin = fine < Self.fineBins
            ? fine
            : Self.fineBins + max(0, Int((ms - Self.fineLimit).rounded()))
        histogram[min(bin, histogram.count - 1)] += 1
    }

    /// Everything but the 1 s window, which the meter computes.
    func snapshot(now: UInt64) -> PollingStats {
        var stats = PollingStats()
        stats.totalReports = count
        stats.elapsed = PollingClock.seconds(now &- startTicks)
        stats.pauses = pauses
        stats.usedFallbackClock = usedFallbackClock
        stats.intervalCount = intervals
        if intervals > 0 {
            stats.meanInterval = mean
            stats.minInterval = minimum
            stats.maxInterval = maximum
            stats.medianInterval = percentile(0.5)
            stats.p99Interval = percentile(0.99)
            if mean > 0 { stats.averageHz = 1000 / mean }
        }
        if intervals > 1 {
            stats.jitter = (m2 / Double(intervals - 1)).squareRoot()
        }
        stats.reportIDs = idCounts.indices.compactMap { id in
            idCounts[id] > 0 ? PollingStats.ReportIDCount(id: id, count: idCounts[id]) : nil
        }
        return stats
    }

    /// The centre of the bin holding the given fraction of the intervals,
    /// clamped to the observed range (within 0.005 ms below 20 ms, 0.5 ms
    /// above).
    private func percentile(_ fraction: Double) -> Double {
        let target = max(1, Int((Double(intervals) * fraction).rounded(.up)))
        var seen = 0
        for (bin, binCount) in histogram.enumerated() where binCount > 0 {
            seen += binCount
            if seen >= target {
                let centre = bin < Self.fineBins
                    ? Double(bin) * Self.fineBin
                    : Self.fineLimit + Double(bin - Self.fineBins)
                return min(max(centre, minimum), maximum)
            }
        }
        return maximum
    }
}

// MARK: - Helpers

/// mach_absolute_time() units (IOKit's timestamps use them too).
nonisolated enum PollingClock {
    private static let nanosecondsPerTick: Double = {
        var timebase = mach_timebase_info_data_t()
        mach_timebase_info(&timebase)
        return timebase.denom == 0 ? 1 : Double(timebase.numer) / Double(timebase.denom)
    }()

    static func seconds(_ ticks: UInt64) -> Double { Double(ticks) * nanosecondsPerTick / 1e9 }
    static func milliseconds(_ ticks: UInt64) -> Double { Double(ticks) * nanosecondsPerTick / 1e6 }
    static func ticks(seconds: Double) -> UInt64 { UInt64(seconds * 1e9 / nanosecondsPerTick) }
}

/// Reading an IOHIDDevice's properties.
nonisolated enum HIDProperties {
    static func info(for device: IOHIDDevice) -> HIDGamepadInfo? {
        let id = entryID(of: device)
        guard id != 0 else { return nil }
        let vendorID = int(device, kIOHIDVendorIDKey) ?? 0
        let productID = int(device, kIOHIDProductIDKey) ?? 0
        return HIDGamepadInfo(
            id: id,
            name: string(device, kIOHIDProductKey)
                ?? String(localized: "HID device \(String(format: "%04X:%04X", vendorID, productID))"),
            manufacturer: string(device, kIOHIDManufacturerKey),
            vendorID: vendorID,
            productID: productID,
            transport: string(device, kIOHIDTransportKey),
            xpadSource: string(device, "XPadSource"),
            reportIntervalMicroseconds: int(device, kIOHIDReportIntervalKey),
            maxInputReportSize: int(device, kIOHIDMaxInputReportSizeKey),
            primaryUsagePage: int(device, kIOHIDPrimaryUsagePageKey),
            primaryUsage: int(device, kIOHIDPrimaryUsageKey))
    }

    static func entryID(of device: IOHIDDevice) -> UInt64 {
        let service = IOHIDDeviceGetService(device)
        guard service != IO_OBJECT_NULL else { return 0 }
        var id: UInt64 = 0
        return IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS ? id : 0
    }

    private static func int(_ device: IOHIDDevice, _ key: String) -> Int? {
        (IOHIDDeviceGetProperty(device, key as CFString) as? NSNumber)?.intValue
    }

    private static func string(_ device: IOHIDDevice, _ key: String) -> String? {
        IOHIDDeviceGetProperty(device, key as CFString) as? String
    }
}
