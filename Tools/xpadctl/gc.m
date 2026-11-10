/*
 * xpadctl gc — what Apple's GameController framework sees: the controller
 * list for `doctor`, and a live terminal view (10 refreshes a second).
 *
 * A command-line tool is never the "foreground app" GameController delivers
 * input to, so shouldMonitorBackgroundEvents must be set, and the framework
 * only discovers controllers while the main run loop runs (it talks to
 * gamecontrollerd asynchronously): both commands spin the run loop.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "xpadctl.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* How long the framework gets to report controllers that are already
 * connected before `doctor` reads the list. */
static const double kDiscoverySeconds = 1.5;

static void gc_prepare(void)
{
    GCController.shouldMonitorBackgroundEvents = YES;
    (void)GCController.controllers;     /* starts discovery */
}

static NSString *gc_name(GCController *c)
{
    return c.vendorName.length ? c.vendorName : @"(no name)";
}

static BOOL gc_looks_xbox(GCController *c)
{
    NSString *s = [NSString stringWithFormat:@"%@ %@", c.vendorName ?: @"", c.productCategory ?: @""];
    return [s rangeOfString:@"Xbox" options:NSCaseInsensitiveSearch].location != NSNotFound;
}

/* ---- doctor ------------------------------------------------------------------- */

void xpadctl_gc_doctor(int xpad_hid_seen)
{
    @autoreleasepool {
        NSArray<GCController *> *list;
        BOOL xbox = NO;

        gc_prepare();
        xpadctl_run_loop_for(kDiscoverySeconds);
        list = GCController.controllers;

        for (GCController *c in list) {
            BOOL isXbox = gc_looks_xbox(c);
            xbox |= isXbox;
            xpadctl_verdict(isXbox ? XPADCTL_OK : XPADCTL_INFO, "\"%s\" — category \"%s\"%s",
                            gc_name(c).UTF8String, (c.productCategory ?: @"?").UTF8String,
                            c.extendedGamepad ? ", extendedGamepad" : "");
        }
        if (list.count == 0) {
            if (xpad_hid_seen) {
                xpadctl_verdict(XPADCTL_FAIL, "The XPad driver created a HID device, but GameController lists no controllers.");
                xpadctl_hint("Unplug and replug the controller. If that does not help, report the `xpadctl doctor` output and the log: "
                             "log stream --predicate 'eventMessage CONTAINS \"XPad:\"'");
            } else {
                xpadctl_verdict(XPADCTL_WARN, "GameController sees no controllers yet.");
                xpadctl_hint("GIP/XInput controllers only show up here while the XPadMac driver is running (see above).");
            }
        } else if (!xbox && xpad_hid_seen) {
            xpadctl_verdict(XPADCTL_WARN, "There is an XPad device, but GameController does not list it as an Xbox controller.");
        }
    }
}

/* ---- live view ------------------------------------------------------------------ */

static void add_if(NSMutableArray<NSString *> *names, GCControllerButtonInput *b, NSString *name)
{
    if (b && b.isPressed) [names addObject:name];
}

static NSString *axis(float v)
{
    return [NSString stringWithFormat:@"%+.2f", v];
}

