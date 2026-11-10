//
//  USBDiagnostics.swift
//  XPadMac
//
//  Reads the IORegistry (read-only, no device is opened) to show which Xbox
//  controllers (GIP or XInput) are attached over USB, how they are
//  configured and which driver classes sit on them: XPadMac's own
//  (XPadGIPDevice / XPadGIPGamepad / XPadGIPAudio / XPad360Gamepad),
//  Apple's, or none.
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//

import Foundation
import IOKit
import Observation

// MARK: - Model

@MainActor
@Observable
final class USBDiagnosticsModel {
    private(set) var devices: [USBDeviceReport] = []
    private(set) var isScanning = false
    private(set) var lastScan: Date?
    /// False if IOKit refused the hotplug notifications (refresh by hand then).
    private(set) var watchesHotplug = false

    @ObservationIgnored private var rescanQueued = false
    @ObservationIgnored private var pendingRefresh: Task<Void, Never>?
    /// Held only to keep its IOKit notifications alive.
    @ObservationIgnored private var hotplugWatcher: USBHotplugWatcher?

    /// USB devices arrive in bursts (device, then interfaces, then drivers),
    /// so a hotplug event triggers one scan after things settle.
    private static let hotplugDelay: Duration = .milliseconds(800)

    init() {
        let watcher = USBHotplugWatcher { [weak self] in self?.scheduleRefresh() }
        hotplugWatcher = watcher
        watchesHotplug = watcher.isActive
        refresh()
    }

    /// Scans the IORegistry off the main thread and publishes the result.
    func refresh() {
        guard !isScanning else {
            rescanQueued = true
            return
        }
        isScanning = true
        Task { [weak self] in
            let devices = await Task.detached(priority: .userInitiated) {
                USBRegistryScanner.scan()
            }.value
            self?.publish(devices)
        }
    }

    private func publish(_ devices: [USBDeviceReport]) {
        self.devices = devices
        lastScan = Date()
        isScanning = false
        if rescanQueued {
            rescanQueued = false
            refresh()
        }
    }

    private func scheduleRefresh() {
        pendingRefresh?.cancel()
        pendingRefresh = Task { [weak self] in
            try? await Task.sleep(for: Self.hotplugDelay)
            guard !Task.isCancelled else { return }
            self?.refresh()
        }
    }
}

// MARK: - Reports

/// One Xbox-protocol USB device and what is attached below it.
nonisolated struct USBDeviceReport: Identifiable, Hashable, Sendable {
    /// IORegistry entry ID.
    let id: UInt64
    let vendorID: Int
    let productID: Int
    let productName: String
    let vendorName: String?
    let locationID: Int?
    /// bConfigurationValue in use; nil when no driver has configured the device.
    let configuration: Int?
    /// Every registry entry below the device, depth first.
    let nodes: [RegistryNode]

    var vidPid: String { String(format: "%04X:%04X", vendorID, productID) }

    /// Entries that drive the device or one of its interfaces.
    var drivers: [RegistryNode] { nodes.filter(\.isDriver) }

    var verdict: DriverVerdict {
        let drivers = drivers
        if drivers.contains(where: { $0.origin == .xpad }) { return .xpad }
        if drivers.isEmpty { return .none }
        return drivers.allSatisfy { $0.origin == .apple } ? .apple : .thirdParty
    }

    /// The HID identity published below the device (VendorID/ProductID of the
    /// shallowest HID node), e.g. 045E:0B13 when XPadMac's driver is active.
    var hidIdentity: HIDIdentity? { nodes.lazy.compactMap(\.hid).first }

    /// XPadSource of the HID device XPadMac's driver publishes, if any.
    var xpadSource: String? { nodes.lazy.compactMap(\.xpadSource).first }

    /// Processes with raw USB access to the device or one of its interfaces
    /// (a browser using WebUSB, adb, a vendor tool, xpadctl…).
    var clientProcesses: [String] {
        var seen = Set<String>()
        return nodes.filter(\.isUSBClient).map(\.name).filter { seen.insert($0).inserted }
    }
}

