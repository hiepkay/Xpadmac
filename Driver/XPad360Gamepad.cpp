/*
 * XPad360Gamepad: Xbox 360 wired (XInput) pad on interface 0 (FF/5D/01),
 * published as an Xbox Wireless Controller. See XPad360Gamepad.iig.
 *
 * Every method here runs on the service's default dispatch queue (the
 * pipe completions and the timer are delivered there), so the ivars need
 * no locking, and nothing here blocks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSCollections.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "XPad360Gamepad.h"
#include "XPadCommon.h"

static constexpr uint32_t kReadCount = 2;           /* interrupt-IN transfers always queued */
static constexpr uint32_t kReadSize = 32;           /* or the endpoint's max packet, if larger */
static constexpr uint32_t kReadSizeMax = 1024;
static constexpr uint32_t kMessageMax = 32;         /* the OUT endpoint's packet; ours are 3 and 8 bytes */
static constexpr uint32_t kQueueDepth = 8;          /* messages waiting for the OUT endpoint */
static constexpr uint64_t kTickNs = 20 * 1000000ull;
static constexpr uint64_t kTickLeewayNs = 5 * 1000000ull;
static constexpr uint32_t kMaxErrorsInRow = 8;     /* as XPadGIPGamepad: failed completions in a row on
                                                       one pipe before it gives up */
static constexpr uint64_t kLogErrors = 10;          /* failures of each kind logged, then only counted */
static constexpr uint32_t kDefaultIntervalUs = 4000;
/* The vendor GET Linux's xpad (xpad_start_input) and SDL (init_xbox360)
 * send every wired 360 pad: some third-party ones report nothing until
 * they got it. Windows' XUSB driver sends it too. */
static constexpr uint8_t  kInitRequest = 0x01;
static constexpr uint16_t kInitValue = 0x0100;
static constexpr uint16_t kInitLength = 20;
static constexpr uint32_t kInitTimeoutMs = 100;

struct XPad360Message {
    uint8_t length;
    uint8_t bytes[kMessageMax];
};

struct XPad360Gamepad_IVars {
    IOUSBHostInterface       *interface;
    bool                      interfaceOpen;
    IOUSBHostPipe            *inPipe, *outPipe;
    uint32_t                  readSize;
    uint8_t                   interfaceNumber;

    IOBufferMemoryDescriptor *readBuffer[kReadCount];
    XPadMapping               readMap[kReadCount];
    OSAction                 *readAction[kReadCount];
    uint32_t                  readsQueued;
    uint32_t                  readErrors;           /* failed completions in a row, both reads together */

    IOBufferMemoryDescriptor *writeBuffer;
    uint8_t                  *writeBytes;           /* writeBuffer's bytes (the dext's own allocation) */
    OSAction                 *writeAction;
    bool                      writeBusy;
    uint32_t                  writeErrorsInRow;
    XPad360Message            fifo[kQueueDepth];
    uint32_t                  fifoHead, fifoCount;

    /* Rumble is not queued: the latest request replaces an unsent one. */
    bool                      rumblePending;
    xpad_rumble               rumble;
    /* XPadNowMs() at which the motors are stopped unless another report
     * came: the 360 message holds its level until the next one, a real
     * Xbox Wireless Controller stops after the report's own play time.
     * 0 = nothing to stop. */
    long                      rumbleUntil;
    uint64_t                  rumbleExpired;
    /* The rumble message in flight (or just tried) failed: the next tick
     * sends the latest request again, unless a newer one is pending. */
    bool                      writeIsRumble;
    bool                      rumbleResend;
    uint32_t                  rumbleResends;

    IODispatchQueue          *queue;
    IOTimerDispatchSource    *timer;
    OSAction                 *timerAction;
    mach_timebase_info_data_t timebase;             /* ns <-> mach_absolute_time ticks */

    IOBufferMemoryDescriptor *xboxReport;           /* input report 1, 17 bytes */
    uint8_t                  *xboxBytes;

    xpad_state                state;                /* the last input message */
    bool                      sawInput;
    bool                      stopping, tornDown;

    uint16_t                  vid, pid;
    uint64_t                  locationID;
    uint32_t                  reportIntervalUs;
    char                      name[16];             /* "vvvv:pppp" */
    char                      serial[64];

