/*
 * xpadctl — diagnostics for XPadMac: what macOS sees of an Xbox-protocol
 * controller, raw HID reports, the GameController view, and (built
 * WITH_LIBUSB=1) a userspace GIP host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpadctl.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
    fprintf(f,
        "xpadctl %s — controller diagnostics for XPadMac\n"
        "\n"
        "Usage: xpadctl <command> [options]\n"
        "\n"
        "Commands:\n"
        "  doctor                 Check everything: macOS, SIP, USB devices, HID,\n"
        "                         GameController, the XPadMac driver and the app.\n"
        "  hid [--vid V --pid P]  Print each raw HID report and the decoded state.\n"
        "                         Default: the XPad device 045E:0B13.\n"
        "        --all            Print every report (default: only when it changes).\n"
        "        --seconds N      Exit after N seconds.\n"
        "  gc [--seconds N]       Live view of controllers through GameController (10 per second).\n"
#ifdef XPADCTL_WITH_LIBUSB
        "  gip [options]          Run a userspace GIP host (libusb) and print the\n"
        "                         decoded state. See: xpadctl gip --help\n"
#else
        "  gip                    (not available: rebuild with `make WITH_LIBUSB=1`)\n"
#endif
        "  help                   Show this help.\n"
        "\n"
        "VID/PID are hexadecimal (e.g. 0e6f, 0x02a8). Press Ctrl-C to stop the live commands.\n",
        XPADCTL_VERSION);
}

int main(int argc, char **argv)
{
    const char *cmd;

    /* Line-buffer stdout even when piped, so live output appears promptly. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    cmd = argv[1];
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) {
        printf("xpadctl %s\n", XPADCTL_VERSION);
        return 0;
    }

    xpadctl_signals_install();

    /* Each command sees its own arguments as argv[1..argc). */
    if (!strcmp(cmd, "doctor")) return xpadctl_cmd_doctor(argc - 1, argv + 1);
    if (!strcmp(cmd, "hid")) return xpadctl_cmd_hid(argc - 1, argv + 1);
    if (!strcmp(cmd, "gc")) return xpadctl_cmd_gc(argc - 1, argv + 1);
    if (!strcmp(cmd, "gip")) {
#ifdef XPADCTL_WITH_LIBUSB
        return xpadctl_cmd_gip(argc - 1, argv + 1);
#else
        fprintf(stderr, "xpadctl gip: this build has no libusb support.\n"
                        "Rebuild with: make -C Tools/xpadctl WITH_LIBUSB=1 (needs Homebrew libusb in /opt/homebrew)\n");
        return 2;
#endif
    }

    fprintf(stderr, "xpadctl: unknown command \"%s\".\n\n", cmd);
    usage(stderr);
    return 2;
}
