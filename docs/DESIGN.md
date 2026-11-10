# XPadMac — design

Make Xbox One / Series (GIP) controllers and third-party Xbox 360 (XInput) controllers that macOS ignores show up on macOS 27 (Apple Silicon, SIP on) as an **Xbox Wireless Controller**. Then GameController-framework games, System Settings › Game Controllers, Safari and IOHID apps all see a standard Xbox pad.

## Why macOS ignores these pads (measured on macOS 27)

- Apple's `/System/Library/DriverExtensions/XboxGamepad.dext` matches only `045E:028E/02EA/0B00/0B12`. A GIP pad with another VID or PID gets no driver, no configuration and no interfaces. A third-party Xbox 360 pad gets its interfaces (it is usually a composite device) but no driver for its XInput interface (FF/5D/01).
- Some third-party pads fall back to a plain HID gamepad mode when no GIP host answers them. macOS's generic HID driver takes those, but **GameController never lists them**: they are not among Apple's known controllers. XPadMac does not drive such fallback modes; it keeps a pad in GIP by answering it.
- Virtual HID (`IOHIDUserDevice`) needs the restricted entitlement `com.apple.developer.hid.virtual.device`. Apple must approve it. An unentitled or ad-hoc binary gets SIGKILLed.
- **DriverKit "Development" entitlements are free** for any paid team. A development-signed dext activates with SIP on, provided:
  - the app is in /Applications;
  - the profile lists the Mac's Provisioning UDID (System Information › Hardware);
  - the user approves it.
- In the kernel, `AppleGCHIDUserEventDriver::probe` lets a non-Apple dext's HID device match only personalities with `GCIOMatchVirtual=true`. Those are the Bluetooth Xbox IDs: `045E:0B13/0B23/0B4F`, `02E0/02FD/0B20`, and the Elite BT IDs.
- Apple's `XboxOneHIDServicePlugin` parses the report at fixed offsets. It requires `VendorID == 0x045E`, a `Transport` other than `"USB"`, and a PID in its list.
- **So the driver publishes `045E:0B13`, Transport `BluetoothLowEnergy`, the real 283-byte BLE descriptor, 17-byte input report 1 and 9-byte rumble output report 3.**

The evidence is in `docs/research/`. Sources: kernelcache disassembly, Apple DTS forum threads, Linux xpad/xone, SDL and xpadneo.

## Layout and ownership

```
XPadMac/
  project.yml              xcodegen spec: app + dext (+ embeds dext in app)
  Config.xcconfig          DEVELOPMENT_TEAM (empty), BUNDLE_ID_PREFIX (com.example.), versions;
                           includes the git-ignored Config.local.xcconfig
  Makefile                 test / tool / project / build-unsigned / build / install / uninstall
  README.md, README.vi.md  user guide (English, Vietnamese)
  LICENSE, NOTICE          GPL-3.0-or-later; credits (AnyPad, xone, Linux xpad, SDL, xpadneo)
  docs/DESIGN.md           this file;  docs/research/  evidence
  Shared/                  pure C99, builds for DriverKit and host
    xpad.h  gip.h          the contract between the driver, xpadctl and the tests
    xpad_core.c            state reset, scaling, xpad_xbox_build_input, xpad_xbox_parse_rumble
    xpad_xbox_desc.c       const uint8_t xpad_xbox_desc[283] (verbatim from docs/research/xbox_series_ble_0b13_descriptor.bin)
    xpad_x360.c            xpad_x360_parse / _build_rumble / _build_led: Xbox 360 wired (XInput) messages
    gip.c                  adapted from docs/research/anypad_gip.c.orig
    gip_auth.c/.h          GIP authentication, the host's side (from xone's auth/auth.c, GPL-2.0-or-later)
    gip_crypto.c/.h        SHA-256, HMAC, xone's PRF, RSA PKCS#1 v1.5, P-256 ECDH (pure C99)
  Driver/                  DriverKit extension "XPadDriver"
    XPadGIPDevice.iig/.cpp    IOService on IOUSBHostDevice: sets configuration for GIP pads
    XPadGIPGamepad.iig/.cpp   IOUserHIDDevice on IOUSBHostInterface FF/47/D0: GIP host
    XPadGIPAudio.iig/.cpp     IOService on the GIP audio interface (1): SET_INTERFACE(1, 0), as xone
    XPad360Gamepad.iig/.cpp   IOUserHIDDevice on IOUSBHostInterface FF/5D/01: Xbox 360 (XInput) pads
    XPadCommon.cpp/.h         identity, report and teardown helpers the classes share
    XPadLog.cpp               xpad_log() -> os_log; xpad_random() -> arc4random_buf
    Info.plist                IOKitPersonalities (below)
    XPadDriver.entitlements   development entitlements (below)
  App/                     SwiftUI host app "XPadMac"
  Tools/xpadctl/           C + one Objective-C file, built by the Makefile
  Tools/resign-app.py      re-signs the app with a Developer ID profile (APP_DEVID_PROFILE)
  Tests/                   host unit tests in C, `make test`
```