    uint64_t                  readFailures, writeErrors, messagesDropped, reportErrors, otherMessages;
};

bool XPad360Gamepad::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(XPad360Gamepad_IVars, 1);
    return ivars != nullptr;
}

void XPad360Gamepad::free()
{
    if (ivars) {
        for (uint32_t i = 0; i < kReadCount; i++) {
            XPadMappingReset(&ivars->readMap[i]);
            OSSafeReleaseNULL(ivars->readBuffer[i]);
            OSSafeReleaseNULL(ivars->readAction[i]);
        }
        OSSafeReleaseNULL(ivars->writeBuffer);
        OSSafeReleaseNULL(ivars->writeAction);
        OSSafeReleaseNULL(ivars->timer);
        OSSafeReleaseNULL(ivars->timerAction);
        OSSafeReleaseNULL(ivars->queue);
        OSSafeReleaseNULL(ivars->xboxReport);
        OSSafeReleaseNULL(ivars->inPipe);
        OSSafeReleaseNULL(ivars->outPipe);
        OSSafeReleaseNULL(ivars->interface);
        IOSafeDeleteNULL(ivars, XPad360Gamepad_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(XPad360Gamepad, Start)
{
    /* IOUserHIDDevice::Start calls handleStart, then publishes the HID
     * device with newDeviceDescription and newReportDescriptor. */
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: Start failed: 0x%x", ivars->name, ret);
        teardown();
        return ret;
    }
    /* As in XPadGIPGamepad: without it the kernel's AppleUserHIDDevice
     * stays unregistered and nothing in user space sees the pad. */
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: RegisterService: 0x%x", ivars->name, ret);
        teardown();
        return ret;
    }
    XPadLog("X360 %{public}s: published as Xbox Wireless Controller 045E:0B13", ivars->name);
    return kIOReturnSuccess;
}

kern_return_t IMPL(XPad360Gamepad, Stop)
{
    teardown();
    return Stop(provider, SUPERDISPATCH);
}

/* Quiesces everything; called once, from Stop or from a failed Start (at
 * any point of it: every step checks what exists). */
void XPad360Gamepad::teardown()
{
    if (ivars->tornDown) return;
    ivars->tornDown = true;
    ivars->stopping = true;

    XPadCancelTimer(&ivars->timer);
    /* The only place the transfers are aborted: never while the pad is in use. */
    if (ivars->inPipe) ivars->inPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
    if (ivars->outPipe) ivars->outPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
    if (ivars->interfaceOpen) {
        ivars->interface->Close(this, 0);
        ivars->interfaceOpen = false;
    }
    /* An action keeps its target (this object) alive until it is cancelled. */
    for (uint32_t i = 0; i < kReadCount; i++) {
        XPadCancelAction(&ivars->readAction[i]);
    }
    XPadCancelAction(&ivars->writeAction);
    XPadCancelAction(&ivars->timerAction);
}

bool XPad360Gamepad::handleStart(IOService *provider)
{
    uint8_t led[XPAD_X360_LED_LEN];
    kern_return_t ret;

    if (!super::handleStart(provider)) return false;
    if (!openInterface(provider) || !createIO()) return false;
    xpad_state_reset(&ivars->state);

    for (uint32_t i = 0; i < kReadCount; i++) {
        ret = submitRead(i);
        if (ret == kIOReturnSuccess) {
            ivars->readsQueued++;
        } else {
            XPadLog("X360 %{public}s: read %u: AsyncIO: 0x%x", ivars->name, i, ret);
        }
    }
    if (!ivars->readsQueued) return false;

    /* Once the reads are queued, as Linux does: failure is logged only. */
    sendInitRequest();

    /* Player 1's LED, as Linux's xpad sends at probe: some third-party pads
     * report nothing until they got it (or any OUT transfer). */
    if (xpad_x360_build_led(XPAD_X360_LED_PLAYER1_FLASH, led, sizeof led) == sizeof led)
        (void)sendMessage(led, sizeof led);
    armTimer();

    XPadLog("X360 %{public}s at 0x%08llx: %u reads of %u bytes queued, LED sent", ivars->name,
            (unsigned long long)ivars->locationID, ivars->readsQueued, ivars->readSize);
    return true;
}

/* GET (vendor, interface) bRequest 0x01, wValue 0x0100, 20 bytes; the
 * reply is not used. Synchronous, bounded by its timeout, and never fatal
 * (Linux only warns). */
void XPad360Gamepad::sendInitRequest()
{
    IOBufferMemoryDescriptor *buffer = nullptr;
    uint16_t got = 0;
    kern_return_t ret;

    ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionIn, kInitLength, &buffer);
    if (ret == kIOReturnSuccess && buffer) ret = buffer->SetLength(kInitLength);
    if (ret == kIOReturnSuccess && buffer) {
        ret = ivars->interface->DeviceRequest(kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeVendor |
                                                  kIOUSBDeviceRequestRecipientInterface,
                                              kInitRequest, kInitValue, ivars->interfaceNumber, kInitLength, buffer,
                                              &got, kInitTimeoutMs);
        XPadLog("X360 %{public}s: init request (C1 01 0100): 0x%x, %u bytes", ivars->name, ret, got);
    } else {
        XPadLog("X360 %{public}s: init request buffer: 0x%x", ivars->name, ret);
    }
    OSSafeReleaseNULL(buffer);
}

