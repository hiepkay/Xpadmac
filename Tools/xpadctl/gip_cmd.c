/*
 * xpadctl gip — a userspace GIP host on libusb (built only WITH_LIBUSB=1).
 *
 * The USB side follows docs/research/probe4_userspace_gip.c, the probe that
 * first brought a third-party GIP pad up on macOS; the protocol is
 * Shared/gip.c, the same state machine the driver runs, and the decoded
 * xpad_state is printed whenever it changes.
 *
 * Why the IN transfers are asynchronous and never cancelled: on macOS,
 * libusb's darwin backend aborts the pipe *and clears the endpoint halt*
 * (ClearPipeStallBothEnds -> CLEAR_FEATURE(ENDPOINT_HALT), which also resets
 * the data toggle) whenever a transfer is cancelled — and a synchronous read
 * that times out is a cancelled transfer. GIP pads react badly to that (they
 * drop frames or re-enumerate). So four interrupt-IN transfers with no
 * timeout stay queued all the time and are resubmitted from their callback;
 * the only cancel is at teardown, when we are leaving anyway. The driver
 * follows the same rule (DESIGN.md: "Never cancel or time out the reads").
 *
 * The one exception, as in the driver: a read that fails (stall, overflow,
 * transaction error) leaves the interrupt endpoint halted, and nothing more
 * arrives on it, for any queued transfer, until the halt is cleared. Then
 * the failed reads are parked, the main loop clears the halt (that also
 * aborts the reads still queued, which are parked as well) and queues them
 * all again, at most MAX_HALT_CLEARS times in a row.
 *
 * This command sends traffic to the pad (SET_CONFIGURATION, the GIP
 * bring-up, acknowledgements): it is a test tool, not something `doctor` runs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef XPADCTL_WITH_LIBUSB

#include "gip.h"
#include "xpadctl.h"

#include <libusb-1.0/libusb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_TRANSFERS        4       /* as probe4: enough that the pad never waits for a buffer */
#define IN_BUF_LEN          64      /* GIP max packet size */
#define OUT_TIMEOUT_MS      1000    /* see snd() */
#define WAIT_POLL_MS        2       /* how often to look for the pad while waiting */
#define RAW_FRAMES_DEFAULT  25      /* raw frames printed before only decoded state */
#define AXIS_DEADBAND       256     /* raw stick units (of 65535) worth a new line */
#define MAX_HALT_CLEARS     8       /* IN halt clears in a row (as the driver) before giving up */

typedef struct {
    int have_vid, have_pid;
    uint16_t vid, pid;
    double wait_s, run_s;
    int windows_desc, opts, raw_all, quiet;
} gip_args;

static struct {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint8_t ep_in, ep_out;
    int iface;
    gip_t g;
    xpad_state st, shown;
    int have_shown;
    int gone, closing;
    int read_failed;                /* the reads kept failing: stopped (the pad may still be there) */
    int in_pending, out_pending;
    int need_clear;                 /* a read failed: the IN endpoint is halted */
    int clears_in_row;              /* halt clears since the last good read */
    unsigned long frames, tx_failed;
    int raw_left;                   /* -1: every frame */
    struct libusb_transfer *in[IN_TRANSFERS];
    struct libusb_transfer *parked[IN_TRANSFERS];   /* reads waiting to be queued again */
    int n_parked;
} U;

/* ---- finding the pad ------------------------------------------------------- */

static const uint16_t gip_vendors[] = { 0x045E, 0x2DC8, 0x3537, 0x0F0D, 0x2E24, 0x0738,
                                        0x0E6F, 0x24C6, 0x20D6, 0x1532, 0x10F5, 0x366C };

