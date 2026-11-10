/*
 * XPadGIPGamepad: GIP host on interface 0 of an Xbox One / Series pad,
 * published as an Xbox Wireless Controller. See XPadGIPGamepad.iig.
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

#include "XPadGIPGamepad.h"
#include "XPadCommon.h"
#include "gip.h"

static constexpr uint32_t kReadCount = 2;           /* interrupt-IN transfers always queued */
static constexpr uint32_t kReadSize = 64;           /* or the endpoint's max packet, if larger */
static constexpr uint32_t kReadSizeMax = 1024;
static constexpr uint32_t kFrameMax = 64;           /* the largest frame queued for the pad */
static constexpr uint32_t kQueueDepth = 16;         /* frames waiting for the OUT endpoint */
static constexpr uint64_t kTickNs = 20 * 1000000ull;            /* gip_tick period */
static constexpr uint64_t kTickLeewayNs = 5 * 1000000ull;
static constexpr long     kRumbleSpacingMs = 10;    /* gip.c's T_RUMBLE: some firmwares choke on more */
static constexpr uint32_t kMaxErrorsInRow = 8;     /* failed completions in a row on one pipe (errors
                                                       followed by a halt clear, and the aborts those
                                                       clears cause) before it gives up */
static constexpr uint64_t kLogErrors = 10;          /* failures of each kind logged, then only counted */
static constexpr uint32_t kDefaultIntervalUs = 4000;

struct XPadOutFrame {
    uint8_t length;
    uint8_t bytes[kFrameMax];
};

struct XPadGIPGamepad_IVars {
    IOUSBHostInterface       *interface;
    bool                      interfaceOpen;
    IOUSBHostPipe            *inPipe, *outPipe;
    uint32_t                  readSize;

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
    XPadOutFrame              fifo[kQueueDepth];
    uint32_t                  fifoHead, fifoCount;

    /* Rumble is not queued: the latest request replaces an unsent one. */
    bool                      rumblePending;
    xpad_rumble               rumble;
    long                      rumbleSentAt;
    /* XPadNowMs() at which the motors are stopped unless another report
     * came: the GIP frame holds its level for minutes, a real pad stops
     * after the report's own play time. 0 = nothing to stop. */
    long                      rumbleUntil;
    uint64_t                  rumbleExpired;
    /* The rumble frame in flight (or just tried) failed: the next tick
     * sends the latest request again, unless a newer one is pending. A
     * lost stop would otherwise leave the pad rumbling for minutes. */
    bool                      writeIsRumble;
    bool                      rumbleResend;
    uint32_t                  rumbleResends;

    IODispatchQueue          *queue;
    IOTimerDispatchSource    *timer;
    OSAction                 *timerAction;
    mach_timebase_info_data_t timebase;             /* ns <-> mach_absolute_time ticks */

    IOBufferMemoryDescriptor *xboxReport;           /* input report 1, 17 bytes */
    uint8_t                  *xboxBytes;

    gip_t                     gip;
    xpad_state                state;
    bool                      gipStarted;
    bool                      stopping, tornDown;

    uint16_t                  vid, pid;
    uint64_t                  locationID;
    uint32_t                  reportIntervalUs;
    char                      name[16];             /* "vvvv:pppp": gip.c logs with it */
    char                      serial[64];

    uint64_t                  readFailures, writeErrors, framesDropped, reportErrors;
};

/* gip_send_fn: gip.c hands its frames to the driver through this. */
static int XPadGIPSend(void *context, const uint8_t *frame, int length)
{
    return static_cast<XPadGIPGamepad *>(context)->sendFrame(frame, length);
}

bool XPadGIPGamepad::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(XPadGIPGamepad_IVars, 1);
    return ivars != nullptr;
}

void XPadGIPGamepad::free()
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
        IOSafeDeleteNULL(ivars, XPadGIPGamepad_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(XPadGIPGamepad, Start)
{
    /* IOUserHIDDevice::Start calls handleStart, then publishes the HID
     * device with newDeviceDescription and newReportDescriptor. */
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP %{public}s: Start failed: 0x%x", ivars->name, ret);
        teardown();
        return ret;
    }
    /* IOUserHIDDevice leaves this to the concrete class (its Start
     * documentation): without it the kernel's AppleUserHIDDevice stays
     * unregistered, gets no IOHIDInterface, and nothing in user space
     * (IOHIDManager, GameController) ever sees the pad. */
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP %{public}s: RegisterService: 0x%x", ivars->name, ret);
        teardown();
        return ret;
    }
    XPadLog("GIP %{public}s: published as Xbox Wireless Controller 045E:0B13", ivars->name);
    return kIOReturnSuccess;
}