nonisolated enum DriverVerdict: Hashable, Sendable {
    /// One of XPadMac's driver classes is attached.
    case xpad
    /// Only Apple's drivers are attached.
    case apple
    /// Some other vendor's driver is attached.
    case thirdParty
    /// Nothing drives the device or its interfaces.
    case none
}

nonisolated enum DriverOrigin: Hashable, Sendable { case xpad, apple, thirdParty }

/// One IORegistry entry (service plane) below a USB device.
nonisolated struct RegistryNode: Identifiable, Hashable, Sendable {
    let id: UInt64
    /// 1 = direct child of the device.
    let depth: Int
    /// Registry name; for a user client, the name of the process that owns it.
    let name: String
    /// The kernel class (IOObjectCopyClass).
    let className: String
    /// IOUserClass: the DriverKit class behind an IOUserService / AppleUserHIDDevice.
    let userClass: String?
    let bundleIdentifier: String?
    /// Set on IOUSBHostInterface entries.
    let interface: InterfaceInfo?
    /// VendorID/ProductID/Transport, set on HID device entries.
    let hid: HIDIdentity?
    /// XPadMac's diagnostic property on the HID devices its driver publishes.
    let xpadSource: String?
    /// Attached straight to the device or to one of its interfaces, and
    /// neither an interface nor a user client: i.e. a driver.
    let isDriver: Bool
    /// A user client straight on the device or one of its interfaces: a
    /// process using the device through IOUSBHost (its name is the process's).
    let isUSBClient: Bool

    var isUserClient: Bool { className.hasSuffix("UserClient") }

    /// The class to show: the DriverKit class when there is one.
    var displayClass: String { userClass ?? className }

    var origin: DriverOrigin {
        if userClass?.hasPrefix("XPad") == true { return .xpad }
        if let bundleIdentifier { return bundleIdentifier.hasPrefix("com.apple.") ? .apple : .thirdParty }
        // Kernel classes without a bundle identifier come from Apple's families.
        return displayClass.hasPrefix("Apple") || displayClass.hasPrefix("IO") ? .apple : .thirdParty
    }
}

nonisolated struct InterfaceInfo: Hashable, Sendable {
    let number: Int
    let interfaceClass: Int
    let subclass: Int
    let interfaceProtocol: Int

    /// e.g. "#0 FF/47/D0" (the GIP interface).
    var label: String {
        String(format: "#%d %02X/%02X/%02X", number, interfaceClass, subclass, interfaceProtocol)
    }

    var isGIP: Bool { interfaceClass == 0xFF && subclass == 0x47 && interfaceProtocol == 0xD0 }

    /// The Xbox 360 wired (XInput) gamepad interface, which XPad360Gamepad drives.
    var isXInput: Bool { interfaceClass == 0xFF && subclass == 0x5D && interfaceProtocol == 0x01 }
}

nonisolated struct HIDIdentity: Hashable, Sendable {
    let vendorID: Int
    let productID: Int
    let transport: String?
    let product: String?

    var vidPid: String { String(format: "%04X:%04X", vendorID, productID) }
}

// MARK: - IORegistry scanner