static int candidate_ids(const gip_args *a, uint16_t vid, uint16_t pid)
{
    size_t i;

    if (a->have_vid) return vid == a->vid && (!a->have_pid || pid == a->pid);
    /* Apple's XboxGamepad.dext already owns these: leave them alone. */
    if (vid == 0x045E && (pid == 0x028E || pid == 0x02EA || pid == 0x0B00 || pid == 0x0B12)) return 0;
    for (i = 0; i < sizeof gip_vendors / sizeof gip_vendors[0]; i++)
        if (gip_vendors[i] == vid) return 1;
    return 0;
}

typedef struct {
    libusb_device *dev;
    uint16_t vid, pid;
    uint8_t config_value;
    int iface;
    uint8_t ep_in, ep_out;
} gip_target;

/* The GIP interface (FF/47/D0, number 0 preferred) of configuration 0 and its
 * interrupt endpoints. libusb reads the descriptor macOS cached at
 * enumeration: no traffic to the device. Returns 1 if found. */
static int find_gip_interface(libusb_device *dev, gip_target *t)
{
    struct libusb_config_descriptor *cfg;
    int found = 0, i;

    if (libusb_get_config_descriptor(dev, 0, &cfg) != 0) return 0;
    for (i = 0; i < cfg->bNumInterfaces && found != 2; i++) {
        const struct libusb_interface_descriptor *alt;
        uint8_t in = 0, out = 0;
        int e;

        if (cfg->interface[i].num_altsetting < 1) continue;
        alt = &cfg->interface[i].altsetting[0];
        if (alt->bInterfaceClass != 0xFF || alt->bInterfaceSubClass != 0x47 || alt->bInterfaceProtocol != 0xD0)
            continue;
        for (e = 0; e < alt->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_INTERRUPT) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) { if (!in) in = ep->bEndpointAddress; }
            else if (!out) out = ep->bEndpointAddress;
        }
        if (!in || !out) continue;
        t->config_value = cfg->bConfigurationValue;
        t->iface = alt->bInterfaceNumber;
        t->ep_in = in;
        t->ep_out = out;
        found = alt->bInterfaceNumber == 0 ? 2 : 1;     /* 2: the one we want, stop looking */
    }
    libusb_free_config_descriptor(cfg);
    return found != 0;
}

/* Looks once through the bus; on success t->dev holds a reference. */
static int find_target(const gip_args *a, gip_target *t)
{
    libusb_device **list;
    ssize_t n, i;
    int ok = 0;

    n = libusb_get_device_list(U.ctx, &list);
    if (n < 0) return 0;
    for (i = 0; i < n && !ok; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (!candidate_ids(a, dd.idVendor, dd.idProduct)) continue;
        memset(t, 0, sizeof *t);
        if (!find_gip_interface(list[i], t)) continue;
        t->dev = libusb_ref_device(list[i]);
        t->vid = dd.idVendor;
        t->pid = dd.idProduct;
        ok = 1;
    }
    libusb_free_device_list(list, 1);
    return ok;
}

/* ---- moving bytes ------------------------------------------------------------ */

static void hexlog(const char *what, const uint8_t *p, int n)
{
    char h[3 * IN_BUF_LEN + 8];

    xpadctl_hex(h, sizeof h, p, n > 0 ? (size_t)n : 0);
    xpad_log("%s (%d): %s", what, n, h);
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *t)
{
    U.out_pending--;
    if (t->status != LIBUSB_TRANSFER_COMPLETED) {
        U.tx_failed++;
        xpad_log("tx failed (status %d)", t->status);
    }
    /* LIBUSB_TRANSFER_FREE_BUFFER | FREE_TRANSFER: libusb frees both. */
}

/* gip_send_fn: one frame to the OUT endpoint, queued in order. A copy is
 * made because gip.c reuses its buffer. The 1 s timeout (as probe4) is never
 * reached while the pad is listening — it takes a frame within a few ms — so
 * the cancel-clears-halt problem above does not arise for writes in practice. */