kern_return_t IMPL(XPadGIPGamepad, Stop)
{
    if (ivars->gipStarted) gip_lost(&ivars->gip, XPadNowMs());
    teardown();
    return Stop(provider, SUPERDISPATCH);
}

/* Quiesces everything; called once, from Stop or from a failed Start. */
void XPadGIPGamepad::teardown()
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

bool XPadGIPGamepad::handleStart(IOService *provider)
{
    kern_return_t ret;

    if (!super::handleStart(provider)) return false;
    if (!openInterface(provider) || !createIO()) return false;

    /* Vendor quirk (VID 0x3537): also xone's serial-number request (1E)
     * after the Host Hello; some of this vendor's pads leave GIP shortly
     * after they report ACTIVE unless the host asks for it, as xone does. */
    gip_init(&ivars->gip, ivars->name, ivars->vid, ivars->pid, ivars->vid == 0x3537 ? GIP_OPT_SERIAL : 0,
             XPadNowMs());
    xpad_state_reset(&ivars->state);
    ivars->gipStarted = true;

    for (uint32_t i = 0; i < kReadCount; i++) {
        ret = submitRead(i);
        if (ret == kIOReturnSuccess) {
            ivars->readsQueued++;
        } else {
            XPadLog("GIP %{public}s: read %u: AsyncIO: 0x%x", ivars->name, i, ret);
        }
    }
    if (!ivars->readsQueued) return false;
    ivars->gip.reads = 1;           /* gip.c writes nothing before a read is queued */
    armTimer();

    XPadLog("GIP %{public}s at 0x%08llx: %u reads of %u bytes queued, waiting for the pad's hello", ivars->name,
            (unsigned long long)ivars->locationID, ivars->readsQueued, ivars->readSize);
    return true;
}

bool XPadGIPGamepad::openInterface(IOService *provider)
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
        XPadLog("GIP: provider is not an IOUSBHostInterface");
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
        XPadLog("GIP %{public}s: interface Open: 0x%x", ivars->name, ret);
        return false;
    }
    ivars->interfaceOpen = true;

    /* The interrupt endpoints: IN for the pad's frames, OUT for ours. */
    config = interface->CopyConfigurationDescriptor();
    descriptor = config ? interface->GetInterfaceDescriptor(config) : nullptr;
    if (descriptor) {
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
        XPadLog("GIP %{public}s: no interrupt IN/OUT endpoint pair (in 0x%02x, out 0x%02x)", ivars->name, inAddress,
                outAddress);
        return false;
    }

    ret = interface->CopyPipe(inAddress, &ivars->inPipe);
    if (ret == kIOReturnSuccess) ret = interface->CopyPipe(outAddress, &ivars->outPipe);
    if (ret != kIOReturnSuccess || !ivars->inPipe || !ivars->outPipe) {
        XPadLog("GIP %{public}s: CopyPipe(0x%02x/0x%02x): 0x%x", ivars->name, inAddress, outAddress, ret);
        return false;
    }

    ivars->readSize = inMaxPacket > kReadSize ? inMaxPacket : kReadSize;
    if (ivars->readSize > kReadSizeMax) ivars->readSize = kReadSizeMax;
    return true;
}

