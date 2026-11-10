/*
 * xpadctl: clock, Ctrl-C handling, argument parsing, formatting and the
 * verdict printer used by `doctor`.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "xpadctl.h"

#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- time and stopping ------------------------------------------------------ */

static uint64_t raw_ns(void)
{
    /* CLOCK_UPTIME_RAW: monotonic, not slewed by NTP, does not count sleep —
     * the right base for protocol timers (gip_tick) and log timestamps. */
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

long xpadctl_now_ms(void)
{
    static uint64_t t0;
    uint64_t now = raw_ns();

    if (t0 == 0) t0 = now;
    return (long)((now - t0) / 1000000u);
}

static volatile sig_atomic_t stop_flag;
static long deadline_ms = -1;           /* -1: no deadline */

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;                      /* nothing else is async-signal safe here */
}

void xpadctl_signals_install(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a blocking call returns EINTR so the loop sees the flag. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    (void)xpadctl_now_ms();             /* start the clock */
}

void xpadctl_set_deadline(double seconds)
{
    deadline_ms = seconds > 0 ? xpadctl_now_ms() + (long)(seconds * 1000.0 + 0.5) : -1;
}

int xpadctl_should_stop(void)
{
    if (stop_flag) return 1;
    return deadline_ms >= 0 && xpadctl_now_ms() >= deadline_ms;
}

void xpadctl_run_loop_for(double seconds)
{
    long end = xpadctl_now_ms() + (long)(seconds * 1000.0 + 0.5);

    /* Short slices so Ctrl-C is noticed within 50 ms: the signal handler
     * cannot safely stop the run loop itself. */
    while (!xpadctl_should_stop()) {
        long left = end - xpadctl_now_ms();
        if (left <= 0) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, (left < 50 ? left : 50) / 1000.0, false);
    }
}

int xpadctl_sleep_ms(long ms)
{
    long end = xpadctl_now_ms() + ms;

    while (!xpadctl_should_stop()) {
        long left = end - xpadctl_now_ms();
        struct timespec ts;

        if (left <= 0) return 1;
        if (left > 20) left = 20;
        ts.tv_sec = 0;
        ts.tv_nsec = left * 1000000L;
        nanosleep(&ts, NULL);           /* EINTR (Ctrl-C) just ends the slice early */
    }
    return 0;
}

/* ---- CoreFoundation helpers ---------------------------------------------------- */

int xpadctl_cf_number(const void *cf, long long *out)
{
    if (!cf) return 0;
    if (CFGetTypeID(cf) == CFNumberGetTypeID()) {
        long long v = 0;
        if (!CFNumberGetValue((CFNumberRef)cf, kCFNumberLongLongType, &v)) return 0;
        *out = v;
        return 1;
    }
    if (CFGetTypeID(cf) == CFBooleanGetTypeID()) {
        *out = CFBooleanGetValue((CFBooleanRef)cf) ? 1 : 0;
        return 1;
    }
    return 0;
}

int xpadctl_cf_string(const void *cf, char *out, size_t out_max)
{
    if (!out_max) return 0;
    out[0] = '\0';
    if (!cf || CFGetTypeID(cf) != CFStringGetTypeID()) return 0;
    if (!CFStringGetCString((CFStringRef)cf, out, (CFIndex)out_max, kCFStringEncodingUTF8)) {
        /* Too long for out: keep what fits rather than nothing. */
        CFIndex used = 0;
        CFStringRef s = (CFStringRef)cf;
        CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingUTF8, '?', false,
                         (UInt8 *)out, (CFIndex)out_max - 1, &used);
        out[used] = '\0';
    }
    return 1;
}

/* ---- argument parsing -------------------------------------------------------- */

int xpadctl_parse_hex16(const char *s, uint16_t *out)
{
    char *end;
    unsigned long v;

    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s || *s == '-' || *s == '+') return 0;
    errno = 0;
    v = strtoul(s, &end, 16);
    if (errno || *end || v > 0xFFFFu) return 0;
    *out = (uint16_t)v;
    return 1;
}