static int snd(void *ctx, const uint8_t *p, int len)
{
    struct libusb_transfer *t;
    uint8_t *b;

    (void)ctx;
    if (U.gone || U.closing || len <= 0) return 0;
    t = libusb_alloc_transfer(0);
    b = malloc((size_t)len);
    if (!t || !b) { libusb_free_transfer(t); free(b); return 0; }
    memcpy(b, p, (size_t)len);
    libusb_fill_interrupt_transfer(t, U.h, U.ep_out, b, len, out_cb, NULL, OUT_TIMEOUT_MS);
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
    if (libusb_submit_transfer(t) != 0) {
        libusb_free_transfer(t);        /* frees b too (FREE_BUFFER) */
        return 0;
    }
    U.out_pending++;
    return 1;
}

static void on_frame(const uint8_t *buf, int len)
{
    long now = xpadctl_now_ms();
    int r;

    U.frames++;
    if (U.raw_left != 0) {
        hexlog("rx", buf, len);
        if (U.raw_left > 0 && --U.raw_left == 0)
            xpad_log("(printed the first %d raw frames; --raw prints all)", RAW_FRAMES_DEFAULT);
    }
    r = gip_receive(&U.g, buf, len, now, &U.st, snd, NULL);
    if ((r & GIP_RX_STATE) && (!U.have_shown || xpadctl_state_changed(&U.st, &U.shown, AXIS_DEADBAND))) {
        char line[160];
        xpadctl_format_state(&U.st, line, sizeof line);
        printf("[%7.3f] %s\n", now / 1000.0, line);
        U.shown = U.st;
        U.have_shown = 1;
    }
}

static void park(struct libusb_transfer *t)
{
    if (U.n_parked < IN_TRANSFERS) U.parked[U.n_parked++] = t;
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *t)
{
    U.in_pending--;
    if (U.closing) return;              /* teardown: drain() cancelled it */
    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        U.clears_in_row = 0;
        on_frame(t->buffer, t->actual_length);
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        xpad_log("rx status %d: device gone", t->status);
        U.gone = 1;
        return;
    case LIBUSB_TRANSFER_CANCELLED:
        /* Not teardown: our own libusb_clear_halt aborts every read still
         * queued on the endpoint. Queued again by recover_reads(). */
        park(t);
        return;
    default:
        /* Stall, overflow, or an error: a transaction error on a pad that is
         * still there, or the pad leaving (when the pad re-enumerates, macOS
         * reports LIBUSB_TRANSFER_ERROR; the clear then fails and says so).
         * The endpoint is halted: resubmitting now would only fail again.
         * The halt is cleared from the main loop, as libusb_clear_halt is
         * synchronous and must not run inside a callback. */
        xpad_log("rx status %d", t->status);
        park(t);
        U.need_clear = 1;
        return;
    }
    if (U.gone) return;
    if (libusb_submit_transfer(t) == 0) U.in_pending++;
    else U.gone = 1;
}

/* From the main loop: clears the IN endpoint's halt after a failed read,
 * then queues the parked reads again. The reads that clear aborts come back
 * cancelled on a later pass and are queued again then, with no new clear. */
static void recover_reads(void)
{
    int i, n, r;

    if (U.need_clear) {
        U.need_clear = 0;
        if (++U.clears_in_row > MAX_HALT_CLEARS) {
            xpad_log("reads failed %d times in a row despite clearing the halt, stopping", MAX_HALT_CLEARS + 1);
            U.read_failed = 1;
            return;
        }
        r = libusb_clear_halt(U.h, U.ep_in);
        xpad_log("clear halt IN %02x -> %s", U.ep_in, libusb_error_name(r));
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_NOT_FOUND) {
            xpad_log("device gone");
            U.gone = 1;
            return;
        }
    }
    n = U.n_parked;
    U.n_parked = 0;
    for (i = 0; i < n; i++) {
        if (libusb_submit_transfer(U.parked[i]) == 0) {
            U.in_pending++;
        } else {
            xpad_log("could not resubmit a read: device gone");
            U.gone = 1;
        }
    }
}