bool XPad360Gamepad::openInterface(IOService *provider)
{
    IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, provider);
    IOUSBHostDevice *device = nullptr;
    const IOUSBConfigurationDescriptor *config;
    const IOUSBInterfaceDescriptor *descriptor;
    const IOUSBEndpointDescriptor *endpoint = nullptr;
    uint8_t speed = 0, inAddress = 0, outAddress = 0;
    uint16_t inMaxPacket = 0;
    kern_return_t ret;

    if (!interface) {
        XPadLog("X360: provider is not an IOUSBHostInterface");
        return false;
    }
    interface->retain();
    ivars->interface = interface;

    /* Who the pad is: VID/PID, serial number and location of its device. */
    if (interface->CopyDevice(&device) == kIOReturnSuccess && device) {
        const IOUSBDeviceDescriptor *deviceDescriptor = device->CopyDeviceDescriptor();
        OSDictionary *properties = nullptr;

        if (deviceDescriptor) {
            ivars->vid = USBToHost16(deviceDescriptor->idVendor);
            ivars->pid = USBToHost16(deviceDescriptor->idProduct);
            IOUSBHostFreeDescriptor(deviceDescriptor);
        }
        device->GetSpeed(&speed);
        if (device->CopyProperties(&properties) == kIOReturnSuccess && properties) {
            const char *serial = OSDictionaryGetStringValue(properties, "kUSBSerialNumberString");
            if (serial) snprintf(ivars->serial, sizeof ivars->serial, "%s", serial);
            ivars->locationID = OSDictionaryGetUInt64Value(properties, "locationID");
            properties->release();
        }
        device->release();
    }
    snprintf(ivars->name, sizeof ivars->name, "%04x:%04x", ivars->vid, ivars->pid);

    ret = interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: interface Open: 0x%x", ivars->name, ret);
        return false;
    }
    ivars->interfaceOpen = true;

    /* The interrupt endpoints: IN for the pad's messages, OUT for ours (the
     * XInput class descriptor 0x21 between them is skipped by the walk). */
    config = interface->CopyConfigurationDescriptor();
    descriptor = config ? interface->GetInterfaceDescriptor(config) : nullptr;
    if (descriptor) {
        ivars->interfaceNumber = descriptor->bInterfaceNumber;
        while ((endpoint = IOUSBGetNextEndpointDescriptor(config, descriptor,
                                                          reinterpret_cast<const IOUSBDescriptorHeader *>(endpoint)))) {
            if (IOUSBGetEndpointType(endpoint) != kIOUSBEndpointTypeInterrupt) continue;
            if (IOUSBGetEndpointDirection(endpoint) == kIOUSBEndpointDirectionIn) {
                if (inAddress) continue;
                inAddress = IOUSBGetEndpointAddress(endpoint);
                inMaxPacket = IOUSBGetEndpointMaxPacketSize(speed, endpoint);
                ivars->reportIntervalUs = IOUSBGetEndpointIntervalMicroframes(speed, endpoint) * 125;
            } else if (!outAddress) {
                outAddress = IOUSBGetEndpointAddress(endpoint);
            }
        }
    }
    if (config) IOUSBHostFreeDescriptor(config);
    if (!inAddress || !outAddress) {
        XPadLog("X360 %{public}s: no interrupt IN/OUT endpoint pair (in 0x%02x, out 0x%02x)", ivars->name, inAddress,
                outAddress);
        return false;
    }

    ret = interface->CopyPipe(inAddress, &ivars->inPipe);
    if (ret == kIOReturnSuccess) ret = interface->CopyPipe(outAddress, &ivars->outPipe);
    if (ret != kIOReturnSuccess || !ivars->inPipe || !ivars->outPipe) {
        XPadLog("X360 %{public}s: CopyPipe(0x%02x/0x%02x): 0x%x", ivars->name, inAddress, outAddress, ret);
        return false;
    }

    ivars->readSize = inMaxPacket > kReadSize ? inMaxPacket : kReadSize;
    if (ivars->readSize > kReadSizeMax) ivars->readSize = kReadSizeMax;
    return true;
}

