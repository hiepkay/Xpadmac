/*
 * xpadctl doctor — one screen that says what macOS sees of the controller
 * and what to do next, with ✅/⚠️/❌ verdicts and hints.
 *
 * Read-only: it reads sysctl, the IORegistry and IOHIDManager properties,
 * GameController's list, the output of `csrutil status` and
 * `systemextensionsctl list`, and the installed app's code signature. It
 * never opens a USB device or sends anything.
 *
 * Exit status: 0 if nothing is ❌, 1 otherwise.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpadctl.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <Security/Security.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#define APP_PATH "/Applications/XPadMac.app"
#define HINT_INSTALL_DRIVER \
    "Install the driver: open " APP_PATH ", go to Driver, click \"Install Driver\", then allow it in " \
    "System Settings › General › " \
    "Login Items & Extensions › Driver Extensions."

/* ---- small IORegistry helpers ---------------------------------------------- */

static CFTypeRef reg_copy(io_registry_entry_t e, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = k ? IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0) : NULL;

    if (k) CFRelease(k);
    return v;
}

static int reg_number(io_registry_entry_t e, const char *key, long long *out)
{
    CFTypeRef v = reg_copy(e, key);
    int ok = xpadctl_cf_number(v, out);

    if (v) CFRelease(v);
    return ok;
}

static int reg_string(io_registry_entry_t e, const char *key, char *out, size_t out_max)
{
    CFTypeRef v = reg_copy(e, key);
    int ok = xpadctl_cf_string(v, out, out_max);

    if (v) CFRelease(v);
    return ok;
}

/* "IOClass [IOUserClass]" for a driver entry: the user class is the dext's. */
static void describe_driver(io_registry_entry_t e, char *out, size_t out_max)
{
    io_name_t cls;
    char uc[128];

    if (IOObjectGetClass(e, cls) != KERN_SUCCESS) strcpy(cls, "?");
    if (reg_string(e, "IOUserClass", uc, sizeof uc) && uc[0]) snprintf(out, out_max, "%s [%s]", cls, uc);
    else snprintf(out, out_max, "%s", cls);
}

/* Appends `s` to the comma-separated list `out` unless already there. */
static void list_add(char *out, size_t out_max, const char *s)
{
    size_t k = strlen(out);
    const char *p = out;
    size_t n = strlen(s);

    while ((p = strstr(p, s))) {                /* whole-item match only */
        if ((p == out || p[-1] == ' ') && (p[n] == '\0' || p[n] == ',')) return;
        p += n;
    }
    snprintf(out + k, out_max - k, "%s%s", k ? ", " : "", s);
}