/* Two lines for one controller: identity, then its live state. */
static NSString *describe(GCController *c, NSUInteger index)
{
    NSMutableString *s = [NSMutableString string];
    GCExtendedGamepad *gp = c.extendedGamepad;

    [s appendFormat:@"[%lu] %@ — %@\n", (unsigned long)index, gc_name(c), c.productCategory ?: @"?"];
    if (gp) {
        NSMutableArray<NSString *> *pressed = [NSMutableArray array];
        add_if(pressed, gp.buttonA, @"A");
        add_if(pressed, gp.buttonB, @"B");
        add_if(pressed, gp.buttonX, @"X");
        add_if(pressed, gp.buttonY, @"Y");
        add_if(pressed, gp.leftShoulder, @"LB");
        add_if(pressed, gp.rightShoulder, @"RB");
        add_if(pressed, gp.leftTrigger, @"LT");
        add_if(pressed, gp.rightTrigger, @"RT");
        add_if(pressed, gp.leftThumbstickButton, @"LS");
        add_if(pressed, gp.rightThumbstickButton, @"RS");
        add_if(pressed, gp.buttonOptions, @"View");
        add_if(pressed, gp.buttonMenu, @"Menu");
        /* macOS keeps the Home (Xbox) button for a system gesture
         * (Launchpad, the Games app) and reports it to apps only if they
         * ask: real Xbox controllers behave the same. AlwaysReceive is
         * deprecated from macOS 27 (WITH_LIBUSB=1 builds target the running
         * macOS); it still works, so the warning is silenced here. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (gp.buttonHome && gp.buttonHome.preferredSystemGestureState != GCSystemGestureStateAlwaysReceive)
            gp.buttonHome.preferredSystemGestureState = GCSystemGestureStateAlwaysReceive;
#pragma clang diagnostic pop
        add_if(pressed, gp.buttonHome, @"Home");
        if ([gp isKindOfClass:GCXboxGamepad.class]) {
            GCXboxGamepad *x = (GCXboxGamepad *)gp;
            add_if(pressed, x.buttonShare, @"Share");
            add_if(pressed, x.paddleButton1, @"P1");
            add_if(pressed, x.paddleButton2, @"P2");
            add_if(pressed, x.paddleButton3, @"P3");
            add_if(pressed, x.paddleButton4, @"P4");
        }
        add_if(pressed, gp.dpad.up, @"↑");
        add_if(pressed, gp.dpad.down, @"↓");
        add_if(pressed, gp.dpad.left, @"←");
        add_if(pressed, gp.dpad.right, @"→");

        [s appendFormat:@"    L(%@,%@) R(%@,%@)  LT %.2f  RT %.2f  buttons: %@\n",
                        axis(gp.leftThumbstick.xAxis.value), axis(gp.leftThumbstick.yAxis.value),
                        axis(gp.rightThumbstick.xAxis.value), axis(gp.rightThumbstick.yAxis.value),
                        gp.leftTrigger.value, gp.rightTrigger.value,
                        pressed.count ? [pressed componentsJoinedByString:@" "] : @"-"];
    } else {
        /* Not an extended gamepad: list whatever buttons the profile has. */
        NSMutableArray<NSString *> *pressed = [NSMutableArray array];
        [c.physicalInputProfile.buttons enumerateKeysAndObjectsUsingBlock:^(NSString *key,
                                                                            GCControllerButtonInput *b, BOOL *stop) {
            (void)stop;
            if (b.isPressed) [pressed addObject:key];
        }];
        [pressed sortUsingSelector:@selector(compare:)];
        [s appendFormat:@"    (no extendedGamepad)  buttons: %@\n",
                        pressed.count ? [pressed componentsJoinedByString:@", "] : @"-"];
    }
    return s;
}

static NSString *frame(void)
{
    NSArray<GCController *> *list = GCController.controllers;
    NSMutableString *s = [NSMutableString string];
    NSUInteger i = 0;

    [s appendFormat:@"GameController: %lu controller(s) (Ctrl-C to stop)\n", (unsigned long)list.count];
    if (list.count == 0)
        [s appendString:@"    No controllers yet. GIP/XInput controllers only show up here\n"
                        @"    while the XPadMac driver is running (see `xpadctl doctor`).\n"];
    for (GCController *c in list) [s appendString:describe(c, ++i)];
    return s;
}

/* Terminal rows `s` occupies once long lines wrap at `cols` columns, so the
 * next frame can move the cursor back up exactly that far. Counts characters
 * (precomposed): every glyph used here is one column wide. */
static NSUInteger rows_for(NSString *s, NSUInteger cols)
{
    NSUInteger rows = 0;

    for (NSString *line in [s.precomposedStringWithCanonicalMapping componentsSeparatedByString:@"\n"]) {
        if (line.length == 0) continue;     /* the empty piece after the final '\n' */
        rows += cols ? (line.length + cols - 1) / cols : 1;
    }
    return rows;
}

static NSUInteger terminal_cols(void)
{
    struct winsize w;
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 ? w.ws_col : 80;
}

int xpadctl_cmd_gc(int argc, char **argv)
{
    double seconds = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds")) {
            if (i + 1 >= argc || !xpadctl_parse_seconds(argv[i + 1], &seconds))
                return xpadctl_usage_error("gc", "--seconds needs a non-negative number of seconds");
            i++;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("xpadctl gc [--seconds N]: live view of controllers through GameController, 10 times a second.\n");
            return 0;
        } else {
            return xpadctl_usage_error("gc", "unknown option: %s", argv[i]);
        }
    }

    @autoreleasepool {
        /* On a terminal the frame is redrawn in place; piped, a frame is only
         * printed when it differs from the previous one (with a timestamp). */
        BOOL tty = isatty(STDOUT_FILENO);
        NSString *last = nil;
        NSUInteger lastLines = 0;

        gc_prepare();
        xpadctl_set_deadline(seconds);
        if (tty) fputs("\033[?25l", stdout);            /* hide the cursor */

        while (!xpadctl_should_stop()) {
            @autoreleasepool {
                NSString *now = frame();
                if (tty) {
                    if (lastLines) printf("\033[%luA", (unsigned long)lastLines);   /* back to the top */
                    fputs("\r\033[J", stdout);                                      /* clear below */
                    fputs(now.UTF8String, stdout);
                    fflush(stdout);
                    lastLines = rows_for(now, terminal_cols());
                } else if (![now isEqualToString:last]) {
                    printf("[%7.3f]\n%s", xpadctl_now_ms() / 1000.0, now.UTF8String);
                    fflush(stdout);
                }
                last = now;
            }
            xpadctl_run_loop_for(0.1);   /* lets GameController deliver connects and values */
        }
        if (tty) fputs("\033[?25h", stdout);            /* show the cursor again */
        printf("\n");
    }
    return 0;
}