bool XPadGIPGamepad::createIO()
{
    xpad_state idle;
    kern_return_t ret;

    for (uint32_t i = 0; i < kReadCount; i++) {
        ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionIn, ivars->readSize, &ivars->readBuffer[i]);
        if (ret == kIOReturnSuccess && ivars->readBuffer[i]) ret = ivars->readBuffer[i]->SetLength(ivars->readSize);
        if (ret == kIOReturnSuccess && !XPadMappingGet(&ivars->readMap[i], ivars->readBuffer[i]))
            ret = kIOReturnNoMemory;
        if (ret == kIOReturnSuccess) ret = CreateActionReadComplete(sizeof(uint32_t), &ivars->readAction[i]);
        if (ret != kIOReturnSuccess) {
            XPadLog("GIP %{public}s: read %u setup: 0x%x", ivars->name, i, ret);
            return false;
        }
        /* The completion finds its buffer by this index. */
        *static_cast<uint32_t *>(ivars->readAction[i]->GetReference()) = i;
    }

    /* The OUT buffer is the dext's own IOBufferMemoryDescriptor: one from
     * the interface's CreateIOBuffer(kIOMemoryDirectionOut) gave an address
     * (GetAddressRange) that was not mapped in the dext, and the first frame
     * written there (the Identify after a Hello) crashed it with a
     * translation fault. XPadCreateBuffer writes the whole buffer once
     * (memset) before anything uses it. The IN buffers keep CreateIOBuffer:
     * they are only read, and read fine. */
    ret = XPadCreateBuffer(kIOMemoryDirectionOut, kFrameMax, &ivars->writeBuffer, &ivars->writeBytes);
    if (ret == kIOReturnSuccess) ret = CreateActionWriteComplete(0, &ivars->writeAction);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP %{public}s: write setup: 0x%x", ivars->name, ret);
        return false;
    }

    ret = XPadCreateBuffer(kIOMemoryDirectionInOut, XPAD_XBOX_INPUT_LEN, &ivars->xboxReport, &ivars->xboxBytes);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP %{public}s: input report buffer: 0x%x", ivars->name, ret);
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
        XPadLog("GIP %{public}s: timer setup: 0x%x", ivars->name, ret);
        return false;
    }
    return true;
}

kern_return_t XPadGIPGamepad::submitRead(uint32_t index)
{
    /* No timeout (interrupt endpoints take none anyway), and never cancelled. */
    return ivars->inPipe->AsyncIO(ivars->readBuffer[index], ivars->readSize, ivars->readAction[index], 0);
}

void IMPL(XPadGIPGamepad, ReadComplete)
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
        long now = XPadNowMs();
        uint64_t length = actualByteCount < ivars->readMap[index].length ? actualByteCount
                                                                         : ivars->readMap[index].length;
        int result;

        ivars->readErrors = 0;
        result = gip_receive(&ivars->gip, ivars->readMap[index].bytes, (int)length, now, &ivars->state,
                             XPadGIPSend, this);
        if (result & GIP_RX_STATE) publish(completionTimestamp);
        break;
    }
    case kIOReturnNoDevice:
        /* The pad is gone: Stop follows. */
        ivars->readsQueued--;
        XPadLog("GIP %{public}s: read %u ended: 0x%x (%u still queued)", ivars->name, index, status,
                ivars->readsQueued);
        return;
    case kIOReturnAborted:
        /* Not a teardown (that sets stopping first, see above). Our own
         * ClearStall (below) aborts every read queued on the endpoint, so
         * this is most likely the other read; otherwise the interface is
         * going away, and then the resubmit fails or Stop follows. Either
         * way it is read again, without a clear (an abort leaves no halt),
         * and counted so that a pipe that only aborts cannot spin. */
        if (++ivars->readErrors > kMaxErrorsInRow) {
            ivars->readsQueued--;
            XPadLog("GIP %{public}s: read %u aborted, %u failures in a row: it stops (%u still queued)",
                    ivars->name, index, ivars->readErrors, ivars->readsQueued);
            return;
        }
        break;
    default:
        /* kIOReturnNotResponding (what xHCI reports for a transaction error
         * on a pad that is still there: a marginal cable, a hub), Offline,
         * a stall, an overrun... Any error but a timeout leaves an interrupt
         * endpoint halted (IOUSBHostPipe.iig): nothing more arrives, not
         * even for the other read, until the halt is cleared. So clear it
         * and read on. Apple's IOUserUSBHostHIDDevice recovers its input
         * pipe the same way (scheduleInputReportRetry, then initInputReport:
         * ClearStall and a new AsyncIO, a bounded number of times).
         * A pad that really left ends this within kMaxErrorsInRow passes
         * (ClearStall and AsyncIO then fail at once), and Stop comes anyway.
         * This is the only clear outside teardown: a working pipe is never
         * touched. */
        if (ivars->readFailures++ < kLogErrors)
            XPadLog("GIP %{public}s: read %u failed: 0x%x", ivars->name, index, status);
        if (++ivars->readErrors > kMaxErrorsInRow) {
            ivars->readsQueued--;
            XPadLog("GIP %{public}s: %u reads failed in a row, read %u stops (%u still queued)", ivars->name,
                    ivars->readErrors, index, ivars->readsQueued);
            return;
        }
        ret = ivars->inPipe->ClearStall(true);
        if (ret != kIOReturnSuccess) XPadLog("GIP %{public}s: IN ClearStall: 0x%x", ivars->name, ret);
        break;
    }

    ret = submitRead(index);
    if (ret != kIOReturnSuccess) {
        ivars->readsQueued--;
        XPadLog("GIP %{public}s: read %u: AsyncIO: 0x%x (%u still queued)", ivars->name, index, ret,
                ivars->readsQueued);
    }
}