/* The XPad dext classes (IOUserClass "XPad…") anywhere below `e`. */
static void xpad_classes_below(io_registry_entry_t e, char *out, size_t out_max)
{
    io_iterator_t it;
    io_registry_entry_t c;
    char uc[128];

    out[0] = '\0';
    if (IORegistryEntryCreateIterator(e, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS)
        return;
    while ((c = IOIteratorNext(it))) {
        if (reg_string(c, "IOUserClass", uc, sizeof uc) && !strncmp(uc, "XPad", 4)) list_add(out, out_max, uc);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
}

/* ---- macOS and SIP ------------------------------------------------------------ */

static int sysctl_string(const char *name, char *out, size_t out_max)
{
    size_t len = out_max;

    if (sysctlbyname(name, out, &len, NULL, 0) != 0 || len == 0) { out[0] = '\0'; return 0; }
    out[out_max - 1] = '\0';
    return 1;
}

static int sysctl_int(const char *name, int *out)
{
    size_t len = sizeof *out;
    return sysctlbyname(name, out, &len, NULL, 0) == 0;
}

static void check_system(void)
{
    char ver[64], build[64], model[64];
    struct utsname u;
    int translated = 0, arm64 = 0, major;

    xpadctl_section("System");
    sysctl_string("kern.osproductversion", ver, sizeof ver);
    sysctl_string("kern.osversion", build, sizeof build);
    sysctl_string("hw.model", model, sizeof model);
    major = atoi(ver);
    if (!ver[0]) xpadctl_verdict(XPADCTL_WARN, "Could not read the macOS version.");
    else if (major >= 14) xpadctl_verdict(XPADCTL_OK, "macOS %s (%s)", ver, build[0] ? build : "?");
    else {
        xpadctl_verdict(XPADCTL_FAIL, "macOS %s: XPadMac needs macOS 14 or later.", ver);
        xpadctl_hint("Update macOS.");
    }

    if (uname(&u) != 0) strcpy(u.machine, "?");
    sysctl_int("sysctl.proc_translated", &translated);
    sysctl_int("hw.optional.arm64", &arm64);
    if (translated) {
        xpadctl_verdict(XPADCTL_WARN, "xpadctl is running under Rosetta (%s, Apple silicon).", model);
        xpadctl_hint("Build xpadctl for arm64 (make -C Tools/xpadctl) for accurate results.");
    } else if (arm64 || !strcmp(u.machine, "arm64")) {
        xpadctl_verdict(XPADCTL_OK, "Apple silicon (%s, %s)", u.machine, model);
    } else {
        xpadctl_verdict(XPADCTL_WARN, "Intel Mac (%s, %s): XPadMac has only been tested on Apple silicon.",
                        u.machine, model);
    }
}

/* Runs a fixed command line and returns its output (caller frees), or NULL. */
static char *run_capture(const char *cmdline)
{
    FILE *p = popen(cmdline, "r");
    char *buf = NULL, *nb;
    size_t len = 0, cap = 0, n;
    char chunk[1024];

    if (!p) return NULL;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(p); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    pclose(p);
    if (!buf) buf = calloc(1, 1);
    else buf[len] = '\0';
    return buf;
}

static void check_sip(void)
{
    char *out = run_capture("/usr/bin/csrutil status 2>&1");

    xpadctl_section("System Integrity Protection (SIP)");
    if (!out || !*out) {
        xpadctl_verdict(XPADCTL_WARN, "Could not run `csrutil status`.");
    } else if (strstr(out, "status: enabled")) {
        xpadctl_verdict(XPADCTL_OK, "SIP is enabled — as it should be; XPadMac does not need SIP disabled.");
    } else if (strstr(out, "status: disabled")) {
        xpadctl_verdict(XPADCTL_WARN, "SIP is disabled. XPadMac does not need SIP disabled.");
        xpadctl_hint("Consider turning SIP back on (Recovery › Terminal › `csrutil enable`) to keep the Mac safer.");
    } else {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
        xpadctl_verdict(XPADCTL_INFO, "%s", out);
    }
    free(out);
}

/* ---- USB devices --------------------------------------------------------------- */

static const struct { uint16_t vid; const char *name; } gip_vendors[] = {
    { 0x045E, "Microsoft" }, { 0x2DC8, "8BitDo" },   { 0x3537, "GameSir" },  { 0x0F0D, "Hori" },
    { 0x2E24, "Hyperkin" },  { 0x0738, "Mad Catz" }, { 0x0E6F, "PDP" },      { 0x24C6, "PowerA" },
    { 0x20D6, "PowerA" },    { 0x1532, "Razer" },    { 0x10F5, "Turtle Beach" },
};

/* 1 if the device has an Xbox interface: vendor class 255 with subclass
 * 0x5D (XInput, Xbox 360) or 0x47 (GIP, Xbox One/Series), whatever the
 * vendor (8BitDo, Logitech, clones...). */
static int has_xbox_interface(io_registry_entry_t dev)
{
    io_iterator_t it = 0;
    io_registry_entry_t child;
    int found = 0;

    if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    while (!found && (child = IOIteratorNext(it))) {
        long long cls = -1, sub = -1;
        if (IOObjectConformsTo(child, "IOUSBHostInterface")) {
            reg_number(child, "bInterfaceClass", &cls);
            reg_number(child, "bInterfaceSubClass", &sub);
            if (cls == 0xFF && (sub == 0x5D || sub == 0x47)) found = 1;
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    return found;
}

static const char *vendor_name(uint16_t vid)
{
    size_t i;
    for (i = 0; i < sizeof gip_vendors / sizeof gip_vendors[0]; i++)
        if (gip_vendors[i].vid == vid) return gip_vendors[i].name;
    return NULL;
}

/* PIDs Apple's own XboxGamepad.dext drives. */
static int apple_drives(uint16_t vid, uint16_t pid)
{
    return vid == 0x045E && (pid == 0x028E || pid == 0x02EA || pid == 0x0B00 || pid == 0x0B12);
}

/* Microsoft PIDs the XPad driver matches (Xbox One 2013/2015, Elite 1). */
static int xpad_ms_pid(uint16_t pid)
{
    return pid == 0x02D1 || pid == 0x02DD || pid == 0x02E3;
}

static int is_gip_interface(io_registry_entry_t iface)
{
    long long cls = 0, sub = 0, proto = 0;

    reg_number(iface, "bInterfaceClass", &cls);
    reg_number(iface, "bInterfaceSubClass", &sub);
    reg_number(iface, "bInterfaceProtocol", &proto);
    return cls == 0xFF && sub == 0x47 && proto == 0xD0;
}

/* The Xbox 360 wired (XInput) gamepad interface, FF/5D/01: XPad360Gamepad's. */
static int is_xinput_interface(io_registry_entry_t iface)
{
    long long cls = 0, sub = 0, proto = 0;

    reg_number(iface, "bInterfaceClass", &cls);
    reg_number(iface, "bInterfaceSubClass", &sub);
    reg_number(iface, "bInterfaceProtocol", &proto);
    return cls == 0xFF && sub == 0x5D && proto == 0x01;
}

/* 1 if one of the device's interfaces is an XInput gamepad (FF/5D/01). */
static int has_xinput_interface(io_registry_entry_t dev)
{
    io_iterator_t it = 0;
    io_registry_entry_t child;
    int found = 0;

    if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    while (!found && (child = IOIteratorNext(it))) {
        if (IOObjectConformsTo(child, "IOUSBHostInterface") && is_xinput_interface(child)) found = 1;
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    return found;
}

/* Walks the device's direct clients: its interfaces (once configured) and any
 * device-level driver. Prints them if `print`; returns 1 if one interface is
 * the GIP interface FF/47/D0. */
static int walk_interfaces(io_registry_entry_t dev, int print)
{
    io_iterator_t it, it2;
    io_registry_entry_t c, d;
    int any_gip = 0;

    if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    while ((c = IOIteratorNext(it))) {
        char drivers[512] = "", one[192];

        if (IOObjectConformsTo(c, "IOUSBHostInterface")) {
            long long num = -1, cls = 0, sub = 0, proto = 0;
            int gip = is_gip_interface(c);

            any_gip |= gip;
            if (print) {
                reg_number(c, "bInterfaceNumber", &num);
                reg_number(c, "bInterfaceClass", &cls);
                reg_number(c, "bInterfaceSubClass", &sub);
                reg_number(c, "bInterfaceProtocol", &proto);
                if (IORegistryEntryGetChildIterator(c, kIOServicePlane, &it2) == KERN_SUCCESS) {
                    while ((d = IOIteratorNext(it2))) {
                        describe_driver(d, one, sizeof one);
                        list_add(drivers, sizeof drivers, one);
                        IOObjectRelease(d);
                    }
                    IOObjectRelease(it2);
                }
                xpadctl_detail("interface %lld  %02llX/%02llX/%02llX%s → %s", num, cls, sub, proto,
                               gip ? " (GIP)" : is_xinput_interface(c) ? " (XInput)" : "",
                               drivers[0] ? drivers : "no driver");
            }
        } else if (print) {
            describe_driver(c, one, sizeof one);
            xpadctl_detail("device driver: %s", one);
        }
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
    return any_gip;
}

/* User-space programs holding a user client on the device itself (IOUSBLib:
 * Chrome's WebUSB/Gamepad code, a libusb tool...), as "Name (pid N)", into
 * `out`. Returns how many. Such a program may have the device open, and only
 * one client besides the interfaces can (IOUSBHostDevice.h), which would make
 * XPadGIPDevice's Open fail. */
static int user_clients(io_registry_entry_t dev, char *out, size_t out_max)
{
    io_iterator_t it;
    io_registry_entry_t c;
    int n = 0;

    out[0] = '\0';
    if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it) != KERN_SUCCESS) return 0;
    while ((c = IOIteratorNext(it))) {
        char creator[160], one[192];

        if (IOObjectConformsTo(c, "IOUserClient") && reg_string(c, "IOUserClientCreator", creator, sizeof creator)) {
            /* "pid 58997, Google Chrome" -> "Google Chrome (pid 58997)" */
            const char *comma = strstr(creator, ", ");
            if (!strncmp(creator, "pid ", 4) && comma)
                snprintf(one, sizeof one, "%s (pid %.*s)", comma + 2, (int)(comma - creator - 4), creator + 4);
            else
                snprintf(one, sizeof one, "%s", creator);
            list_add(out, out_max, one);
            n++;
        }
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
    return n;
}

static void check_usb_device(io_registry_entry_t dev, uint16_t vid, uint16_t pid)
{
    char product[128], xpad[256], what[320], clients[512];
    const char *hints[2] = { NULL, NULL };
    long long config = 0, location = 0;
    xpadctl_level level;
    int configured, gip_iface, xinput_iface, i;

    if (!reg_string(dev, "USB Product Name", product, sizeof product) || !product[0])
        snprintf(product, sizeof product, "%s", vendor_name(vid) ? vendor_name(vid) : "?");
    reg_number(dev, "locationID", &location);
    configured = reg_number(dev, "kUSBCurrentConfiguration", &config) && config > 0;
    gip_iface = configured && walk_interfaces(dev, 0);
    xinput_iface = configured && has_xinput_interface(dev);
    xpad_classes_below(dev, xpad, sizeof xpad);

    /* The verdict, from the most specific case to the most general. */
    if (apple_drives(vid, pid)) {
        level = XPADCTL_OK;
        snprintf(what, sizeof what, "supported by Apple (XboxGamepad.dext), XPadMac not needed");
    } else if (strstr(xpad, "XPad360Gamepad")) {
        level = XPADCTL_OK;
        snprintf(what, sizeof what, "Xbox 360 (XInput), driver %s running", xpad);
    } else if (strstr(xpad, "XPadGIPGamepad")) {
        level = XPADCTL_OK;
        snprintf(what, sizeof what, "GIP, driver %s running", xpad);
    } else if (xinput_iface) {
        level = XPADCTL_WARN;
        snprintf(what, sizeof what, "Xbox 360 (XInput): interface FF/5D/01 has no XPad360Gamepad driver yet");
        hints[0] = vendor_name(vid) ? HINT_INSTALL_DRIVER
                                    : "This vendor is not in the list XPadMac matches (see README).";
        hints[1] = "Once the driver is installed, unplug and replug the controller so XPad360Gamepad picks it up.";
    } else if (strstr(xpad, "XPadGIPDevice")) {
        level = XPADCTL_WARN;
        snprintf(what, sizeof what, "XPadGIPDevice configured it, but XPadGIPGamepad has not attached to interface 0");
        hints[0] = "Check the driver log: log stream --predicate 'eventMessage CONTAINS \"XPad:\"'";
    } else if (configured) {
        level = XPADCTL_WARN;
        snprintf(what, sizeof what, "configured, but no XPad driver");
        hints[0] = gip_iface ? "It has a GIP interface, but another program or driver holds it (see the list above)."
                             : "No GIP interface (FF/47/D0): this may not be an Xbox One controller.";
    } else if (vid != 0x045E || xpad_ms_pid(pid)) {
        level = XPADCTL_FAIL;
        snprintf(what, sizeof what, "GIP controller with no driver: macOS ignores it (not configured, no interfaces)");
        hints[0] = "Apple only supports 045E:028E/02EA/0B00/0B12. " HINT_INSTALL_DRIVER;
        hints[1] = "Some third-party controllers fall back to a HID mode when no GIP host answers; "
                   "once the driver is running, unplug and replug the controller.";
    } else {
        level = XPADCTL_WARN;
        snprintf(what, sizeof what, "Microsoft device not configured; XPadMac does not support this PID");
    }

    xpadctl_verdict(level, "%04x:%04x \"%s\" — %s", vid, pid, product, what);
    if (configured) xpadctl_detail("location 0x%08llx, configured (configuration %lld)", location, config);
    else xpadctl_detail("location 0x%08llx, NOT configured", location);
    if (configured) walk_interfaces(dev, 1);
    for (i = 0; i < 2; i++)
        if (hints[i]) xpadctl_hint("%s", hints[i]);
    if (user_clients(dev, clients, sizeof clients)) {
        xpadctl_detail("programs using this USB device: %s", clients);
        if (!strstr(xpad, "XPadGIP") && !strstr(xpad, "XPad360Gamepad"))
            xpadctl_hint("A program holding the device can stop the XPad driver from configuring the controller: "
                         "quit that program, then unplug and replug the controller.");
    }
}

static void check_usb(void)
{
    io_iterator_t it;
    io_registry_entry_t dev;
    unsigned found = 0;

    xpadctl_section("USB devices (Xbox controllers: GIP and XInput)");
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS) {
        xpadctl_verdict(XPADCTL_WARN, "Could not read the IORegistry.");
        return;
    }
    while ((dev = IOIteratorNext(it))) {
        long long vid = -1, pid = -1, dclass = -1;

        reg_number(dev, "idVendor", &vid);
        reg_number(dev, "idProduct", &pid);
        reg_number(dev, "bDeviceClass", &dclass);
        /* The listed vendors also make mice, keyboards and headsets: only the
         * vendor-specific class 255 that Xbox controllers use counts, or an
         * Xbox interface (GIP or XInput) whatever the vendor. */
        if ((vendor_name((uint16_t)vid) && dclass == 0xFF) || has_xbox_interface(dev)) {
            found++;
            check_usb_device(dev, (uint16_t)vid, (uint16_t)pid);
        }
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    if (!found) {
        xpadctl_verdict(XPADCTL_WARN, "No Xbox controller (GIP or XInput) found on USB.");
        xpadctl_hint("Plug in the controller (or its dock/receiver) and press the Xbox button to turn it on.");
    }
}

/* ---- HID gamepads ---------------------------------------------------------------- */

static CFDictionaryRef usage_dict(int usage)
{
    int page = 0x01;
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef u = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    const void *keys[2] = { CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey) };
    const void *vals[2] = { p, u };
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(p);
    CFRelease(u);
    return d;
}

static int check_hid(void)
{
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFDictionaryRef d[3];
    CFArrayRef match;
    CFSetRef set;
    CFIndex n = 0, i;
    const void **devs = NULL;
    int xpad_seen = 0, k;

    xpadctl_section("HID controllers (IOHIDManager)");
    if (!mgr) { xpadctl_verdict(XPADCTL_WARN, "Could not create an IOHIDManager."); return 0; }
    d[0] = usage_dict(0x05);    /* Game Pad */
    d[1] = usage_dict(0x04);    /* Joystick */
    d[2] = usage_dict(0x08);    /* Multi-axis Controller */
    match = CFArrayCreate(kCFAllocatorDefault, (const void **)d, 3, &kCFTypeArrayCallBacks);
    for (k = 0; k < 3; k++) CFRelease(d[k]);
    IOHIDManagerSetDeviceMatchingMultiple(mgr, match);
    CFRelease(match);

    /* Properties only: the devices are never opened. */
    set = IOHIDManagerCopyDevices(mgr);
    if (set) n = CFSetGetCount(set);
    if (n > 0 && (devs = calloc((size_t)n, sizeof *devs))) CFSetGetValues(set, devs);
    else n = 0;

    for (i = 0; i < n; i++) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)devs[i];
        char product[128], transport[64], source[128], uclass[128] = "";
        long long vid = 0, pid = 0;
        io_service_t svc = IOHIDDeviceGetService(dev);

        xpadctl_cf_number(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDVendorIDKey)), &vid);
        xpadctl_cf_number(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)), &pid);
        if (!xpadctl_cf_string(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey)), product, sizeof product))
            strcpy(product, "?");
        if (!xpadctl_cf_string(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)), transport, sizeof transport))
            strcpy(transport, "?");
        xpadctl_cf_string(IOHIDDeviceGetProperty(dev, CFSTR("XPadSource")), source, sizeof source);
        if (svc) reg_string(svc, "IOUserClass", uclass, sizeof uclass);

        if (source[0]) {
            /* Published by the XPad driver: check it wears the identity
             * GameController accepts. */
            int identity_ok = vid == XPAD_XBOX_VID && pid == XPAD_XBOX_PID && !strcmp(transport, XPAD_XBOX_TRANSPORT);
            xpad_seen = 1;
            xpadctl_verdict(identity_ok ? XPADCTL_OK : XPADCTL_WARN, "%04llx:%04llx \"%s\" transport %s — XPad device",
                            vid, pid, product, transport);
            xpadctl_detail("XPadSource = \"%s\"%s%s", source, uclass[0] ? ", driver " : "", uclass);
            if (!identity_ok)
                xpadctl_hint("Identity is not 045E:0B13 / %s: GameController may not accept it. Check the driver version.",
                             XPAD_XBOX_TRANSPORT);
        } else {
            xpadctl_verdict(XPADCTL_INFO, "%04llx:%04llx \"%s\" transport %s%s%s", vid, pid, product, transport,
                            uclass[0] ? ", driver " : "", uclass);
        }
    }
    if (n == 0) xpadctl_verdict(XPADCTL_INFO, "No HID controllers.");
    free(devs);
    if (set) CFRelease(set);
    CFRelease(mgr);
    return xpad_seen;
}

