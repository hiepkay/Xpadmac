/*
 * XPadGIPDevice: selects the configuration of a GIP pad so that its
 * interfaces appear. See XPadGIPDevice.iig.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSCollections.h>
#include <DriverKit/OSDictionary.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "XPadGIPDevice.h"
#include "XPadCommon.h"

/* The GIP interface: vendor class 0xFF, subclass 0x47, protocol 0xD0. */
static constexpr uint8_t kGIPClass = 0xFF, kGIPSubClass = 0x47, kGIPProtocol = 0xD0;

static constexpr uint32_t kControlTimeoutMs = 500;

struct XPadGIPDevice_IVars {
    uint16_t vid, pid;
};

bool XPadGIPDevice::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(XPadGIPDevice_IVars, 1);
    return ivars != nullptr;
}

void XPadGIPDevice::free()
{
    IOSafeDeleteNULL(ivars, XPadGIPDevice_IVars, 1);
    super::free();
}

kern_return_t IMPL(XPadGIPDevice, Start)
{
    IOUSBHostDevice *device;
    kern_return_t ret;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP device: IOService::Start: 0x%x", ret);
        return ret;
    }

    device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device) {
        XPadLog("GIP device: provider is not an IOUSBHostDevice");
        ret = kIOReturnBadArgument;
        goto fail;
    }

    ret = device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP device: Open: 0x%x", ret);
        goto fail;
    }
    ret = configure(device);
    /* Only the configuration was ours to set: the interfaces are separate
     * services, so the device is not kept open. */
    device->Close(this, 0);
    if (ret != kIOReturnSuccess) goto fail;

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP device: RegisterService: 0x%x", ret);
        goto fail;
    }
    return kIOReturnSuccess;

fail:
    Stop(provider, SUPERDISPATCH);
    return ret;
}