/// Synchronous, read-only IORegistry walk. Safe to call from any thread.
nonisolated enum USBRegistryScanner {
    /// Microsoft and the third-party vendors XPadMac's personalities match
    /// (Driver/Info.plist): 8BitDo, GameSir, Hori, Hyperkin, Mad Catz, PDP,
    /// PowerA (two VIDs), Razer, Turtle Beach.
    static let vendorIDs: Set<Int> = [0x045E, 0x2DC8, 0x3537, 0x0F0D, 0x2E24, 0x0738, 0x0E6F, 0x24C6,
                                      0x20D6, 0x1532, 0x10F5]

    /// HID stacks are about six levels deep; stop well below runaway trees.
    private static let maxDepth = 10

    static func scan() -> [USBDeviceReport] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator)
                == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }

        var reports: [USBDeviceReport] = []
        while case let device = IOIteratorNext(iterator), device != 0 {
            defer { IOObjectRelease(device) }
            // These vendors also make mice, keyboards and headsets: only their
            // vendor-class (255) devices count, or an Xbox interface whatever
            // the vendor.
            guard let vendorID = intProperty(device, "idVendor"),
                  (vendorIDs.contains(vendorID) && intProperty(device, "bDeviceClass") == 0xFF)
                    || hasXboxInterface(device) else { continue }
            reports.append(report(for: device, vendorID: vendorID))
        }
        return reports.sorted { ($0.locationID ?? 0, $0.id) < ($1.locationID ?? 0, $1.id) }
    }

    /// True if the device has an Xbox interface (vendor class 255, subclass
    /// 0x5D XInput or 0x47 GIP), whatever its vendor: 8BitDo, clones...
    private static func hasXboxInterface(_ device: io_registry_entry_t) -> Bool {
        var iterator: io_iterator_t = 0
        guard IORegistryEntryGetChildIterator(device, kIOServicePlane, &iterator) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(iterator) }
        while case let child = IOIteratorNext(iterator), child != 0 {
            defer { IOObjectRelease(child) }
            guard IOObjectConformsTo(child, "IOUSBHostInterface") != 0 else { continue }
            if intProperty(child, "bInterfaceClass") == 0xFF,
               let sub = intProperty(child, "bInterfaceSubClass"), sub == 0x5D || sub == 0x47 {
                return true
            }
        }
        return false
    }

    private static func report(for device: io_registry_entry_t, vendorID: Int) -> USBDeviceReport {
        var nodes: [RegistryNode] = []
        collectChildren(of: device, depth: 1, parentIsAttachPoint: true, into: &nodes)
        return USBDeviceReport(
            id: entryID(device),
            vendorID: vendorID,
            productID: intProperty(device, "idProduct") ?? 0,
            productName: stringProperty(device, "USB Product Name")
                ?? stringProperty(device, "kUSBProductString")
                ?? String(localized: "USB device"),
            vendorName: stringProperty(device, "USB Vendor Name") ?? stringProperty(device, "kUSBVendorString"),
            locationID: intProperty(device, "locationID"),
            configuration: intProperty(device, "kUSBCurrentConfiguration"),
            nodes: nodes)
    }

    /// Depth-first walk of the service plane below `entry`. A hub's
    /// downstream devices are separate devices and are not descended into.
    private static func collectChildren(of entry: io_registry_entry_t, depth: Int, parentIsAttachPoint: Bool,
                                        into nodes: inout [RegistryNode]) {
        guard depth <= maxDepth else { return }
        var children: io_iterator_t = 0
        guard IORegistryEntryGetChildIterator(entry, kIOServicePlane, &children) == KERN_SUCCESS else { return }
        defer { IOObjectRelease(children) }

        while case let child = IOIteratorNext(children), child != 0 {
            defer { IOObjectRelease(child) }
            if IOObjectConformsTo(child, "IOUSBHostDevice") != 0 { continue }
            let node = makeNode(child, depth: depth, parentIsAttachPoint: parentIsAttachPoint)
            nodes.append(node)
            collectChildren(of: child, depth: depth + 1, parentIsAttachPoint: node.interface != nil, into: &nodes)
        }
    }

    private static func makeNode(_ entry: io_registry_entry_t, depth: Int, parentIsAttachPoint: Bool) -> RegistryNode {
        let className = IOObjectCopyClass(entry)?.takeRetainedValue() as String? ?? "?"

        var interface: InterfaceInfo?
        if className == "IOUSBHostInterface" {
            interface = InterfaceInfo(
                number: intProperty(entry, "bInterfaceNumber") ?? -1,
                interfaceClass: intProperty(entry, "bInterfaceClass") ?? 0,
                subclass: intProperty(entry, "bInterfaceSubClass") ?? 0,
                interfaceProtocol: intProperty(entry, "bInterfaceProtocol") ?? 0)
        }

        var hid: HIDIdentity?
        if let vendorID = intProperty(entry, "VendorID"), let productID = intProperty(entry, "ProductID") {
            hid = HIDIdentity(vendorID: vendorID, productID: productID,
                              transport: stringProperty(entry, "Transport"),
                              product: stringProperty(entry, "Product"))
        }

        let isUserClient = className.hasSuffix("UserClient")
        return RegistryNode(
            id: entryID(entry),
            depth: depth,
            name: registryName(entry) ?? className,
            className: className,
            userClass: stringProperty(entry, "IOUserClass"),
            bundleIdentifier: stringProperty(entry, "CFBundleIdentifier"),
            interface: interface,
            hid: hid,
            xpadSource: stringProperty(entry, "XPadSource"),
            isDriver: parentIsAttachPoint && interface == nil && !isUserClient,
            isUSBClient: parentIsAttachPoint && isUserClient)
    }

    // MARK: Property helpers

    private static func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    private static func intProperty(_ entry: io_registry_entry_t, _ key: String) -> Int? {
        (property(entry, key) as? NSNumber)?.intValue
    }

    private static func stringProperty(_ entry: io_registry_entry_t, _ key: String) -> String? {
        property(entry, key) as? String
    }

    private static func entryID(_ entry: io_registry_entry_t) -> UInt64 {
        var id: UInt64 = 0
        IORegistryEntryGetRegistryEntryID(entry, &id)
        return id
    }

    private static func registryName(_ entry: io_registry_entry_t) -> String? {
        // io_name_t is char[128].
        var buffer = [CChar](repeating: 0, count: 128)
        let result = buffer.withUnsafeMutableBufferPointer { pointer -> String? in
            guard let base = pointer.baseAddress, IORegistryEntryGetName(entry, base) == KERN_SUCCESS else { return nil }
            return String(cString: base)
        }
        return result
    }
}

