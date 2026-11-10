# Research files

Reference material kept next to the code. Nothing here is built into the app or the driver; `Tests/` reads some of it. Unless stated otherwise, everything here is part of XPadMac and is licensed **GPL-3.0-or-later** (see [`LICENSE`](../../LICENSE) and [`NOTICE`](../../NOTICE)).

| File | What it is | Origin and license |
| --- | --- | --- |
| `anypad_gip.c.orig`, `anypad_gip.h.orig` | The GIP host module `Shared/gip.c` and `Shared/gip.h` were adapted from, kept verbatim for comparison. | A module (`src/gip.c`, `src/gip.h`) written by an XPadMac contributor on top of [AnyPad PS4](https://github.com/Vld439/AnyPad-PS4) (a PS4 port of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by elmonomalvad0) and never published in that project. GPL-3.0-or-later, like AnyPad. |
| `anypad_pad.h.orig` | The `src/pad.h` the module above was written against, kept verbatim. | AnyPad PS4's `src/pad.h` (AnyPad PS5 by elmonomalvad0; GPL-3.0-or-later), plus the `PAD_EXTRA` buttons added by the same contributor. |
| `probe4_userspace_gip.c` | The libusb probe (not built) that recorded `gip_bringup_capture.log`; it uses the module's API. `Tools/xpadctl/gip_cmd.c` follows its USB side. | Written for XPadMac. GPL-3.0-or-later. |
| `gip_bringup_capture.log` | The log of one real GIP bring-up against a third-party dock (VID `3537`). The pad's 6-byte device ID is replaced by `00 11 22 33 44 55`. `Tests/test_gip.c` replays it. | Recorded for XPadMac. |
| `xbox_series_ble_0b13_descriptor.bin` | The 283-byte HID report descriptor of an Xbox Wireless Controller (`045E:0B13`) over Bluetooth LE. `Tests/gen_xbox_desc.py` builds `Shared/xpad_xbox_desc.c` from it. | The controller's own descriptor, byte for byte (CRC-16/ARC `0x931D`, the same as the one [xpadneo](https://github.com/atar-axis/xpadneo) documents). Factual data describing Microsoft's hardware. |
| `xbox_series_ble_0b13_descriptor_commented.c` | The same descriptor, decoded item by item; its comments go into `Shared/xpad_xbox_desc.c`. | Written for XPadMac. GPL-3.0-or-later. |
| `xbox_hid_format.txt` | Notes on how macOS GameController matches and parses an Xbox controller, and on the DriverKit entitlements. | Written for XPadMac. GPL-3.0-or-later. |

The `.orig` files are left exactly as they were, without a license header, so that they can be compared with `Shared/gip.c` and `Shared/gip.h`. This table is their license notice.
