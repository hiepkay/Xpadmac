/*
 * XPadMac shared core — the contract between the DriverKit driver, the
 * xpadctl tool and the unit tests. Pure C99, no OS headers: it must build
 * for DriverKit (arm64e, DriverKit SDK) and for the host (macOS) alike.
 *
 * Everything a controller sends is reduced to one canonical state
 * (xpad_state), which is then written out as the input report of an
 * "Xbox Wireless Controller" (Series X|S over Bluetooth LE, 045E:0B13,
 * 283-byte unified descriptor). macOS GameController accepts that
 * identity from a third-party driver because its personality carries
 * GCIOMatchVirtual=true, and Apple's XboxOneHIDServicePlugin parses the
 * report at fixed offsets (see docs/DESIGN.md and docs/research/).
 *
 * GPL-3.0-or-later, as the rest of XPadMac (gip.c derives from AnyPad's).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XPAD_H
#define XPAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- canonical controller state ---------------------------------------- */

enum {
    XPAD_BTN_A          = 1u << 0,
    XPAD_BTN_B          = 1u << 1,
    XPAD_BTN_X          = 1u << 2,
    XPAD_BTN_Y          = 1u << 3,
    XPAD_BTN_LB         = 1u << 4,
    XPAD_BTN_RB         = 1u << 5,
    XPAD_BTN_VIEW       = 1u << 6,   /* Back / View */
    XPAD_BTN_MENU       = 1u << 7,   /* Start / Menu */
    XPAD_BTN_GUIDE      = 1u << 8,   /* Xbox button */
    XPAD_BTN_LS         = 1u << 9,   /* left stick click */
    XPAD_BTN_RS         = 1u << 10,  /* right stick click */
    XPAD_BTN_SHARE      = 1u << 11,
    XPAD_BTN_DPAD_UP    = 1u << 12,
    XPAD_BTN_DPAD_DOWN  = 1u << 13,
    XPAD_BTN_DPAD_LEFT  = 1u << 14,
    XPAD_BTN_DPAD_RIGHT = 1u << 15,
};

#define XPAD_AXIS_CENTER  0x8000u    /* sticks: 0..65535, 0 = left / up */
#define XPAD_TRIGGER_MAX  1023u      /* triggers: 0..1023 */

typedef struct {
    uint32_t buttons;                /* XPAD_BTN_* */
    uint16_t lx, ly, rx, ry;         /* 0..65535, centre XPAD_AXIS_CENTER, y: 0 = up */
    uint16_t lt, rt;                 /* 0..XPAD_TRIGGER_MAX */
} xpad_state;

/* Sticks centred, nothing pressed. */
void xpad_state_reset(xpad_state *s);

/* Rumble, as GameController asks it (and as GIP takes it): 0..100 each. */
typedef struct {
    uint8_t strong, weak;            /* left (low freq) / right (high freq) motors */
    uint8_t lt, rt;                  /* trigger impulse motors */
} xpad_rumble;

/* ---- the identity the driver publishes ------------------------------------ */

#define XPAD_XBOX_VID        0x045Eu
#define XPAD_XBOX_PID        0x0B13u            /* Xbox Series X|S, Bluetooth LE */
#define XPAD_XBOX_VERSION    0x0517u
#define XPAD_XBOX_PRODUCT    "Xbox Wireless Controller"
#define XPAD_XBOX_MANUFACTURER "Microsoft"
#define XPAD_XBOX_TRANSPORT  "BluetoothLowEnergy" /* must not be "USB" (Apple's plugin rejects USB) */

#define XPAD_XBOX_DESC_LEN   283u
extern const uint8_t xpad_xbox_desc[XPAD_XBOX_DESC_LEN];   /* CRC-16/ARC 0x931D */

#define XPAD_XBOX_INPUT_ID   0x01u
#define XPAD_XBOX_INPUT_LEN  17u                 /* report ID included */
#define XPAD_XBOX_RUMBLE_ID  0x03u
#define XPAD_XBOX_RUMBLE_LEN 9u                  /* report ID included */

/* Writes the 17-byte input report 1 for *s into out (out_max >= 17).
 * Layout (as Apple's XboxOneHIDServicePlugin reads it, Android layout):
 *  [0] 0x01  [1..8] LX LY RX RY u16 LE  [9..10] LT [11..12] RT u16 LE 0..1023
 *  [13] hat low nibble 0 none,1 N,2 NE,3 E,4 SE,5 S,6 SW,7 W,8 NW
 *  [14] b0 A b1 B b3 X b4 Y b6 LB b7 RB
 *  [15] b2 View b3 Menu b4 Guide b5 LS b6 RS
 *  [16] b0 Share
 * Returns the length written (17), or 0 if out_max is too small. */