/* ---- system extension and app ---------------------------------------------------- */

static void check_sysext(void)
{
    char *out = run_capture("/usr/bin/systemextensionsctl list 2>&1");
    char *line, *save = NULL;
    int lines = 0, enabled = 0, waiting = 0, removing = 0;

    xpadctl_section("XPadMac driver (System Extension)");
    if (!out) {
        xpadctl_verdict(XPADCTL_WARN, "Could not run `systemextensionsctl list`.");
        return;
    }
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t;
        if (!strcasestr(line, "xpadmac")) continue;
        for (t = line; *t; t++) if (*t == '\t') *t = ' ';
        while (*line == ' ' || *line == '*') line++;    /* the "enabled/active" star columns */
        lines++;
        if (strstr(line, "[activated enabled]")) enabled++;
        else if (strstr(line, "waiting for user")) waiting++;
        else if (strstr(line, "terminated") || strstr(line, "uninstall")) removing++;
        xpadctl_detail("%s", line);
    }
    free(out);

    if (enabled) {
        xpadctl_verdict(XPADCTL_OK, "The driver is enabled (activated enabled).");
    } else if (waiting) {
        xpadctl_verdict(XPADCTL_WARN, "The driver is installed but waiting for your approval.");
        xpadctl_hint("System Settings › General › Login Items & Extensions › Driver Extensions: turn on XPadMac.");
    } else if (removing) {
        xpadctl_verdict(XPADCTL_WARN, "The driver is waiting to be removed, or has stopped.");
        xpadctl_hint("Restart the Mac, then reinstall from XPadMac.app if needed.");
    } else if (lines) {
        xpadctl_verdict(XPADCTL_WARN, "The driver is listed but not enabled.");
        xpadctl_hint(HINT_INSTALL_DRIVER);
    } else {
        xpadctl_verdict(XPADCTL_FAIL, "The XPadMac driver is not installed.");
        xpadctl_hint(HINT_INSTALL_DRIVER);
    }
}