int xpadctl_parse_seconds(const char *s, double *out)
{
    char *end;
    double v;

    if (!s || !*s) return 0;
    errno = 0;
    v = strtod(s, &end);
    if (errno || *end || !(v >= 0) || v > 86400.0 * 365) return 0;
    *out = v;
    return 1;
}

int xpadctl_usage_error(const char *cmd, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "xpadctl %s: ", cmd);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n(see: xpadctl help)\n");
    return 2;
}

/* ---- formatting ------------------------------------------------------------------ */

void xpadctl_hex(char *out, size_t out_max, const uint8_t *p, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    size_t k = 0, i;

    if (out_max == 0) return;
    for (i = 0; i < len; i++) {
        /* Room for "xx " plus, if more bytes follow, the "…" (3 UTF-8 bytes) and NUL. */
        if (k + 3 + (i + 1 < len ? 3 : 0) + 1 > out_max) {
            if (k + 4 <= out_max) { memcpy(out + k, "\xe2\x80\xa6", 3); k += 3; }
            break;
        }
        if (i) out[k++] = ' ';
        out[k++] = digits[p[i] >> 4];
        out[k++] = digits[p[i] & 0x0F];
    }
    out[k] = '\0';
}

static const struct { uint32_t bit; const char *name; } button_names[] = {
    { XPAD_BTN_A, "A" },          { XPAD_BTN_B, "B" },
    { XPAD_BTN_X, "X" },          { XPAD_BTN_Y, "Y" },
    { XPAD_BTN_LB, "LB" },        { XPAD_BTN_RB, "RB" },
    { XPAD_BTN_LS, "LS" },        { XPAD_BTN_RS, "RS" },
    { XPAD_BTN_VIEW, "View" },    { XPAD_BTN_MENU, "Menu" },
    { XPAD_BTN_GUIDE, "Xbox" },   { XPAD_BTN_SHARE, "Share" },
    { XPAD_BTN_DPAD_UP, "\xe2\x86\x91" },    /* ↑ */
    { XPAD_BTN_DPAD_DOWN, "\xe2\x86\x93" },  /* ↓ */
    { XPAD_BTN_DPAD_LEFT, "\xe2\x86\x90" },  /* ← */
    { XPAD_BTN_DPAD_RIGHT, "\xe2\x86\x92" }, /* → */
};

void xpadctl_format_state(const xpad_state *s, char *out, size_t out_max)
{
    char btn[128];
    size_t k = 0, i;
    int any = 0;

    btn[0] = '\0';
    for (i = 0; i < sizeof button_names / sizeof button_names[0]; i++) {
        if (!(s->buttons & button_names[i].bit)) continue;
        int n = snprintf(btn + k, sizeof btn - k, "%s%s", any ? " " : "", button_names[i].name);
        if (n < 0 || (size_t)n >= sizeof btn - k) break;
        k += (size_t)n;
        any = 1;
    }
    snprintf(out, out_max, "buttons[%s] LX=%5u LY=%5u RX=%5u RY=%5u LT=%4u RT=%4u",
             any ? btn : "-", s->lx, s->ly, s->rx, s->ry, s->lt, s->rt);
}

static unsigned absdiff(unsigned a, unsigned b) { return a > b ? a - b : b - a; }