bool XPad360Gamepad::createIO()
{
    xpad_state idle;
    kern_return_t ret;

    /* IN buffers from the interface, read through their mapping (as in
     * XPadGIPGamepad: they read fine). */
    for (uint32_t i = 0; i < kReadCount; i++) {
        ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionIn, ivars->readSize, &ivars->readBuffer[i]);
        if (ret == kIOReturnSuccess && ivars->readBuffer[i]) ret = ivars->readBuffer[i]->SetLength(ivars->readSize);
        if (ret == kIOReturnSuccess && !XPadMappingGet(&ivars->readMap[i], ivars->readBuffer[i]))
            ret = kIOReturnNoMemory;
        if (ret == kIOReturnSuccess) ret = CreateActionReadComplete(sizeof(uint32_t), &ivars->readAction[i]);
        if (ret != kIOReturnSuccess) {
            XPadLog("X360 %{public}s: read %u setup: 0x%x", ivars->name, i, ret);
            return false;
        }
        /* The completion finds its buffer by this index. */
        *static_cast<uint32_t *>(ivars->readAction[i]->GetReference()) = i;
    }

    /* The OUT buffer is the dext's own (XPadGIPGamepad.cpp says why: the
     * interface's CreateIOBuffer(Out) gave an address not mapped here). */
    ret = XPadCreateBuffer(kIOMemoryDirectionOut, kMessageMax, &ivars->writeBuffer, &ivars->writeBytes);
    if (ret == kIOReturnSuccess) ret = CreateActionWriteComplete(0, &ivars->writeAction);
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: write setup: 0x%x", ivars->name, ret);
        return false;
    }

    ret = XPadCreateBuffer(kIOMemoryDirectionInOut, XPAD_XBOX_INPUT_LEN, &ivars->xboxReport, &ivars->xboxBytes);
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: input report buffer: 0x%x", ivars->name, ret);
        return false;
    }
    xpad_state_reset(&idle);
    xpad_xbox_build_input(&idle, ivars->xboxBytes, XPAD_XBOX_INPUT_LEN);

    ret = mach_timebase_info(&ivars->timebase);
    if (ret == kIOReturnSuccess && !ivars->timebase.numer) ret = kIOReturnError;
    if (ret == kIOReturnSuccess) ret = CopyDispatchQueue(kIOServiceDefaultQueueName, &ivars->queue);
    if (ret == kIOReturnSuccess) ret = IOTimerDispatchSource::Create(ivars->queue, &ivars->timer);
    if (ret == kIOReturnSuccess) ret = CreateActionTimerFired(0, &ivars->timerAction);
    if (ret == kIOReturnSuccess) ret = ivars->timer->SetHandler(ivars->timerAction);
    if (ret != kIOReturnSuccess) {
        XPadLog("X360 %{public}s: timer setup: 0x%x", ivars->name, ret);
        return false;
    }
    return true;
}

kern_return_t XPad360Gamepad::submitRead(uint32_t index)
{
    /* No timeout (interrupt endpoints take none anyway), and never cancelled. */
    return ivars->inPipe->AsyncIO(ivars->readBuffer[index], ivars->readSize, ivars->readAction[index], 0);
}

