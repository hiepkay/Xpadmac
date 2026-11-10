/*
 * xpadctl — XPadMac's command-line diagnostic tool (host macOS build).
 *
 * Internal declarations shared by the tool's C files and gc.m. The shared
 * core (Shared/xpad.h, Shared/gip.h) is the contract with the driver; this
 * header only adds what the tool itself needs: command entry points, a
 * monotonic clock, Ctrl-C handling, verdict printing for `doctor`, formatting
 * of an xpad_state, and a decoder for the Xbox input report the driver
 * publishes (the reverse of xpad_xbox_build_input).
 *
 * All user-facing text is English.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPADCTL_H
#define XPADCTL_H

#include <stddef.h>
#include <stdint.h>

#include "xpad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XPADCTL_VERSION "1.0"

/* ---- commands (each returns the process exit status) ---------------------- */

int xpadctl_cmd_doctor(int argc, char **argv);
int xpadctl_cmd_hid(int argc, char **argv);
int xpadctl_cmd_gc(int argc, char **argv);          /* gc.m */
#ifdef XPADCTL_WITH_LIBUSB
int xpadctl_cmd_gip(int argc, char **argv);
#endif

/* `doctor`'s GameController section (gc.m). `xpad_hid_seen` says whether the
 * HID section found a device published by the XPad driver, which changes the
 * verdict when GameController lists nothing. */
void xpadctl_gc_doctor(int xpad_hid_seen);

/* ---- time and stopping ------------------------------------------------------ */

/* Milliseconds on a monotonic clock, counted from the first call. */
long xpadctl_now_ms(void);

/* Installs SIGINT/SIGTERM/SIGHUP handlers that only set a flag (async-signal
 * safe); long-running commands poll xpadctl_should_stop() from their loop. */
void xpadctl_signals_install(void);

/* Arms an optional deadline: after `seconds` (> 0) xpadctl_should_stop()
 * returns 1 as if Ctrl-C had been pressed. 0 or less disarms it. */
void xpadctl_set_deadline(double seconds);

/* 1 once Ctrl-C (or SIGTERM/SIGHUP) arrived or the deadline passed. */
int xpadctl_should_stop(void);

/* Runs the current thread's CFRunLoop (and so the main dispatch queue) for
 * up to `seconds`, returning early if xpadctl_should_stop() becomes true. */
void xpadctl_run_loop_for(double seconds);

/* ---- argument parsing -------------------------------------------------------- */

/* "0e6f", "0x0e6f" or "0X0E6F" (hexadecimal, as VID/PID are always written)
 * into *out. Returns 1 on success, 0 if malformed or above 0xFFFF. */
int xpadctl_parse_hex16(const char *s, uint16_t *out);

/* A non-negative number of seconds ("2", "0.5"). Returns 1 on success. */
int xpadctl_parse_seconds(const char *s, double *out);

/* Prints "xpadctl <cmd>: <message>" to stderr; returns 2 (usage error). */
int xpadctl_usage_error(const char *cmd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- formatting ------------------------------------------------------------------ */

/* "aa bb cc" (lower-case, space separated) into out; always NUL-terminated,
 * truncated with "…" if out is too small. */
void xpadctl_hex(char *out, size_t out_max, const uint8_t *p, size_t len);

/* One line describing *s: pressed buttons, then the raw axis and trigger
 * values, e.g. "buttons[A LB ↑] LX=32768 LY=32768 RX=32768 RY=32768 LT=0 RT=1023". */
void xpadctl_format_state(const xpad_state *s, char *out, size_t out_max);

/* 1 if a and b differ enough to be worth printing again: any button, a
 * trigger, or a stick axis moved by more than `axis_deadband` (raw units). */
int xpadctl_state_changed(const xpad_state *a, const xpad_state *b, unsigned axis_deadband);

/* Decodes the 17-byte Xbox Wireless Controller input report 1 (layout in
 * xpad.h), with or without its leading report ID byte, back into *s.
 * Returns 1 on success, 0 if it is not an input report 1. */
int xpadctl_xbox_decode(const uint8_t *rep, size_t len, xpad_state *s);

/* ---- CoreFoundation helpers (cf is a CFTypeRef, may be NULL) ------------------------ */

/* A CFNumber (or CFBoolean) into *out. Returns 1 if cf was one. */
int xpadctl_cf_number(const void *cf, long long *out);

/* A CFString as UTF-8 into out (NUL-terminated). Returns 1 if cf was one. */
int xpadctl_cf_string(const void *cf, char *out, size_t out_max);

/* Sleeps `ms` milliseconds in short slices, returning early (0) if
 * xpadctl_should_stop() becomes true; 1 if the whole time elapsed. */
int xpadctl_sleep_ms(long ms);

/* ---- doctor verdicts --------------------------------------------------------------- */

typedef enum {
    XPADCTL_OK,      /* ✅ */
    XPADCTL_WARN,    /* ⚠️ */
    XPADCTL_FAIL,    /* ❌ */
    XPADCTL_INFO     /* •  (informational, not counted) */
} xpadctl_level;

void xpadctl_section(const char *title);
void xpadctl_verdict(xpadctl_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* An indented detail line under the previous verdict. */
void xpadctl_detail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* An indented "→ hint" line telling the user what to do. */
void xpadctl_hint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Number of verdicts printed so far at `level`. */
unsigned xpadctl_verdict_count(xpadctl_level level);

/* ---- host logging ------------------------------------------------------------------ */

/* xpad_log (declared in gip.h) prints to stderr with a timestamp; this turns
 * it off (`gip --quiet`). */
void xpadctl_log_set_enabled(int enabled);

#ifdef __cplusplus
}
#endif

#endif