/* ---- the optional Windows probes ---------------------------------------------- */

/* Before SET_CONFIGURATION, ask what Windows asks: the Microsoft OS string
 * descriptor (index 0xEE) and, if it answers "MSFT100", the extended compat
 * ID through its vendor code. Some pads pick their mode from this. */
static void windows_descriptors(libusb_device_handle *h)
{
    static const uint8_t sig[14] = { 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0 };
    uint8_t os[18] = { 0 }, c[64] = { 0 };
    int r, len;

    r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                                (LIBUSB_DT_STRING << 8) | 0xEE, 0, os, sizeof os, 500);
    if (r < 18 || memcmp(os + 2, sig, sizeof sig) != 0) {
        xpad_log("string 0xEE -> %s (no MSFT100)", r < 0 ? libusb_error_name(r) : "short/other");
        return;
    }
    xpad_log("string 0xEE -> MSFT100, vendor code %02x", os[16]);
    r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, os[16], 0, 4, c, 16, 500);
    len = r >= 4 ? (int)(c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24) : 0;
    if (len > 16 && len <= (int)sizeof c)
        r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, os[16], 0, 4, c,
                                    (uint16_t)len, 500);
    if (r >= 26) xpad_log("compat ID -> %d bytes (%.8s)", r, (const char *)c + 18);
    else xpad_log("compat ID -> %s", r < 0 ? libusb_error_name(r) : "short");
}

/* ---- the command ----------------------------------------------------------------- */

static void gip_usage(void)
{
    printf("xpadctl gip [options] — userspace GIP host (libusb), prints the decoded state.\n"
           "  --vid V [--pid P]  Only this device (hexadecimal). Default: any GIP controller (interface\n"
           "                     FF/47/D0) from a known vendor, except the PIDs Apple already supports.\n"
           "  --wait N           Wait up to N seconds for the controller to appear (default 180).\n"
           "  --seconds N        Run for N seconds, then exit (default: until Ctrl-C).\n"
           "  --windows-desc     Send the Windows-style descriptor requests (0xEE, compat ID) before configuring.\n"
           "  --blind            Start the controller right away, without waiting for Hello.\n"
           "  --keepalive        Resend START/LED/security every 20 seconds.\n"
           "  --auth             Real GIP authentication (as xone) for every vendor (default for VID 3537).\n"
           "  --fake-auth        No authentication: only send \"security complete\".\n"
           "  --serial           Ask for the serial number (1E 20 SS 01 04) after the security step, as xone.\n"
           "  --raw              Print every frame received (default: the first %d).\n"
           "  --quiet            Do not print gip.c's log.\n"
           "Note: this command sends data to the controller (SET_CONFIGURATION, GIP bring-up). If the\n"
           "XPadMac driver holds the controller, libusb cannot claim the interface.\n",
           RAW_FRAMES_DEFAULT);
}

static int parse_gip_args(int argc, char **argv, gip_args *a)
{
    int i;

    memset(a, 0, sizeof *a);
    a->wait_s = 180;
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--help") || !strcmp(s, "-h")) { gip_usage(); return -1; }
        if (!strcmp(s, "--vid") || !strcmp(s, "--pid")) {
            uint16_t v;
            if (i + 1 >= argc || !xpadctl_parse_hex16(argv[i + 1], &v))
                return xpadctl_usage_error("gip", "%s needs a hexadecimal value (0..FFFF)", s);
            i++;
            if (s[2] == 'v') { a->vid = v; a->have_vid = 1; }
            else { a->pid = v; a->have_pid = 1; }
        } else if (!strcmp(s, "--wait") || !strcmp(s, "--seconds")) {
            double v;
            if (i + 1 >= argc || !xpadctl_parse_seconds(argv[i + 1], &v))
                return xpadctl_usage_error("gip", "%s needs a non-negative number of seconds", s);
            i++;
            if (s[2] == 'w') a->wait_s = v;
            else a->run_s = v;
        } else if (!strcmp(s, "--windows-desc")) a->windows_desc = 1;
        else if (!strcmp(s, "--blind")) a->opts |= GIP_OPT_BLIND;
        else if (!strcmp(s, "--keepalive")) a->opts |= GIP_OPT_KEEPALIVE;
        else if (!strcmp(s, "--auth")) a->opts |= GIP_OPT_AUTH;
        else if (!strcmp(s, "--fake-auth")) a->opts |= GIP_OPT_FAKE_AUTH;
        else if (!strcmp(s, "--serial")) a->opts |= GIP_OPT_SERIAL;
        else if (!strcmp(s, "--raw")) a->raw_all = 1;
        else if (!strcmp(s, "--quiet")) a->quiet = 1;
        else return xpadctl_usage_error("gip", "unknown option: %s", s);
    }
    if (a->have_pid && !a->have_vid) return xpadctl_usage_error("gip", "--pid needs --vid");
    return 0;
}