void IMPL(XPad360Gamepad, ReadComplete)
{
    uint32_t *reference = action ? static_cast<uint32_t *>(action->GetReference()) : nullptr;
    uint32_t index = reference ? *reference : kReadCount;
    kern_return_t ret;

    if (index >= kReadCount) return;
    if (ivars->stopping) {
        ivars->readsQueued--;
        return;
    }

    switch (status) {
    case kIOReturnSuccess: {
        const uint8_t *bytes = ivars->readMap[index].bytes;
        uint64_t length = actualByteCount < ivars->readMap[index].length ? actualByteCount
                                                                         : ivars->readMap[index].length;

        ivars->readErrors = 0;
        if (xpad_x360_parse(bytes, (size_t)length, &ivars->state)) {
            if (!ivars->sawInput) {
                ivars->sawInput = true;
                XPadLog("X360 %{public}s: first input message (%llu bytes)", ivars->name,
                        (unsigned long long)length);
            }
            publish(completionTimestamp);
        } else if (length && ivars->otherMessages++ < kLogErrors) {
            /* LED state (01 03), rumble state (03 03), headset (08 03)...: nothing to publish. */
            XPadLog("X360 %{public}s: message %02x, %llu bytes, ignored", ivars->name, bytes[0],
                    (unsigned long long)length);
        }
        break;
    }
    case kIOReturnNoDevice:
        /* The pad is gone: Stop follows. */
        ivars->readsQueued--;
        XPadLog("X360 %{public}s: read %u ended: 0x%x (%u still queued)", ivars->name, index, status,
                ivars->readsQueued);
        return;
    case kIOReturnAborted:
        /* Not a teardown (that sets stopping first). Our own ClearStall
         * (below) aborts every read queued on the endpoint, so this is most
         * likely the other read: read again, without a clear (an abort
         * leaves no halt), counted so that a pipe that only aborts cannot
         * spin. */
        if (++ivars->readErrors > kMaxErrorsInRow) {
            ivars->readsQueued--;
            XPadLog("X360 %{public}s: read %u aborted, %u failures in a row: it stops (%u still queued)",
                    ivars->name, index, ivars->readErrors, ivars->readsQueued);
            return;
        }
        break;
    default:
        /* Any error but a timeout leaves an interrupt endpoint halted: clear
         * it and read on, a bounded number of times in a row (see
         * XPadGIPGamepad::ReadComplete). A working pipe is never touched. */
        if (ivars->readFailures++ < kLogErrors)
            XPadLog("X360 %{public}s: read %u failed: 0x%x", ivars->name, index, status);
        if (++ivars->readErrors > kMaxErrorsInRow) {
            ivars->readsQueued--;
            XPadLog("X360 %{public}s: %u reads failed in a row, read %u stops (%u still queued)", ivars->name,
                    ivars->readErrors, index, ivars->readsQueued);
            return;
        }
        ret = ivars->inPipe->ClearStall(true);
        if (ret != kIOReturnSuccess) XPadLog("X360 %{public}s: IN ClearStall: 0x%x", ivars->name, ret);
        break;
    }

    ret = submitRead(index);
    if (ret != kIOReturnSuccess) {
        ivars->readsQueued--;
        XPadLog("X360 %{public}s: read %u: AsyncIO: 0x%x (%u still queued)", ivars->name, index, ret,
                ivars->readsQueued);
    }
}

bool XPad360Gamepad::sendMessage(const uint8_t *message, uint32_t length)
{
    XPad360Message *slot;

    if (ivars->stopping || !message || !length || length > kMessageMax) return false;
    if (ivars->fifoCount == kQueueDepth) {
        if (ivars->messagesDropped++ < kLogErrors)
            XPadLog("X360 %{public}s: OUT queue full, message %02x dropped", ivars->name, message[0]);
        return false;
    }
    slot = &ivars->fifo[(ivars->fifoHead + ivars->fifoCount) % kQueueDepth];
    memcpy(slot->bytes, message, length);
    slot->length = (uint8_t)length;
    ivars->fifoCount++;
    startWrite();
    return true;
}