kern_return_t IMPL(XPadGIPDevice, Stop)
{
    XPadLog("GIP device %04x:%04x: stop", ivars->vid, ivars->pid);
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t XPadGIPDevice::configure(IOUSBHostDevice *device)
{
    const IOUSBDeviceDescriptor *deviceDescriptor;
    const IOUSBConfigurationDescriptor *config;
    const IOUSBInterfaceDescriptor *interface = nullptr;
    bool gip = false, audio = false;
    uint8_t value;
    kern_return_t ret;

    deviceDescriptor = device->CopyDeviceDescriptor();
    if (!deviceDescriptor) {
        XPadLog("GIP device: no device descriptor");
        return kIOReturnNotReadable;
    }
    ivars->vid = USBToHost16(deviceDescriptor->idVendor);
    ivars->pid = USBToHost16(deviceDescriptor->idProduct);
    IOUSBHostFreeDescriptor(deviceDescriptor);

    /* The first configuration (index 0, not "the current one for client"). */
    config = device->CopyConfigurationDescriptor(static_cast<uint8_t>(0));
    if (!config) {
        XPadLog("GIP device %04x:%04x: no configuration descriptor", ivars->vid, ivars->pid);
        return kIOReturnNotReadable;
    }
    while ((interface = IOUSBGetNextInterfaceDescriptor(config,
                                                        reinterpret_cast<const IOUSBDescriptorHeader *>(interface)))) {
        if (interface->bInterfaceClass == kGIPClass && interface->bInterfaceSubClass == kGIPSubClass &&
            interface->bInterfaceProtocol == kGIPProtocol) {
            if (interface->bInterfaceNumber == 0) gip = true;
            /* GIP's audio interface: number 1, with an alternate setting 1. */
            if (interface->bInterfaceNumber == 1 && interface->bAlternateSetting == 1) audio = true;
        }
    }
    value = config->bConfigurationValue;
    IOUSBHostFreeDescriptor(config);

    if (!gip) {
        XPadLog("GIP device %04x:%04x: no FF/47/D0 interface, not a GIP pad: declined", ivars->vid, ivars->pid);
        return kIOReturnUnsupported;
    }

    if (windowsDescriptorsEnabled()) probeWindowsDescriptors(device);

    ret = device->SetConfiguration(value, true);
    XPadLog("GIP device %04x:%04x: SetConfiguration(%u): 0x%x", ivars->vid, ivars->pid, value, ret);
    if (ret != kIOReturnSuccess) return ret;

    /* The audio interface's SET_INTERFACE(1, 0), which some third-party
     * pads wait for (xone), is sent by XPadGIPAudio on that interface:
     * DeviceRequest refuses a standard SET_INTERFACE (kIOReturnNotPermitted). */
    (void)audio;
    return kIOReturnSuccess;
}

bool XPadGIPDevice::windowsDescriptorsEnabled()
{
    OSDictionary *properties = nullptr;
    bool enabled = false;

    if (CopyProperties(&properties) == kIOReturnSuccess && properties) {
        enabled = OSDictionaryGetValue(properties, "XPadWindowsDescriptors") == kOSBooleanTrue;
        properties->release();
    }
    return enabled;
}

void XPadGIPDevice::probeWindowsDescriptors(IOUSBHostDevice *device)
{
    /* "MSFT100" in UTF-16LE: the signature of a Microsoft OS 1.0 string descriptor. */
    static const uint8_t kSignature[14] = { 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0 };
    IOBufferMemoryDescriptor *buffer = nullptr;
    XPadMapping map = {};
    uint16_t got = 0;
    uint8_t vendorCode;
    uint32_t length;
    kern_return_t ret;

    ret = device->CreateIOBuffer(kIOMemoryDirectionIn, 256, &buffer);
    if (ret != kIOReturnSuccess || !buffer) {
        XPadLog("GIP device: OS descriptor buffer: 0x%x", ret);
        return;
    }
    buffer->SetLength(256);
    if (!XPadMappingGet(&map, buffer) || map.length < 64) goto out;

    /* GET_DESCRIPTOR(CONFIGURATION, 0) in one 255-byte request, as Windows
     * asks for it. Some third-party firmware picks GIP or a fallback HID
     * mode from how the host reads this descriptor (Linux has
     * USB_QUIRK_WINDOWS_CONFIG_REQ_SIZE for such pads); macOS's own
     * enumeration asks for 64 bytes. A pad that switches some time after
     * configuration may still count a Windows-shaped request sent now. */
    ret = device->DeviceRequest(this, kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeStandard |
                                          kIOUSBDeviceRequestRecipientDevice,
                                kIOUSBDeviceRequestGetDescriptor, (kIOUSBDescriptorTypeConfiguration << 8) | 0, 0, 255,
                                buffer, &got, kControlTimeoutMs);
    XPadLog("GIP device %04x:%04x: config descriptor, 255-byte request: 0x%x, %u bytes", ivars->vid, ivars->pid,
            ret, got);

    /* GET_DESCRIPTOR(STRING, index 0xEE), 18 bytes. */
    ret = device->DeviceRequest(this, kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeStandard |
                                          kIOUSBDeviceRequestRecipientDevice,
                                kIOUSBDeviceRequestGetDescriptor, (kIOUSBDescriptorTypeString << 8) | 0xEE, 0, 18,
                                buffer, &got, kControlTimeoutMs);
    if (ret != kIOReturnSuccess || got < 18 || memcmp(map.bytes + 2, kSignature, sizeof kSignature) != 0) {
        XPadLog("GIP device %04x:%04x: OS string descriptor 0xEE: 0x%x, %u bytes, no MSFT100",
                ivars->vid, ivars->pid, ret, got);
        goto out;
    }
    vendorCode = map.bytes[16];

    /* The extended compat ID descriptor (wIndex 4): its 16-byte header
     * first, then all of it (one function section is 40 bytes). */
    ret = device->DeviceRequest(this, kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeVendor |
                                          kIOUSBDeviceRequestRecipientDevice,
                                vendorCode, 0, 4, 16, buffer, &got, kControlTimeoutMs);
    length = got >= 4 ? (uint32_t)(map.bytes[0] | map.bytes[1] << 8 | map.bytes[2] << 16 | (uint32_t)map.bytes[3] << 24)
                      : 0;
    if (ret == kIOReturnSuccess && length > 16 && length <= 64) {
        ret = device->DeviceRequest(this, kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeVendor |
                                              kIOUSBDeviceRequestRecipientDevice,
                                    vendorCode, 0, 4, (uint16_t)length, buffer, &got, kControlTimeoutMs);
    }
    if (ret == kIOReturnSuccess && got >= 26) {
        char compat[9];
        memcpy(compat, map.bytes + 18, 8);
        compat[8] = '\0';
        XPadLog("GIP device %04x:%04x: vendor code 0x%02x, compat ID \"%{public}s\"", ivars->vid, ivars->pid,
                vendorCode, compat);
    } else {
        XPadLog("GIP device %04x:%04x: vendor code 0x%02x, compat ID request: 0x%x, %u bytes", ivars->vid,
                ivars->pid, vendorCode, ret, got);
    }

out:
    XPadMappingReset(&map);
    OSSafeReleaseNULL(buffer);
}