/* The first <name>.dext in `dir` into out (a path); 1 if there is one. */
static int find_dext(const char *dir, char *out, size_t out_max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 5 && !strcmp(e->d_name + n - 5, ".dext")) {
            snprintf(out, out_max, "%s/%s", dir, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* How a bundle is signed, as far as installing the driver goes. */
typedef enum {
    SIGN_UNREADABLE,        /* the signature could not be read */
    SIGN_NONE,              /* not signed */
    SIGN_ADHOC,             /* ad hoc or only linker-signed: `make build-unsigned` */
    SIGN_NO_ENTITLEMENT,    /* signed, without the entitlement asked for */
    SIGN_NO_PROFILE,        /* signed and entitled, but no embedded provisioning profile */
    SIGN_OK
} sign_kind;

/* Reads the bundle's code signature (no validation: only what it claims)
 * and checks that it carries `entitlement` and that a provisioning profile
 * sits at `profile`: a development-signed app or dext has both, a
 * build-unsigned one neither, and sysextd refuses to activate it. */
static sign_kind bundle_signing(const char *path, const char *entitlement, const char *profile)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), true);
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, entitlement, kCFStringEncodingUTF8);
    SecStaticCodeRef code = NULL;
    CFDictionaryRef info = NULL;
    sign_kind kind = SIGN_UNREADABLE;
    struct stat st;

    if (url && key && SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess &&
        SecCodeCopySigningInformation(code, kSecCSSigningInformation | kSecCSRequirementInformation, &info) ==
            errSecSuccess && info) {
        CFTypeRef entitlements = CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict);
        long long flags = 0;

        if (!CFDictionaryContainsKey(info, kSecCodeInfoIdentifier)) {
            kind = SIGN_NONE;           /* the documented test for unsigned code */
        } else if (xpadctl_cf_number(CFDictionaryGetValue(info, kSecCodeInfoFlags), &flags) &&
                   (flags & (kSecCodeSignatureAdhoc | kSecCodeSignatureLinkerSigned))) {
            kind = SIGN_ADHOC;
        } else if (!entitlements || CFGetTypeID(entitlements) != CFDictionaryGetTypeID() ||
                   !CFDictionaryContainsKey((CFDictionaryRef)entitlements, key)) {
            kind = SIGN_NO_ENTITLEMENT;
        } else if (stat(profile, &st) != 0 || !S_ISREG(st.st_mode)) {
            kind = SIGN_NO_PROFILE;
        } else {
            kind = SIGN_OK;
        }
    }
    if (info) CFRelease(info);
    if (code) CFRelease(code);
    if (key) CFRelease(key);
    if (url) CFRelease(url);
    return kind;
}