int XPadGIPGamepad::sendFrame(const uint8_t *frame, int length)
{
    XPadOutFrame *slot;

    if (ivars->stopping || !frame || length <= 0 || length > (int)kFrameMax) return 0;
    if (ivars->fifoCount == kQueueDepth) {
        if (ivars->framesDropped++ < kLogErrors)
            XPadLog("GIP %{public}s: OUT queue full, frame %02x dropped", ivars->name, frame[0]);
        return 0;
    }
    slot = &ivars->fifo[(ivars->fifoHead + ivars->fifoCount) % kQueueDepth];
    memcpy(slot->bytes, frame, (size_t)length);
    slot->length = (uint8_t)length;
    ivars->fifoCount++;
    startWrite();
    return 1;
}

/* Starts the next OUT transfer if none is in flight: queued frames first,
 * then a pending rumble (at most one every kRumbleSpacingMs; the timer
 * sends one held back). */
void XPadGIPGamepad::startWrite()
{
    while (!ivars->writeBusy && !ivars->stopping) {
        uint32_t length;
        kern_return_t ret;

        if (ivars->fifoCount) {
            const XPadOutFrame *frame = &ivars->fifo[ivars->fifoHead];
            memcpy(ivars->writeBytes, frame->bytes, frame->length);
            length = frame->length;
            ivars->writeIsRumble = false;
            ivars->fifoHead = (ivars->fifoHead + 1) % kQueueDepth;
            ivars->fifoCount--;
        } else if (ivars->rumblePending && gip_up(&ivars->gip)) {
            long now = XPadNowMs();
            int n;

            if (ivars->rumbleSentAt && now - ivars->rumbleSentAt < kRumbleSpacingMs) break;
            n = gip_rumble(&ivars->gip, &ivars->rumble, now, ivars->writeBytes, (int)kFrameMax);
            ivars->rumblePending = false;
            if (n <= 0) break;
            ivars->rumbleSentAt = now;
            length = (uint32_t)n;
            ivars->writeIsRumble = true;
        } else {
            break;
        }

        ret = ivars->outPipe->AsyncIO(ivars->writeBuffer, length, ivars->writeAction, 0);
        if (ret == kIOReturnSuccess) {
            ivars->writeBusy = true;
        } else {
            /* That frame is lost; the loop moves on to the next (a rumble
             * frame is sent again from the timer, not from this loop). */
            if (ivars->writeIsRumble) ivars->rumbleResend = true;
            if (ivars->writeErrors++ < kLogErrors)
                XPadLog("GIP %{public}s: OUT AsyncIO (frame %02x): 0x%x", ivars->name, ivars->writeBytes[0], ret);
        }
    }
}

void IMPL(XPadGIPGamepad, WriteComplete)
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
        /* The pad is gone, or (not a teardown: stopping is not set) the
         * interface is going away; our own ClearStall below never runs with
         * a write in flight. That frame is lost; an abort leaves no halt. */
        if (ivars->writeErrors++ < kLogErrors)
            XPadLog("GIP %{public}s: OUT transfer (frame %02x): 0x%x", ivars->name, ivars->writeBytes[0], status);
        break;
    default: {
        kern_return_t ret = kIOReturnSuccess;

        /* NotResponding (a transaction error), Offline, a stall...: the OUT
         * endpoint is halted now, as in ReadComplete, and every later ack,
         * bring-up frame and rumble would fail behind it. With one transfer
         * in flight at most, clearing it aborts nothing else. */
        if (ivars->writeErrors++ < kLogErrors)
            XPadLog("GIP %{public}s: OUT transfer (frame %02x): 0x%x", ivars->name, ivars->writeBytes[0], status);
        if (++ivars->writeErrorsInRow <= kMaxErrorsInRow) ret = ivars->outPipe->ClearStall(true);
        if (ret != kIOReturnSuccess) XPadLog("GIP %{public}s: OUT ClearStall: 0x%x", ivars->name, ret);
        break;
    }
    }
    startWrite();
}