int xpadctl_state_changed(const xpad_state *a, const xpad_state *b, unsigned axis_deadband)
{
    return a->buttons != b->buttons || a->lt != b->lt || a->rt != b->rt ||
           absdiff(a->lx, b->lx) > axis_deadband || absdiff(a->ly, b->ly) > axis_deadband ||
           absdiff(a->rx, b->rx) > axis_deadband || absdiff(a->ry, b->ry) > axis_deadband;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int xpadctl_xbox_decode(const uint8_t *rep, size_t len, xpad_state *s)
{
    /* Hat value (1 N, 2 NE, ... 8 NW; 0 none) back to D-pad bits. */
    static const uint32_t hat_bits[9] = {
        0,
        XPAD_BTN_DPAD_UP,
        XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_RIGHT,
        XPAD_BTN_DPAD_RIGHT,
        XPAD_BTN_DPAD_DOWN | XPAD_BTN_DPAD_RIGHT,
        XPAD_BTN_DPAD_DOWN,
        XPAD_BTN_DPAD_DOWN | XPAD_BTN_DPAD_LEFT,
        XPAD_BTN_DPAD_LEFT,
        XPAD_BTN_DPAD_UP | XPAD_BTN_DPAD_LEFT,
    };
    const uint8_t *p;
    uint8_t hat;
    uint32_t b = 0;

    /* With the report ID (as IOHIDDevice hands input reports over) the
     * payload starts at [1]; without it, at [0]. */
    if (len >= XPAD_XBOX_INPUT_LEN && rep[0] == XPAD_XBOX_INPUT_ID) p = rep + 1;
    else if (len == XPAD_XBOX_INPUT_LEN - 1) p = rep;
    else return 0;

    /* p is now the payload: report byte [n] is p[n - 1]. */
    s->lx = le16(p + 0);
    s->ly = le16(p + 2);
    s->rx = le16(p + 4);
    s->ry = le16(p + 6);
    s->lt = le16(p + 8) & 0x03FF;
    s->rt = le16(p + 10) & 0x03FF;

    hat = p[12] & 0x0F;
    if (hat <= 8) b |= hat_bits[hat];
    if (p[13] & 0x01) b |= XPAD_BTN_A;
    if (p[13] & 0x02) b |= XPAD_BTN_B;
    if (p[13] & 0x08) b |= XPAD_BTN_X;
    if (p[13] & 0x10) b |= XPAD_BTN_Y;
    if (p[13] & 0x40) b |= XPAD_BTN_LB;
    if (p[13] & 0x80) b |= XPAD_BTN_RB;
    if (p[14] & 0x04) b |= XPAD_BTN_VIEW;
    if (p[14] & 0x08) b |= XPAD_BTN_MENU;
    if (p[14] & 0x10) b |= XPAD_BTN_GUIDE;
    if (p[14] & 0x20) b |= XPAD_BTN_LS;
    if (p[14] & 0x40) b |= XPAD_BTN_RS;
    if (p[15] & 0x01) b |= XPAD_BTN_SHARE;
    s->buttons = b;
    return 1;
}

/* ---- doctor verdicts --------------------------------------------------------------- */

static unsigned verdict_counts[4];

void xpadctl_section(const char *title)
{
    printf("\n== %s ==\n", title);
}

static void vline(const char *prefix, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));

static void vline(const char *prefix, const char *fmt, va_list ap)
{
    fputs(prefix, stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
}

void xpadctl_verdict(xpadctl_level level, const char *fmt, ...)
{
    static const char *const marks[4] = {
        "\xe2\x9c\x85 ",            /* ✅ */
        "\xe2\x9a\xa0\xef\xb8\x8f  ", /* ⚠️ (plus a space: the variation selector is zero width) */
        "\xe2\x9d\x8c ",            /* ❌ */
        "\xe2\x80\xa2  ",           /* • */
    };
    va_list ap;

    if ((unsigned)level > XPADCTL_INFO) level = XPADCTL_INFO;
    verdict_counts[level]++;
    va_start(ap, fmt);
    vline(marks[level], fmt, ap);
    va_end(ap);
}

void xpadctl_detail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vline("     ", fmt, ap);
    va_end(ap);
}

void xpadctl_hint(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vline("     \xe2\x86\x92 ", fmt, ap);   /* → */
    va_end(ap);
}

unsigned xpadctl_verdict_count(xpadctl_level level)
{
    return (unsigned)level <= XPADCTL_INFO ? verdict_counts[level] : 0;
}