size_t xpad_xbox_build_input(const xpad_state *s, uint8_t *out, size_t out_max);

/* Parses an output report GameController sends for rumble: report ID 3,
 * 9 bytes: [1] enable mask (0x0F active; 0xFF or 0 = stop) [2] LT [3] RT
 * [4] strong [5] weak (0..100) [6] duration (10 ms) [7] delay [8] loop.
 * `rep` may start with the report ID (len 9) or not (len 8, then report_id
 * says which). A stop request yields all zeros. Returns 1 if it was a rumble
 * report and *out was set, else 0. */
int xpad_xbox_parse_rumble(uint8_t report_id, const uint8_t *rep, size_t len, xpad_rumble *out);

/* How long a real Xbox Wireless Controller plays a rumble report 3 before
 * it stops by itself, in ms: (on + off) x (loop count + 1) x 10 ms, from
 * [6] on, [7] off, [8] loop count (xpadneo's pulse_sustain_10ms,
 * pulse_release_10ms, loop_count). GameController's 0x0A,0,0 is 100 ms,
 * re-sent while a haptic plays; xpadneo's 0xFF,0,0xEB is about 10 min.
 * The drivers hold a level until the next report, so they stop the motors
 * themselves this long (plus some slack) after the last report: a client
 * that dies mid-haptic must not leave the pad rumbling. Same `rep`/`len`/
 * `report_id` rules as xpad_xbox_parse_rumble. 0 for a stop, a report
 * enabling no motor or with all its enabled magnitudes 0, or one that is
 * not a rumble report. */
uint32_t xpad_xbox_rumble_play_ms(uint8_t report_id, const uint8_t *rep, size_t len);

/* ---- Xbox 360 wired (XInput, interface FF/5D/01) ---------------------------
 *
 * Third-party wired 360 pads, and pads with an XInput mode ("Xbox 360
 * Controller for Windows"). Layout as Linux's xpad.c (XTYPE_XBOX360) and
 * SDL_hidapi_xbox360.c read it. */

#define XPAD_X360_MSG_INPUT    0x00u
#define XPAD_X360_INPUT_LEN    20u      /* 0x14 */
#define XPAD_X360_RUMBLE_LEN   8u
#define XPAD_X360_LED_LEN      3u
#define XPAD_X360_LED_PLAYER1_FLASH 0x02u /* 2..5 flash then on, player 1..4; 6..9 on */

/* The 20-byte input message (type 0x00, length 0x14):
 *  [2] b0 D-pad up b1 down b2 left b3 right b4 Start(Menu) b5 Back(View) b6 LS b7 RS
 *  [3] b0 LB b1 RB b2 Guide b4 A b5 B b6 X b7 Y
 *  [4] LT [5] RT 0..255   [6..13] LX LY RX RY s16 LE (Y up positive)
 * Returns 1 and fills *s only for message type 0x00 with len >= 20; any
 * other message (01 03 LED state, 02 03, 03 03 rumble state, 08 03
 * headset...) or a short one leaves *s alone and returns 0. */
int xpad_x360_parse(const uint8_t *msg, size_t len, xpad_state *s);

/* The rumble message 00 08 00 <left/strong> <right/weak> 00 00 00, levels
 * 0..100 scaled to 0..255 (the trigger motors do not exist on a 360 pad).
 * Returns 8, or 0 if max < 8. */
size_t xpad_x360_build_rumble(const xpad_rumble *r, uint8_t *out, size_t max);

/* The LED message 01 03 <n>. Returns 3, or 0 if max < 3. */
size_t xpad_x360_build_led(uint8_t n, uint8_t *out, size_t max);

/* ---- GIP (Xbox One / Series wired protocol) ------------------------------- */

/* The GIP host state machine is Shared/gip.c (adapted from AnyPad's,
 * GPL-3): see Shared/gip.h. It fills an xpad_state directly with full
 * precision (16-bit sticks, 10-bit triggers) and builds rumble frames from
 * an xpad_rumble. */

/* Scaling helpers, exposed for tests. */
uint16_t xpad_axis_from_u8(uint8_t v);        /* 0x00->0, 0x80->0x8000, 0xFF->0xFFFF, monotonic */
uint16_t xpad_axis_from_s16(int16_t v);       /* -32768->0, 0->0x8000, 32767->0xFFFF */
uint16_t xpad_axis_from_s16_inverted(int16_t v); /* GIP Y (up positive) -> 0 = up */
uint16_t xpad_trigger_from_u8(uint8_t v);     /* 0..255 -> 0..1023 */

#ifdef __cplusplus
}
#endif

#endif