/* Starts the next OUT transfer if none is in flight: queued messages
 * first, then a pending rumble. */
void XPad360Gamepad::startWrite()
{
    while (!ivars->writeBusy && !ivars->stopping && ivars->writeBytes) {
        uint32_t length;
        kern_return_t ret;

        if (ivars->fifoCount) {
            const XPad360Message *message = &ivars->fifo[ivars->fifoHead];
            memcpy(ivars->writeBytes, message->bytes, message->length);
            length = message->length;
            ivars->writeIsRumble = false;
            ivars->fifoHead = (ivars->fifoHead + 1) % kQueueDepth;
            ivars->fifoCount--;
        } else if (ivars->rumblePending) {
            size_t n = xpad_x360_build_rumble(&ivars->rumble, ivars->writeBytes, kMessageMax);

            ivars->rumblePending = false;
            if (!n) break;
            length = (uint32_t)n;
            ivars->writeIsRumble = true;
        } else {
            break;
        }

        ret = ivars->outPipe->AsyncIO(ivars->writeBuffer, length, ivars->writeAction, 0);
        if (ret == kIOReturnSuccess) {
            ivars->writeBusy = true;
        } else {
            /* That message is lost; the loop moves on to the next (a rumble
             * is sent again from the timer, not from this loop). */
            if (ivars->writeIsRumble) ivars->rumbleResend = true;
            if (ivars->writeErrors++ < kLogErrors)
                XPadLog("X360 %{public}s: OUT AsyncIO (message %02x): 0x%x", ivars->name, ivars->writeBytes[0], ret);
        }
    }
}

void IMPL(XPad360Gamepad, WriteComplete)
{
    (void)action;
    (void)actualByteCount;
    (void)completionTimestamp;

    ivars->writeBusy = false;
    if (ivars->stopping) return;
    if (ivars->writeIsRumble) {
        if (status == kIOReturnSuccess) ivars->rumbleResends = 0;
        else ivars->rumbleResend = true;
    }
    switch (status) {
    case kIOReturnSuccess:
        ivars->writeErrorsInRow = 0;
        break;
    case kIOReturnNoDevice:
    case kIOReturnAborted:
        /* The pad is gone, or the interface is going away; that message is
         * lost, and an abort leaves no halt. */
        if (ivars->writeErrors++ < kLogErrors)
            XPadLog("X360 %{public}s: OUT transfer (message %02x): 0x%x", ivars->name, ivars->writeBytes[0], status);
        break;
    default: {
        kern_return_t ret = kIOReturnSuccess;

        /* The OUT endpoint is halted now; with one transfer in flight at
         * most, clearing it aborts nothing else. Bounded, as the reads. */
        if (ivars->writeErrors++ < kLogErrors)
            XPadLog("X360 %{public}s: OUT transfer (message %02x): 0x%x", ivars->name, ivars->writeBytes[0], status);
        if (++ivars->writeErrorsInRow <= kMaxErrorsInRow) ret = ivars->outPipe->ClearStall(true);
        if (ret != kIOReturnSuccess) XPadLog("X360 %{public}s: OUT ClearStall: 0x%x", ivars->name, ret);
        break;
    }
    }
    startWrite();
}

/* No report since the last one's play time (plus slack) ran out: the
 * client stopped asking without a stop (it died mid-haptic, say). Stop
 * the motors as a real pad would have by now. */
void XPad360Gamepad::expireRumble(long now)
{
    if (!ivars->rumbleUntil || now < ivars->rumbleUntil) return;
    ivars->rumbleUntil = 0;
    ivars->rumble = xpad_rumble{};
    ivars->rumblePending = true;
    ivars->rumbleResends = 0;
    if (ivars->rumbleExpired++ < kLogErrors)
        XPadLog("X360 %{public}s: rumble expired with no stop: motors stopped", ivars->name);
}

void IMPL(XPad360Gamepad, TimerFired)
{
    (void)action;
    (void)time;

    if (ivars->stopping) return;
    expireRumble(XPadNowMs());
    if (ivars->rumbleResend) {
        ivars->rumbleResend = false;
        if (!ivars->rumblePending && ivars->rumbleResends < kMaxErrorsInRow) {
            ivars->rumbleResends++;
            ivars->rumblePending = true;            /* the latest request: what failed, or newer */
        }
    }
    startWrite();
    armTimer();
}

