/*
 * XPadMac driver: the xpad_log() hook the shared C core (gip.c) logs
 * through, implemented with os_log, and xpad_random() (GIP authentication's
 * random numbers), implemented with arc4random_buf (DriverKit's libSystem
 * exports it; it is the kernel's CSPRNG). Read the log with
 *   log stream --predicate 'eventMessage BEGINSWITH "XPad:"'
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <os/log.h>

#include "gip.h"

void xpad_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* The text is the driver's own (frame bytes, states, counters): public. */
    os_log(OS_LOG_DEFAULT, "XPad: %{public}s", line);
}

void xpad_random(void *buf, size_t n)
{
    arc4random_buf(buf, n);
}