// MARK: - Hotplug

/// Calls `onChange` (on the main actor) whenever an IOUSBHostDevice appears
/// or goes away. IOKit delivers on the main queue.
nonisolated final class USBHotplugWatcher {
    private let port: IONotificationPortRef?
    private let onChange: @MainActor () -> Void
    private var iterators: [io_iterator_t] = []

    /// False if IOKit refused every notification.
    var isActive: Bool { !iterators.isEmpty }

    init(onChange: @escaping @MainActor () -> Void) {
        port = IONotificationPortCreate(kIOMainPortDefault)
        self.onChange = onChange
        guard let port else { return }
        IONotificationPortSetDispatchQueue(port, .main)

        // The port only lives as long as self (destroyed in deinit), so an
        // unretained pointer is safe for its callbacks.
        let context = Unmanaged.passUnretained(self).toOpaque()
        for type in [kIOFirstMatchNotification, kIOTerminatedNotification] {
            var iterator: io_iterator_t = 0
            let result = IOServiceAddMatchingNotification(
                port, type, IOServiceMatching("IOUSBHostDevice"),
                { context, iterator in USBHotplugWatcher.fired(context, iterator) },
                context, &iterator)
            guard result == KERN_SUCCESS else { continue }
            // Draining the iterator arms the notification (the devices already
            // present are not news: the model scans once on its own).
            Self.drain(iterator)
            iterators.append(iterator)
        }
    }

    deinit {
        for iterator in iterators { IOObjectRelease(iterator) }
        if let port { IONotificationPortDestroy(port) }
    }

    private static func fired(_ context: UnsafeMutableRawPointer?, _ iterator: io_iterator_t) {
        drain(iterator)
        guard let context else { return }
        let onChange = Unmanaged<USBHotplugWatcher>.fromOpaque(context).takeUnretainedValue().onChange
        if Thread.isMainThread {
            MainActor.assumeIsolated { onChange() }
        } else {
            Task { @MainActor in onChange() }
        }
    }

    private static func drain(_ iterator: io_iterator_t) {
        while case let object = IOIteratorNext(iterator), object != 0 {
            IOObjectRelease(object)
        }
    }
}