static const char *sign_kind_text(sign_kind kind)
{
    switch (kind) {
    case SIGN_UNREADABLE: return "signature could not be read";
    case SIGN_NONE: return "not signed";
    case SIGN_ADHOC: return "ad-hoc signed only (a `make build-unsigned` build)";
    case SIGN_NO_ENTITLEMENT: return "signed, but missing the entitlement";
    case SIGN_NO_PROFILE: return "signed, but no provisioning profile";
    case SIGN_OK: return "signed, with entitlement and profile";
    }
    return "?";
}

static void check_app(void)
{
    struct stat st;
    char version[64] = "", build[64] = "", label[256], dext[1024], dext_profile[1100];
    sign_kind app_sign, dext_sign;
    CFURLRef url;

    xpadctl_section("App");
    if (stat(APP_PATH, &st) != 0 || !S_ISDIR(st.st_mode)) {
        xpadctl_verdict(XPADCTL_WARN, "XPadMac.app is not in /Applications.");
        xpadctl_hint("The driver can only be installed from the app in /Applications: run `make build`, then `make install` "
                     "(signing needs an Apple Developer account signed in to Xcode).");
        return;
    }
    url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)APP_PATH,
                                                  (CFIndex)strlen(APP_PATH), true);
    if (url) {
        CFBundleRef b = CFBundleCreate(kCFAllocatorDefault, url);
        if (b) {
            xpadctl_cf_string(CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleShortVersionString")),
                              version, sizeof version);
            xpadctl_cf_string(CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleVersion")), build,
                              sizeof build);
            CFRelease(b);
        }
        CFRelease(url);
    }
    if (version[0] && build[0]) snprintf(label, sizeof label, "%s %s (%s)", APP_PATH, version, build);
    else if (version[0]) snprintf(label, sizeof label, "%s %s", APP_PATH, version);
    else snprintf(label, sizeof label, "%s", APP_PATH);

    if (!find_dext(APP_PATH "/Contents/Library/SystemExtensions", dext, sizeof dext)) {
        xpadctl_verdict(XPADCTL_WARN, "%s has no driver (.dext) in Contents/Library/SystemExtensions.",
                        label);
        xpadctl_hint("Rebuild with `make build`, then `make install`.");
        return;
    }

    /* What sysextd checks before it activates anything: the app may install
     * system extensions, the dext is a DriverKit driver, and both come with
     * the development profile that grants those entitlements. */
    snprintf(dext_profile, sizeof dext_profile, "%s/embedded.provisionprofile", dext);
    app_sign = bundle_signing(APP_PATH, "com.apple.developer.system-extension.install",
                              APP_PATH "/Contents/embedded.provisionprofile");
    dext_sign = bundle_signing(dext, "com.apple.developer.driverkit", dext_profile);
    if (app_sign == SIGN_OK && dext_sign == SIGN_OK) {
        xpadctl_verdict(XPADCTL_OK, "%s (driver included, signed)", label);
        return;
    }
    xpadctl_verdict(XPADCTL_WARN, "%s is not signed correctly: macOS will not install its driver.", label);
    xpadctl_detail("app: %s", sign_kind_text(app_sign));
    xpadctl_detail("driver: %s", sign_kind_text(dext_sign));
    xpadctl_hint("Build a signed copy with `make build` (needs an Apple Developer account signed in to Xcode), "
                 "then `make install`. A `make build-unsigned` build is only for checking that it compiles.");
}

/* ---- the command -------------------------------------------------------------------- */

int xpadctl_cmd_doctor(int argc, char **argv)
{
    int xpad_hid;

    if (argc > 1) {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            printf("xpadctl doctor: checks macOS, SIP, USB, HID, GameController, the driver and the app.\n"
                   "Read-only: sends nothing to the controller. Exit status 1 if any item is ❌.\n");
            return 0;
        }
        return xpadctl_usage_error("doctor", "this command takes no options: %s", argv[1]);
    }

    printf("XPadMac doctor (xpadctl %s)\n", XPADCTL_VERSION);
    check_system();
    check_sip();
    check_usb();
    xpad_hid = check_hid();
    xpadctl_section("GameController");
    xpadctl_gc_doctor(xpad_hid);
    check_sysext();
    check_app();

    printf("\nSummary: %u ✅  %u ⚠️  %u ❌\n", xpadctl_verdict_count(XPADCTL_OK),
           xpadctl_verdict_count(XPADCTL_WARN), xpadctl_verdict_count(XPADCTL_FAIL));
    return xpadctl_verdict_count(XPADCTL_FAIL) ? 1 : 0;
}
