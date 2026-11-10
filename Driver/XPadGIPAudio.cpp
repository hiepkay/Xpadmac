/*
 * XPadGIPAudio: select alternate setting 0 on a GIP pad's audio interface.
 * See XPadGIPAudio.iig.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <USBDriverKit/IOUSBHostInterface.h>

#include "XPadGIPAudio.h"
#include "XPadCommon.h"

struct XPadGIPAudio_IVars {
    IOUSBHostInterface *interface;
    bool                open;
};

bool XPadGIPAudio::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(XPadGIPAudio_IVars, 1);
    return ivars != nullptr;
}

void XPadGIPAudio::free()
{
    if (ivars) {
        OSSafeReleaseNULL(ivars->interface);
        IOSafeDeleteNULL(ivars, XPadGIPAudio_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(XPadGIPAudio, Start)
{
    IOUSBHostInterface *interface;
    kern_return_t ret;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!interface) {
        Stop(provider, SUPERDISPATCH);
        return kIOReturnBadArgument;
    }
    interface->retain();
    ivars->interface = interface;

    ret = interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        XPadLog("GIP audio: interface Open: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }
    ivars->open = true;

    /* Sends SET_INTERFACE(1, 0). The interface stays open (claimed), as
     * xone keeps it, so nothing else selects the streaming setting. */
    ret = interface->SelectAlternateSetting(0);
    XPadLog("GIP audio: SET_INTERFACE(1, alt 0): 0x%x", ret);
    return kIOReturnSuccess;
}

kern_return_t IMPL(XPadGIPAudio, Stop)
{
    if (ivars->open) {
        ivars->interface->Close(this, 0);
        ivars->open = false;
    }
    return Stop(provider, SUPERDISPATCH);
}