/* The deadline is in mach_absolute_time ticks (see XPadGIPGamepad::armTimer). */
void XPad360Gamepad::armTimer()
{
    uint64_t tick, leeway;

    if (!ivars->timer || ivars->stopping || !ivars->timebase.numer) return;
    tick = kTickNs * ivars->timebase.denom / ivars->timebase.numer;
    leeway = kTickLeewayNs * ivars->timebase.denom / ivars->timebase.numer;
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + tick, leeway);
}

void XPad360Gamepad::publish(uint64_t timestamp)
{
    kern_return_t ret;

    if (!ivars->xboxBytes) return;
    if (xpad_xbox_build_input(&ivars->state, ivars->xboxBytes, XPAD_XBOX_INPUT_LEN) != XPAD_XBOX_INPUT_LEN) return;
    if (!timestamp) timestamp = mach_absolute_time();
    ret = handleReport(timestamp, ivars->xboxReport, XPAD_XBOX_INPUT_LEN, kIOHIDReportTypeInput, 0);
    if (ret != kIOReturnSuccess && ivars->reportErrors++ < kLogErrors)
        XPadLog("X360 %{public}s: handleReport: 0x%x", ivars->name, ret);
}

OSDictionary *XPad360Gamepad::newDeviceDescription()
{
    char source[24];
    XPadIdentity identity = {};
    OSDictionary *description;

    snprintf(source, sizeof source, "X360 %04x:%04x", ivars->vid, ivars->pid);
    identity.source = source;
    identity.locationID = ivars->locationID;
    identity.serial = ivars->serial[0] ? ivars->serial : nullptr;
    identity.reportIntervalUs = ivars->reportIntervalUs ? ivars->reportIntervalUs : kDefaultIntervalUs;
    description = XPadCreateXboxDescription(nullptr, &identity);
    if (!description) XPadLog("X360 %{public}s: no device description", ivars->name);
    return description;
}

OSData *XPad360Gamepad::newReportDescriptor()
{
    return OSData::withBytes(xpad_xbox_desc, XPAD_XBOX_DESC_LEN);
}

kern_return_t XPad360Gamepad::setReport(IOMemoryDescriptor *report,
                                        IOHIDReportType reportType,
                                        IOOptionBits options,
                                        uint32_t completionTimeout,
                                        OSAction *action)
{
    xpad_rumble rumble;
    uint32_t count = 0, playMs = 0;
    kern_return_t ret;

    (void)completionTimeout;
    if (!XPadParseRumble(report, reportType, options, &rumble, &count, &playMs)) {
        ret = kIOReturnUnsupported;             /* the descriptor has no other output report */
    } else if (ivars->stopping) {
        ret = kIOReturnOffline;
    } else {
        /* The 360 pad has no trigger motors: only strong and weak count. */
        bool on = (rumble.strong | rumble.weak) != 0;
        long now = XPadNowMs();

        ivars->rumble = rumble;
        ivars->rumblePending = true;            /* the latest request replaces one not sent yet */
        ivars->rumbleResends = 0;
        /* A start (re)sets the deadline (never 0: the slack is not), a stop clears it. */
        ivars->rumbleUntil = on ? now + (long)playMs + kXPadRumbleSlackMs : 0;
        startWrite();
        ret = kIOReturnSuccess;
    }
    /* Completed here, success or not: the HIDDriverKit dispatcher does not
     * complete the action of a failed setReport. */
    if (action) CompleteReport(action, ret, ret == kIOReturnSuccess ? count : 0);
    return ret;
}

kern_return_t XPad360Gamepad::getReport(IOMemoryDescriptor *report,
                                        IOHIDReportType reportType,
                                        IOOptionBits options,
                                        uint32_t completionTimeout,
                                        OSAction *action)
{
    uint32_t count = 0;
    kern_return_t ret;

    (void)completionTimeout;
    ret = XPadServeInputReport(report, reportType, options, ivars->xboxBytes, &count);
    if (action) CompleteReport(action, ret, count);
    return ret;
}