Bundle IDs: app `$(BUNDLE_ID_PREFIX)xpadmac`, dext `$(BUNDLE_ID_PREFIX)xpadmac.driver`. `Config.xcconfig` sets `BUNDLE_ID_PREFIX` to the placeholder `com.example.` and leaves `DEVELOPMENT_TEAM` empty; each developer puts their own in `Config.local.xcconfig` (git-ignored) or passes `make build TEAM=... PREFIX=...`. The Makefile reads both files, and `make build` stops at once when no team, or the placeholder prefix, is set.

## Shared core (C99)

- No OS headers beyond `<stdint.h> <stddef.h> <string.h> <stdarg.h> <stdio.h>`, all present in the DriverKit SDK (`DriverKit.sdk/System/DriverKit/usr/include`). `stdio` is used only for `snprintf`/`vsnprintf` in the log helpers. `Tests/driverkit_check.sh` (part of `make test`) builds `Shared/*.c` for DriverKit to keep it so.
- No heap, no globals except const tables.
- Two hooks are implemented per platform: `xpad_log` and `xpad_random` (the driver and xpadctl: `arc4random_buf`, which DriverKit's libSystem exports; the tests: a deterministic source). `Tests/driverkit_check.sh` allows exactly these two undefined symbols beyond libSystem.
- `xpad_xbox_build_input`: the layout is in `xpad.h`. The hat comes from the D-pad bits: U=1, U+R=2, R=3, D+R=4, D=5, D+L=6, L=7, U+L=8, none=0. Opposing bits cancel per axis (as Linux xpad's hat axes do): U+D and L+R give 0, U+D+L gives W=7.
- `gip.c`: port the AnyPad state machine. Keep:
  - the bring-up order and vendor quirks;
  - the acks and chunking;
  - `waits_for_hello` (a vendor quirk: VID 0x3537 receivers and 366C:0005);
  - module filtering.

  Change:
  - **Identify before the bring-up.** A Hello is answered with `04 20 SS 00` (seq_sys), as SDL (`UpdateInitState`: ANNOUNCED → IDENTIFYING, `CONTROLLER_IDENTIFY_TIMEOUT_MS` 100), xone and Windows do; AnyPad and Linux xpad skip it, and the capture shows a third-party dock leaving GIP 1.35 s after AnyPad's bring-up alone. The burst (START, LED, security, rumble stop, unchanged and in the same order) follows the end of the pad's 0x04 metadata reply (a whole frame, or the empty chunk that closes a chunked one), 100 ms without any part of it, or 500 ms after the request. While identifying the state is `GIP_WAIT_UP` with `bursts == 0`; status, Guide and input are taken in but don't make the pad ACTIVE before the burst has gone out. Blind bring-ups (no Hello) don't identify. That dock still left GIP (for its fallback HID mode) about 1.2 s after ACTIVE with identify + fake "security complete", while xone (real authentication) keeps it; hence the next item.
  - **Authentication (0x06), as xone does it** (`gip_auth.c`; xone `auth/auth.c`, `auth/crypto.c`, framing from `bus/protocol.c`). By default for VID 0x3537 (a vendor quirk: those pads need it to stay in GIP) and for any pad with `GIP_OPT_AUTH`; `GIP_OPT_FAKE_AUTH` turns it off. The v1 Host Hello (`06 30 SS 3A` + 58 bytes) takes the place of the fake `06 20 SS 02 01 00` in the burst (after the LED, before the rumble stop, as xone's probe). The rest is driven by the pad:
    - v1: ack → request 02 → Client Hello → request 03 → Certificate (the first DER RSAPublicKey with a 2048-bit modulus in it) → Host Secret 05 (PKCS#1 v1.5 of a random 48-byte PMS, 274 bytes, chunked) → ack → Host Finish 07 → ack → request 08 → Client Finish, checked → `06 20 SS 02 01 00`.
    - v2 (the pad answers request 02 with another data command): transcript reset, Host Hello 21, request 22, 23, 24, Host Public Key 25 (P-256, 82 bytes, chunked), Host Finish 26, request 27, Client Finish 27, checked → complete.
    - Master secret = PRF(PMS, or SHA-256 of the ECDH X, "Master Secret", random_host ‖ random_client, 48); PRF = TLS 1.2 P_SHA256; Host/Device Finished = PRF(master, "Host Finished"/"Device Finished", transcript snapshot, 32). The transcript is SHA-256 over every host handshake message from byte 6 to the end of its payload (no 8-byte trailer) and every accepted pad data message from byte 6 on; requests, acks and "complete" are not hashed; a pad message is hashed before the reply to it is built. The session key ("EXPORTER DAWN ...") is not derived: xone's wired transport never uses it.
    - Framing: host messages over 58 bytes go as xone's `gip_send_pkt`: `06 F0 SS 3A <total>`, then on the pad's ack (`01 20 .. 09 | 00 06 20 got..`) the rest from `got` (≤ 5 chunks, last `B0`), then on the ack of the whole an empty `06 A0 SS 00 <total>`; every chunk keeps the first one's sequence (`seq_sec`). Pad messages are reassembled (at most 1088 bytes; larger or out-of-range chunks dropped). Chunked 0x06 frames are acked by xone's rule (asked, or the last chunk with data; never the empty one); other commands keep the old rule.
    - Fallback: on any failure (error byte, refusal, no key, point off the curve, Finished mismatch) or 2 s without a word on the handshake, it logs why and sends the plain `06 20 SS 02 01 00`; the pad then gets a whole `T_UP` (2 s) from that frame before another bring-up, and the bring-ups after a fallback send the plain "complete" at once (`auth_skip`) until the pad identifies itself again (a Hello in wait-hello or silent, or a re-announce while active, which try the handshake afresh). The cost for a pad that passes with fake auth but ignores the handshake is the 2 s wait for its "complete", once. A Hello again while the handshake moves on is logged and left alone (xone answers a repeated announce with an identify request only and never restarts the handshake); a bring-up after `T_UP` with the handshake stale, or a re-announce while active, starts it over; the keepalive never sends 06 in its middle. RSA and ECDH run inline (≈0.5 ms and ≈1 ms at -O2 on Apple silicon).
    - Known differences from xone's wire order, to try if a pad still leaves GIP: (1) xone sends the serial-number request `1E 20 SS 01 04` (`gip_init_input` → `gip_send_get_serial_number`) between the Host Hello and the rumble stop; gip.c sends it only with `GIP_OPT_SERIAL` (`xpadctl gip --serial`; the driver sets it for VID 0x3537, a vendor quirk); (2) xone uses one sequence counter per client for every command, gip.c separate ones (system 05/0A/04/1E, security 06, vendor 09/4D); (3) xone registers the client 700 ms after the metadata (`bus/bus.c` `gip_register_client`: `msleep(700)`, a workaround for pads unresponsive after wakeup), so its probe (START, LED, Host Hello, ...) comes that much later; gip.c brings the pad up as soon as the metadata reply ends.
  - **Input 0x20** into `xpad_state`, full precision. Payload `p`:
    - p0 b2 Menu, b3 View, b4 A, b5 B, b6 X, b7 Y;
    - p1 b0 Up, b1 Down, b2 Left, b3 Right, b4 LB, b5 RB, b6 LS, b7 RS;
    - p2..3 LT and p4..5 RT, 10 bits;
    - p6..13 LX LY RX RY as s16 LE, Y up-positive: use `xpad_axis_from_s16_inverted` for LY/RY (32767 → 0, 0 → exactly 0x8000, -32768 → 0xFFFF, so an idle pad gives the idle report);
    - Share bit0 at p[n-18] if n >= 32 and VID != 0x045E (Series layout; set `g->has_share`), as Linux xpad (`data[len-18]`, header included) and xone (`GIP_GP_BTN_SHARE_OFFSET`) read it: p14 for a 32-byte payload, p18 for 36. A 44-byte payload is the DLI firmware layout (xone's offset 26, SDL's fw 5.5 case): Share at p18. Microsoft's own pads that reach us (02D1/02DD/02E3) have no Share.
  - **Guide (0x07)** p0 & 0x03 → `XPAD_BTN_GUIDE` (AnyPad and Linux xpad: the Adaptive Controller sends 2).
  - **Rumble frame**: `09 00 SS 09 00 0F LT RT strong weak FF 00 FF` (values 0..100, same scale as GameController). An all-zero rumble sends the same frame with zero magnitudes.
  - **Logging**: `log_line` becomes `xpad_log` (declared in gip.h).
- Tests (`Tests/`, plain C with a tiny assert macro, `make test` builds them with host clang and runs them):
  1. Descriptor: length 283 and CRC-16/ARC == 0x931D. A mini HID item walker computes report 1 input size = 16 bytes plus ID, and report 3 output size = 8 bytes plus ID.
  2. Every button and hat combination maps to the right bit and offset in the Xbox report. The idle report is `01 00 80 00 80 00 80 00 80 00 00 00 00 00 00 00 00`.
  3. Scaling helpers (`test_xbox`): edges 0x00/0x80/0xFF of the u8 helpers, the s16 and inverted s16 axes (centre exactly 0x8000, monotonic, symmetric). Xbox 360 messages (`test_x360`): every button, axis and trigger, the rumble and LED messages, short and other messages.
  4. GIP, from a real capture in `docs/research/gip_bringup_capture.log` (a third-party dock; its 6-byte device ID replaced by `00 11 22 33 44 55`):
     - the hello frame `02 20 02 1c 00 11 22 33 44 55 00 00 37 35 04 10 01 00 02 00 49 02 02 00 02 0e 01 00 01 00 01 00` triggers the identification request `04 20 01 00`, then (the reply's end, or 100 ms) a bring-up whose first frame is `05 20 02 01 00`; the capture's own frames replayed on its timeline give exactly the captured bring-up 100 ms later, its 05/0A sequence numbers one further on;
     - status `03 20 00 04 87 00 00 00` goes to ACTIVE;
     - a synthetic input frame with known values decodes exactly;
     - ack frames are produced for flags & 0x10;
     - chunked frames don't crash; fuzz with random bytes for 10k iterations.
  5. Rumble parse: an active report → values; a stop report → zeros; GIP rumble frame bytes. The enable nibble uses xpadneo's bits (0x01 weak, 0x02 strong, 0x04 RT, 0x08 LT); motors not enabled are zero; 0xFF or an empty nibble is a stop; duration, delay and loop are not forwarded (GameController sends only 0x0F or 0xFF). `xpad_xbox_rumble_play_ms` gives how long a real pad plays a report on its own: (on + off) × (loop + 1) × 10 ms from bytes [6], [7], [8] (GameController's `0A 00 00` = 100 ms), 0 for a stop.
  6. Authentication: `test_crypto` (SHA-256 FIPS 180-2 and every length 0..200, HMAC RFC 4231, the TLS 1.2 PRF vector, RSA ciphertexts that OpenSSL decrypts, DER key parsing incl. a self-signed certificate with an empty subject, P-256 multiples of G and OpenSSL's ECDH secret) and `test_auth` (a simulated pad drives gip.c through the v1 and v2 handshakes; every 0x06 frame equals the model's byte for byte; then each failure, the switches, the keepalive, a fuzz run). The vectors are `Tests/auth_vectors.h`, written by `Tests/gen_auth_vectors.py` from the committed test keys in `Tests/vectors/` (Python's hashlib/hmac and integer arithmetic, cross-checked with `openssl pkeyutl -decrypt/-derive`); `make -C Tests` regenerates and compares it when python3 and openssl are there.
  7. Personalities (`Tests/check_personalities.py`): every USB personality in `Driver/Info.plist` uses a key combination IOUSBHostFamily matches (below), and the XPadMac ones match what this design asks: only the four classes, USB providers only, no `IOUserServerOneProcess`. `Tests/check_entitlements.py`: `XPadDriver.entitlements` holds exactly the four keys below (no `transport.hid`: nothing matches an `IOHIDInterface`).
  8. After `make build` (not `make test`): `make check-product` runs `check_personalities.py` on the built dext's Info.plist, and `make check-profiles` runs `Tests/check_profiles.py` on the app and the dext: each embedded profile must list the building Mac's Provisioning UDID and grant every restricted entitlement the bundle is signed with (`codesign -d --entitlements - --xml` against the profile's `Entitlements`; `com.apple.security.*` are not restricted; the profile's `*` and `TEAM.*` match any value). Either failing fails `make build`.

## Driver (DriverKit, C++ with .iig)

Common rules:
- Log with `os_log(OS_LOG_DEFAULT, "XPad: ...")`.
- Allocate in `init` or `Start`, release in `free` or `Stop`.
- Follow the ivars pattern: `struct X_IVars` allocated with `IONewZero`.
- No blocking in completion handlers.

**XPadGIPDevice : IOService** (provider `IOUSBHostDevice`), mirroring Apple's `XboxUSBDevice`:
- `Start`:
  - super;
  - `Open`;
  - `CopyConfigurationDescriptor(0)`; walk the interfaces; if none is FF/47/D0, log and return `kIOReturnUnsupported` (declines non-GIP class-255 devices);
  - `SetConfiguration(bConfigurationValue, true)` to match the interfaces;
  - `Close`;
  - `RegisterService()`.
- `Stop`: super.

Optional, behind the personality property `XPadWindowsDescriptors` = true (default false): before `SetConfiguration`, send the Windows probes:
- `GET_DESCRIPTOR(STRING, 0xEE)`, len 18;
- if it answers "MSFT100", the vendor request `0xC0 bVendorCode wIndex 4` for the compat ID.

**XPadGIPGamepad : IOUserHIDDevice** (provider `IOUSBHostInterface` FF/47/D0, `bInterfaceNumber` 0), the GIP host:
- `handleStart`:
  - `Open` the interface;
  - find the interrupt IN and OUT endpoints from the interface descriptor (helpers `IOUSBGetNextEndpointDescriptor` etc.);
  - `CopyPipe` both;
  - create 2 IN buffers of 64 bytes, each with its own `OSAction` (`CreateActionReadComplete`, declared `TYPE(IOUSBHostPipe::CompleteAsyncIO)`), and `AsyncIO` both. **Never cancel or time out the reads.** macOS clears the endpoint halt on cancelled transfers, and that upsets pads; verified with libusb;
  - create an OUT FIFO of 16 × 64-byte frames, one in flight at a time (`CreateActionWriteComplete`);
  - create an `IOTimerDispatchSource` firing every 20 ms → `gip_tick`. Get time from `mach_absolute_time` scaled, or `clock_gettime_nsec_np`; check what DriverKit offers;
  - call `gip_init(vid, pid from the interface's device)`;
  - set `g.reads = 1` after the first `AsyncIO`.
- `ReadComplete`:
  - on success, `gip_receive`; if `GIP_RX_STATE`, build the Xbox report and `handleReport`; resubmit;
  - while stopping (teardown), or on `kIOReturnNoDevice`: stop resubmitting;
  - on `kIOReturnAborted` while not stopping (our own `ClearStall` aborts every read queued on the endpoint, or the interface is going away and the resubmit fails): resubmit, without a clear;
  - on any other error, `kIOReturnNotResponding` (xHCI's transaction error on a pad still attached) and `kIOReturnOffline` included: the endpoint is now halted (IOUSBHostPipe.iig: every IO error but a timeout halts a bulk or interrupt endpoint), so `ClearStall(true)` and resubmit, as Apple's `IOUserUSBHostHIDDevice` retries. At most 8 non-success completions in a row (errors and aborts, both reads together), then a read stops. The OUT pipe is cleared the same way after a failed write (`NoDevice` and `Aborted` excepted). A pipe that works is never cleared or cancelled.
- `newDeviceDescription`: `XPadCreateXboxDescription` (XPadCommon): super's dictionary with `VendorID` 0x045E, `ProductID` 0x0B13, `VersionNumber` 0x0517, `Transport` "BluetoothLowEnergy", `Product` "Xbox Wireless Controller", `Manufacturer` "Microsoft", the pad's `SerialNumber` (or "XPAD-" + locationID hex), `ReportInterval` kept, and the diagnostics property `XPadSource` = "GIP vvvv:pppp".
- `setReport` / `getReport`: HIDDriverKit's dispatcher (`IOHIDDevice::_ProcessReport_Impl`, disassembled) only logs a failed `setReport`/`getReport`; it never completes the action, so every override completes it itself. `getReport` serves input report 1 from the current state; anything else the Xbox descriptor does not have is `kIOReturnUnsupported`.
- `newReportDescriptor`: the Xbox descriptor.
- `setReport` with output ID 3 → `xpad_xbox_parse_rumble` → `gip_rumble` → enqueue; complete the action. The latest request always replaces an unsent one, also while the pad is not up (a stop then cancels a held-back start; a start is sent once the pad is up).
- **Rumble expiry (both HID classes).** The GIP frame (`FF 00 FF`) and the 360 message hold a level for minutes or until told otherwise; a real Xbox Wireless Controller stops after the report's own play time, and GameController relies on that: it re-sends its 100 ms report every 100 ms while a haptic plays and does not send a stop when the client dies. So each start sets a deadline `now + xpad_xbox_rumble_play_ms + kXPadRumbleSlackMs` (200 ms), each stop clears it, and when it passes with no new report the driver stops the motors itself: XPadGIPGamepad and XPad360Gamepad check it in their 20 ms timer and queue a zero rumble. A lost stop must not leave the pad rumbling either: both send the latest request again on the next tick when a rumble write fails (at most 8 in a row). Any client report moves the deadline, so a retried stop never overrides a newer start.
- `Stop`/`free`: cancel the timer, abort the pipes (only at teardown), close the interface, release. Timers and actions are cancelled with `XPadCancelTimer` / `XPadCancelAction`, which drop the last reference only in the cancel handler: releasing before the asynchronous cancellation has run crashes the driver process when it outlives the service.

**XPadGIPAudio : IOService** (provider `IOUSBHostInterface` FF/47/D0, `bInterfaceNumber` 1): selects alternate setting 0 of the GIP audio interface (`SET_INTERFACE(1, 0)`) and keeps it open while the pad is there, as xone's `xone_wired_init_audio_port` does ("mandatory for certain third party devices"). `IOUSBHostDevice::DeviceRequest` refuses a standard SET_INTERFACE, so this takes a service on the interface itself.

**XPad360Gamepad : IOUserHIDDevice** (provider `IOUSBHostInterface` FF/5D/01, `bInterfaceNumber` 0): Xbox 360 wired (XInput) pads.
- What it is for: third-party wired Xbox 360 pads, and pads with an XInput mode ("Xbox 360 Controller for Windows"). Such a pad is typically `bDeviceClass` 0 (composite, so Apple's `AppleUSBHostCompositeDevice` configures it) with interface 0 = FF/5D/01 (the XInput class descriptor 0x21, interrupt IN and OUT of 32 bytes), plus vendor HID or audio interfaces that macOS's own drivers take. Apple's `XboxGamepad.dext` drives only `045E:028E`, so Microsoft's VID is never matched.
- Protocol (`Shared/xpad_x360.c`, from Linux `xpad.c` XTYPE_XBOX360 and `SDL_hidapi_xbox360.c`): input message `00 14` + 18 bytes, [2] D-pad up/down/left/right, Start, Back, LS, RS; [3] LB, RB, Guide, -, A, B, X, Y; [4] [5] LT RT 0..255; [6..13] LX LY RX RY s16 LE, Y up positive (so `xpad_axis_from_s16_inverted`; 0 maps to exactly 0x8000, and an idle pad gives the idle Xbox report). Other messages (`01 03` LED state, `02 03`, `03 03` rumble state, `08 03` headset) are ignored (the first ten are logged). Out: rumble `00 08 00 <strong> <weak> 00 00 00` (0..100 scaled to 0..255; the pad has no trigger motors), LED `01 03 <n>`.
- Transport is XPadGIPGamepad's: 2 interrupt-IN reads (`CreateIOBuffer(In)` + `XPadMappingGet`) always queued and never cancelled; `ClearStall` only after a failed completion, at most 8 non-success completions in a row; one OUT transfer at a time from a dext-owned `XPadCreateBuffer`, behind an 8-message FIFO. `handleStart` queues the LED message `01 03 02` (player 1, as Linux's xpad sends at probe; some third-party pads report nothing until an OUT transfer).
- Every parsed input message is published (`xpad_x360_parse` → `xpad_xbox_build_input` → `handleReport`). A 360 pad has no Share button.
- Rumble: `setReport` report 3 → the latest request replaces an unsent one; expiry as for the other HID classes (deadline `now + playMs + kXPadRumbleSlackMs`, checked by the 20 ms timer, which queues a zero rumble); a failed rumble write is resent from the timer, at most 8 in a row.
- `newDeviceDescription`: `XPadCreateXboxDescription` with `XPadSource` = "X360 vvvv:pppp", the pad's serial and locationID, `ReportInterval` from the IN endpoint (bInterval). `Start`: super, then `RegisterService`. Teardown (once, from `Stop` or a failed `Start` at any point): `XPadCancelTimer`, abort both pipes, close the interface, `XPadCancelAction` on every action.
- Not covered: a 360 pad whose device is vendor class (`bDeviceClass` 255, as Microsoft's own) is not configured by anyone, since `XPadGIPDevice` declines devices without a GIP interface; only composite devices get their interfaces. Wireless 360 receivers (FF/5D/81) and the 360's security interface (FF/FD/13) are not matched.

**Info.plist personalities.** Every personality carries:
- `CFBundleIdentifier` = `$(PRODUCT_BUNDLE_IDENTIFIER)`;
- `CFBundleIdentifierKernel` = `com.apple.kpi.iokit`;
- `IOUserServerName` = `$(PRODUCT_BUNDLE_IDENTIFIER)`;
- `IOProviderClass`, `IOClass`, `IOUserClass`, and the matching keys;
- no `IOUserServerOneProcess`: each matched service gets a driver process of its own, so a crash takes one pad down, not all of them (no state is shared between services).

IOUSBHostFamily matches only the USB key combinations of Apple's QA1076; any other combination silently never matches. Measured on macOS 27.2 with `IOServiceGetMatchingServices` (the same `matchPropertyTable`): `idVendor + bDeviceClass 255` matches nothing (not even a live vendor-class GIP dock), and on an interface `bInterfaceNumber` counts only together with `idVendor + idProduct + bConfigurationValue`. `"*"` (a string) matches any value, as Apple's own personalities use it. Hence:

| Personality | Provider / IOClass / IOUserClass | Match | IOProbeScore |
|---|---|---|---|
| `XPadGIPDevice_<Vendor>` | IOUSBHostDevice / IOUserService / XPadGIPDevice | idVendor V, bDeviceClass 255, bDeviceSubClass "*", bDeviceProtocol "*" (some docks are FF/FF/FF, MS-GIPUSB pads FF/47/D0) | default |
| `XPadGIPGamepad_<Vendor>` | IOUSBHostInterface / AppleUserHIDDevice / XPadGIPGamepad | idVendor V, idProduct "*", bConfigurationValue "*", bInterfaceNumber 0, bInterfaceClass 255, bInterfaceSubClass 71, bInterfaceProtocol 208 | default |
| `XPadGIPAudio_<Vendor>` | IOUSBHostInterface / IOUserService / XPadGIPAudio | as `XPadGIPGamepad_<Vendor>` with bInterfaceNumber 1 | default |
| `XPad360Gamepad_<Vendor>` | IOUSBHostInterface / AppleUserHIDDevice / XPad360Gamepad | idVendor V (the GIP vendors below, never Microsoft), idProduct "*", bConfigurationValue "*", bInterfaceNumber 0, bInterfaceClass 255, bInterfaceSubClass 93, bInterfaceProtocol 1 | 100000 |

`XPadWindowsDescriptors` (on `XPadGIPDevice_<Vendor>`) is a per-vendor quirk switch, true only on the VID 0x3537 personality: some firmware of that vendor picks GIP or its fallback HID mode from how the host reads the configuration descriptor, so the driver also asks the way Windows does. Other vendor quirks are keyed on the VID in `Shared/gip.c` (waiting for the Hello, authentication by default) and in the driver (`GIP_OPT_SERIAL`), as xone and SDL key theirs; a new quirk goes in the same places, not in a vendor-specific class.

Supported third-party vendors (GIP and XInput; decimal):
- 8BitDo 11720 (0x2DC8)
- GameSir 13623 (0x3537)
- Hori 3853 (0x0F0D)
- Hyperkin 11812 (0x2E24)
- Mad Catz 1848 (0x0738)
- PDP 3695 (0x0E6F)
- PowerA 9414 (0x24C6) and 8406 (0x20D6)
- Razer 5426 (0x1532)
- Turtle Beach 4341 (0x10F5)

Microsoft 1118 uses `idProductArray` = [721, 733, 739] (0x02D1, 0x02DD, 0x02E3: Xbox One 2013/2015 and Elite 1, which Apple doesn't drive) in place of the wildcards: `idVendor + idProductArray + bDeviceClass 255` for the device, `idVendor + idProductArray + bConfigurationValue "*" + bInterfaceNumber 0` + FF/47/D0 for the interface. Never match Apple's PIDs 654/746/2816/2834.

**XPadDriver.entitlements (development):**
- `com.apple.developer.driverkit` = true
- `com.apple.developer.driverkit.transport.usb` = [{`idVendor` = "*"}]
- `com.apple.developer.driverkit.family.hid.device` = true
- `com.apple.security.app-sandbox` = true

`OSBundleUsageDescription`: "Makes Xbox One (GIP) and third-party Xbox 360 (XInput) controllers work as an Xbox Wireless Controller."

## App (SwiftUI, macOS 14+ deployment, built with the macOS 27 SDK)

A window with a sidebar of four sections, and the driver's on/off switch in the toolbar (reachable from every section).

Strings are written in English in the source and localized to Vietnamese (`App/Localizable.xcstrings`, `App/vi.lproj/InfoPlist.strings`); the app follows the macOS language. xpadctl prints English only.

**Driver section:**
- shows the state (checking, not installed, awaiting approval, enabled, disabled in System Settings, uninstalling, unknown) with an explanation;
- the on/off switch: on = `OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: "<dext id>", queue: .main)`; off = a deactivation request, after a confirmation that says to unplug the pads first (macOS removes a driver at once only when no device uses it, otherwise at the next restart). A driver disabled in System Settings can only be re-enabled there (no API does it), so "on" opens that pane;
- the delegate handles `needsUserApproval`, `didFinishWithResult` (`.willCompleteAfterReboot` reported as needing a restart), `didFailWithError` (each `OSSystemExtensionError` code explained), and `actionForReplacingExtension` (returns `.replace`; when the bundled dext's version equals the installed one, the app warns that sysextd will keep the running one);
- Install / Reinstall button (the same activation request), "Open System Settings" (`x-apple.systempreferences:com.apple.LoginItems-Settings.extension`), Refresh;
- warns if the app isn't in /Applications (or is translocated), with "Show in Finder" and "Open Applications folder";
- technical details: the dext identifier, the bundled dext's version "short (build)", the app's path.

**Controllers section:**
- live list of `GCController.controllers()` (connect/disconnect notifications, `GCController.shouldMonitorBackgroundEvents = true`);
- for each controller: vendorName, productCategory, the profile, and a live view of `extendedGamepad`: every button lights when pressed, two stick dots, two trigger bars, the D-pad, View/Xbox/Menu/Share. The Home button's `preferredSystemGestureState` is set to `.disabled`: gamecontrollerd hands the Home button only to the frontmost app that takes it over (`.alwaysReceive` does not count);
- a "Rumble test" button: `controller.haptics`, one Core Haptics engine per locality, 1 s continuous haptic for each of left handle, right handle, left trigger, right trigger (those `supportedLocalities` lists), then all, with 0.25 s gaps.

**Polling rate section** (`PollingRateMeter`):
- IOHIDManager lists the gamepads and joysticks (Generic Desktop 4/5) without opening them; the XPadMac ones are labelled by their `XPadSource`;
- the selected device is opened with `kIOHIDOptionsTypeNone` (shared, not seized: games and GameController keep working); an input-report callback counts every report with the timestamp IOKit stamped it with; `kIOReturnExclusiveAccess` is explained;
- statistics aggregated in place (fixed-size histogram, running sums), published at 10 Hz: current rate (1 s window), average, peak, declared (`ReportInterval`); intervals min/mean/median/p99/max/jitter (0.01 ms bins under 20 ms, 1 ms bins up to the pause threshold); gaps over 250 ms count as pauses, not intervals; a 30 s rate chart.

**USB Diagnostics section** (read-only IORegistry walk, no device opened):
- the `IOUSBHostDevice` entries of the supported vendors with `bDeviceClass` 255, or with an Xbox interface (FF/47 GIP or FF/5D XInput) whatever the vendor (VID:PID, product name, configuration);
- which driver class is attached to each (walk the children for `IOUserClass`: XPadGIPDevice / XPadGIPGamepad / XPadGIPAudio / XPad360Gamepad / Apple's), with hints;
- refreshed on USB hotplug notifications, and by a Refresh button.

**App.entitlements:** `com.apple.developer.system-extension.install` = true. No sandbox.

**Info.plist:** `NSSystemExtensionUsageDescription` = "Installs the XPadMac driver for Xbox One (GIP) and Xbox 360 (XInput) controllers."

## xpadctl (C, plus `gc.m` in Objective-C)

Built by `make tool` with host clang. It links IOKit, CoreFoundation, Foundation and GameController, and libusb only when `WITH_LIBUSB=1` (Homebrew at /opt/homebrew).

| Command | What it does |
|---|---|
| `xpadctl doctor` | Prints, with ✅/⚠️/❌ verdicts and hints: macOS version and arch; `csrutil status`; Xbox-protocol USB devices (supported vendors' class-255 devices, or any device with a GIP/XInput interface: VID:PID, configuration, attached driver classes); HID gamepads (IOHIDManager: VID:PID, Transport, Product, and whether it is an XPad device via the `XPadSource` property); `GCController` list (gc.m); `systemextensionsctl list` lines containing `xpadmac`; whether XPadMac.app is in /Applications, embeds the dext, and is signed so sysextd will install it (app: `com.apple.developer.system-extension.install`, dext: `com.apple.developer.driverkit`, neither ad hoc, both with `embedded.provisionprofile`). |
| `xpadctl hid [--vid V --pid P] [--all] [--seconds N]` | Default 045E:0B13 (the XPad device). Opens the HID device and prints each raw report (only changes, unless `--all`), plus the decoded `xpad_state` for an Xbox report; other devices are printed raw. |
| `xpadctl gc [--seconds N]` | Live GameController view in the terminal: name, buttons pressed (Home included: it sets the Home button's system-gesture state), stick values, triggers, refreshed 10×/s. Ctrl-C quits. |
| `xpadctl gip` | Only with `WITH_LIBUSB=1`. The userspace GIP test of `docs/research/probe4_userspace_gip.c`, using `Shared/gip.c` and printing the decoded state. A failed read (stall, overflow, error) parks the transfer; the main loop clears the IN halt (`libusb_clear_halt`, which also aborts the other reads) and queues them again, at most 8 times in a row, as the driver does. `--auth` / `--fake-auth` / `--serial` / `--blind` / `--keepalive` set `GIP_OPT_AUTH` / `GIP_OPT_FAKE_AUTH` / `GIP_OPT_SERIAL` / `GIP_OPT_BLIND` / `GIP_OPT_KEEPALIVE`; `--windows-desc` sends the Windows descriptor probes first; `--vid`/`--pid` pick the device, `--wait`/`--seconds` the time. |

## Build

- `project.yml` (xcodegen 2.46):
  - target `XPadMac`: `type: application`, platform macOS, sources `App`;
  - target `XPadDriver`: `type: driver-extension`, declared platform macOS (xcodegen has no DriverKit platform, as Xcode's own template) with `SDKROOT=driverkit` and `SUPPORTED_PLATFORMS=driverkit`, sources `Driver` + `Shared/*.c`;
  - the app depends on the dext with `embed: true`, so it lands in `Contents/Library/SystemExtensions`.
- Settings:
  - `CODE_SIGN_STYLE=Automatic`;
  - `DEVELOPMENT_TEAM` and `BUNDLE_ID_PREFIX` from Config.xcconfig / Config.local.xcconfig;
  - `CODE_SIGN_ENTITLEMENTS` per target;
  - `ENABLE_HARDENED_RUNTIME=NO` for Debug;
  - `MACOSX_DEPLOYMENT_TARGET=14.0`;
  - `DRIVERKIT_DEPLOYMENT_TARGET=21.0`;
  - no `ARCHS` override: the app and the dext are built universal (arm64 and x86_64).
- Makefile targets:
  - `make test`
  - `make tool`
  - `make project` (xcodegen)
  - `make build-unsigned`: `xcodebuild ... CODE_SIGNING_ALLOWED=NO`. It must compile with zero errors even with no account.
  - `make build`: first `check-signing` (fails with instructions when `TEAM` is empty, `PREFIX` is the placeholder `com.example.`, or does not end with a dot), then `-allowProvisioningUpdates -allowProvisioningDeviceRegistration`, and `CURRENT_PROJECT_VERSION=$(BUILD_NUMBER)` (default: the UTC time as DAYS.HOUR.MINUTE, days from 2020-01-01; a dext's `CFBundleVersion` must be a kext version, at most 9999.99.99, and kernelmanagerd refuses the dext otherwise, so seconds since 1970 cannot be used; `make check-product` checks it). sysextd replaces an installed extension only when its version differs ("found existing entry of same version" otherwise, reported as success), so every build gets its own `CFBundleVersion`; the app also warns when the bundled dext's version equals the installed one.
  - `make install`: copy to /Applications, open the app; warn when the app or dext has no `embedded.provisionprofile` (a build-unsigned build, whose driver sysextd refuses).
  - `make uninstall`
  - `make clean`

## Acceptance

1. `make test` passes (unit tests, the DriverKit build check of Shared/, the descriptor generator check, the authentication vector check and the personality check).
2. `make tool` builds `xpadctl`; `xpadctl doctor` runs.
3. `make project && make build-unsigned` builds the app and the dext with zero errors, and the dext Info.plist has the personalities.
4. When an Xcode account exists: `make build` → `make install` → Install → approve → plug in the pad. Then `xpadctl doctor` shows the pad at `045E:0B13` with `XPadSource`, and `xpadctl gc` lists an Xbox controller that tracks the pad.