/* Cancels what is still queued (teardown only) and lets the callbacks run. */
static void drain(void)
{
    long end = xpadctl_now_ms() + 500;
    int i;

    U.closing = 1;
    for (i = 0; i < IN_TRANSFERS; i++)
        if (U.in[i]) libusb_cancel_transfer(U.in[i]);       /* harmless if already done */
    while ((U.in_pending > 0 || U.out_pending > 0) && xpadctl_now_ms() < end) {
        struct timeval tv = { 0, 10000 };
        libusb_handle_events_timeout_completed(U.ctx, &tv, NULL);
    }
}

int xpadctl_cmd_gip(int argc, char **argv)
{
    static char name[16];
    gip_args a;
    gip_target t;
    long t_wait, t_open;
    int rc, cur = -1, r, i, found = 0;

    if ((rc = parse_gip_args(argc, argv, &a))) return rc < 0 ? 0 : rc;
    memset(&U, 0, sizeof U);
    U.raw_left = a.raw_all ? -1 : RAW_FRAMES_DEFAULT;
    xpadctl_log_set_enabled(!a.quiet);

    if ((r = libusb_init(&U.ctx)) != 0) {
        fprintf(stderr, "xpadctl gip: libusb_init failed: %s\n", libusb_error_name(r));
        return 1;
    }

    if (a.have_vid && a.have_pid) printf("Waiting for GIP controller %04x:%04x", a.vid, a.pid);
    else if (a.have_vid) printf("Waiting for a GIP controller from vendor %04x", a.vid);
    else printf("Waiting for a GIP controller (interface FF/47/D0)");
    printf(" — up to %.0f seconds. Plug in (or replug) the controller now.\n", a.wait_s);

    /* Poll fast: some third-party pads fall back to a HID mode within about a
     * second of being plugged in, so the window to grab them is short (probe4
     * polled every 2 ms). */
    t_wait = xpadctl_now_ms();
    while (!xpadctl_should_stop() && xpadctl_now_ms() - t_wait <= (long)(a.wait_s * 1000)) {
        if ((found = find_target(&a, &t))) break;
        xpadctl_sleep_ms(WAIT_POLL_MS);
    }
    if (!found) {
        printf("No GIP controller found.\n");
        libusb_exit(U.ctx);
        return 1;
    }

    r = libusb_open(t.dev, &U.h);
    libusb_unref_device(t.dev);
    if (r != 0) {
        fprintf(stderr, "xpadctl gip: opening %04x:%04x failed: %s\n", t.vid, t.pid, libusb_error_name(r));
        libusb_exit(U.ctx);
        return 1;
    }
    t_open = xpadctl_now_ms();
    U.ep_in = t.ep_in;
    U.ep_out = t.ep_out;
    U.iface = t.iface;
    libusb_get_configuration(U.h, &cur);
    xpad_log("opened %04x:%04x, current configuration %d; GIP: interface %d, IN %02x, OUT %02x", t.vid, t.pid, cur,
             t.iface, t.ep_in, t.ep_out);

    if (a.windows_desc) windows_descriptors(U.h);
    if (cur != t.config_value)
        xpad_log("set_configuration(%d) -> %s", t.config_value,
                 libusb_error_name(libusb_set_configuration(U.h, t.config_value)));
    r = libusb_claim_interface(U.h, U.iface);
    xpad_log("claim interface %d -> %s", U.iface, libusb_error_name(r));
    if (r != 0) {
        if (r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_BUSY)
            fprintf(stderr, "Another driver holds the interface (possibly the XPadMac driver). "
                            "While the driver runs, use `xpadctl gc` or `xpadctl hid` instead of this command.\n");
        libusb_close(U.h);
        libusb_exit(U.ctx);
        return 1;
    }

    snprintf(name, sizeof name, "%04x:%04x", t.vid, t.pid);
    xpad_state_reset(&U.st);
    gip_init(&U.g, name, t.vid, t.pid, a.opts, xpadctl_now_ms());

    /* The reads go in before anything is written (gip.c waits for g.reads):
     * a pad's first frames must not find the host without a buffer. */
    for (i = 0; i < IN_TRANSFERS; i++) {
        uint8_t *b = malloc(IN_BUF_LEN);
        U.in[i] = libusb_alloc_transfer(0);
        if (!U.in[i] || !b) { free(b); xpad_log("out of memory"); continue; }
        /* Timeout 0: never times out, so never cancelled (see the top). */
        libusb_fill_interrupt_transfer(U.in[i], U.h, U.ep_in, b, IN_BUF_LEN, in_cb, NULL, 0);
        U.in[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        if (libusb_submit_transfer(U.in[i]) == 0) U.in_pending++;
        else xpad_log("submit IN %d failed", i);
    }
    if (U.in_pending == 0) {
        fprintf(stderr, "xpadctl gip: could not submit any read.\n");
        U.gone = 1;
    }
    U.g.reads = 1;

    xpadctl_set_deadline(a.run_s);
    while (!U.gone && !U.read_failed && !xpadctl_should_stop()) {
        struct timeval tv = { 0, 10000 };       /* gip_tick wants 10..50 ms */
        libusb_handle_events_timeout_completed(U.ctx, &tv, NULL);
        if (U.need_clear || U.n_parked) recover_reads();
        if (U.gone || U.read_failed) break;
        gip_tick(&U.g, xpadctl_now_ms(), snd, NULL);
    }

    printf("\nRESULT: %s after %ld ms; %lu frames, state %s, hello %lu, status %lu, input %lu, ack %lu%s\n",
           U.gone ? "DEVICE GONE" : U.read_failed ? "STOPPED ON READ ERRORS" : "still connected",
           xpadctl_now_ms() - t_open, U.frames,
           gip_state_name(U.g.state), U.g.hellos, U.g.statuses, U.g.inputs, U.g.acks,
           U.tx_failed ? ", some sends failed" : "");
    if (U.gone)
        printf("Hint: some third-party controllers fall back to a HID mode after leaving GIP — check with `xpadctl doctor`.\n");
    rc = (U.g.inputs || U.g.statuses) ? 0 : 1;
    if (!rc) printf("SUCCESS: the controller answered the GIP protocol.\n");

    if (U.gone) gip_lost(&U.g, xpadctl_now_ms());
    drain();
    if (U.in_pending == 0) {
        for (i = 0; i < IN_TRANSFERS; i++)
            if (U.in[i]) libusb_free_transfer(U.in[i]);     /* frees the buffer too */
    } /* else: still owned by libusb; freeing them would be a use-after-free, so leak at exit */
    if (!U.gone) libusb_release_interface(U.h, U.iface);
    libusb_close(U.h);
    libusb_exit(U.ctx);
    return rc;
}

#endif /* XPADCTL_WITH_LIBUSB */