/* No report since the last one's play time (plus slack) ran out: the
 * client stopped asking without a stop (it died mid-haptic, say). Stop
 * the motors as a real pad would have by now. */
void XPadGIPGamepad::expireRumble(long now)
{
    if (!ivars->rumbleUntil || now < ivars->rumbleUntil) return;
    ivars->rumbleUntil = 0;
    ivars->rumble = xpad_rumble{};
    ivars->rumblePending = gip_up(&ivars->gip);     /* a pad that is down is not rumbling */
    if (ivars->rumbleExpired++ < kLogErrors)
        XPadLog("GIP %{public}s: rumble expired with no stop: motors stopped", ivars->name);
}

void IMPL(XPadGIPGamepad, TimerFired)
{
    (void)action;
    (void)time;

    if (ivars->stopping) return;
    expireRumble(XPadNowMs());
    if (ivars->rumbleResend) {
        ivars->rumbleResend = false;
        if (!ivars->rumblePending && gip_up(&ivars->gip) && ivars->rumbleResends < kMaxErrorsInRow) {
            ivars->rumbleResends++;
            ivars->rumblePending = true;            /* the latest request: what failed, or newer */
        }
    }
    gip_tick(&ivars->gip, XPadNowMs(), XPadGIPSend, this);
    startWrite();
    armTimer();
}

/* The deadline is in mach_absolute_time ticks: kIOTimerClockMachAbsoluteTime
 * is the timebase every DriverKit version takes (the nanosecond clocks
 * came later). */
void XPadGIPGamepad::armTimer()
{
    uint64_t tick, leeway;

    if (!ivars->timer || ivars->stopping || !ivars->timebase.numer) return;
    tick = kTickNs * ivars->timebase.denom / ivars->timebase.numer;
    leeway = kTickLeewayNs * ivars->timebase.denom / ivars->timebase.numer;
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + tick, leeway);
}

void XPadGIPGamepad::publish(uint64_t timestamp)
{
    kern_return_t ret;

    if (xpad_xbox_build_input(&ivars->state, ivars->xboxBytes, XPAD_XBOX_INPUT_LEN) != XPAD_XBOX_INPUT_LEN) return;
    if (!timestamp) timestamp = mach_absolute_time();
    ret = handleReport(timestamp, ivars->xboxReport, XPAD_XBOX_INPUT_LEN, kIOHIDReportTypeInput, 0);
    if (ret != kIOReturnSuccess && ivars->reportErrors++ < kLogErrors)
        XPadLog("GIP %{public}s: handleReport: 0x%x", ivars->name, ret);
}

OSDictionary *XPadGIPGamepad::newDeviceDescription()
{
    char source[24];
    XPadIdentity identity = {};
    OSDictionary *description;

    snprintf(source, sizeof source, "GIP %04x:%04x", ivars->vid, ivars->pid);
    identity.source = source;
    identity.locationID = ivars->locationID;
    identity.serial = ivars->serial[0] ? ivars->serial : nullptr;
    identity.reportIntervalUs = ivars->reportIntervalUs ? ivars->reportIntervalUs : kDefaultIntervalUs;
    description = XPadCreateXboxDescription(nullptr, &identity);
    if (!description) XPadLog("GIP %{public}s: no device description", ivars->name);
    return description;
}

OSData *XPadGIPGamepad::newReportDescriptor()
{
    return OSData::withBytes(xpad_xbox_desc, XPAD_XBOX_DESC_LEN);
}

kern_return_t XPadGIPGamepad::setReport(IOMemoryDescriptor *report,
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
        /* The latest request always replaces one not sent yet, whether the
         * pad is up or not: a stop that arrives while it is down (bringing
         * itself up again after a Hello, say) must still cancel a start
         * held back by an ack in flight or the rumble spacing, or that
         * start would go out once the pad is up and, at 2.55 s x 255 per
         * frame, run for minutes. startWrite sends it only while the pad is
         * up. A stop needs no sending while it is down (it is not rumbling:
         * every bring-up ends with a rumble stop of its own); a start is
         * sent once it is up. */
        bool on = (rumble.strong | rumble.weak | rumble.lt | rumble.rt) != 0;
        long now = XPadNowMs();

        ivars->rumble = rumble;
        ivars->rumblePending = on || gip_up(&ivars->gip);
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

kern_return_t XPadGIPGamepad::getReport(IOMemoryDescriptor *report,
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
