/*
 * xpad_log() for the host: the shared core (Shared/gip.c) logs through this
 * hook; the driver implements it with os_log, the tool with stderr. And
 * xpad_random(), GIP authentication's random numbers: arc4random_buf.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gip.h"
#include "xpadctl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int log_enabled = 1;

void xpadctl_log_set_enabled(int enabled)
{
    log_enabled = enabled;
}

void xpad_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t n;

    if (!log_enabled) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    /* One line per call, whether or not the caller ended it with '\n'. */
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

    /* The decoded state goes to stdout: flush it first so a terminal (or a
     * file both are redirected to) shows the two streams in order. */
    fflush(stdout);
    fprintf(stderr, "[%7.3f] %s\n", xpadctl_now_ms() / 1000.0, line);
}

void xpad_random(void *buf, size_t n)
{
    arc4random_buf(buf, n);
}
