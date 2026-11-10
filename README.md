# XPadMac

**English** · [Tiếng Việt](README.vi.md)

XPadMac is a DriverKit driver for macOS. It makes **wired Xbox One / Xbox Series controllers (the GIP protocol)** and **third-party wired Xbox 360 controllers (XInput)** that macOS ignores show up as an **Xbox Wireless Controller** (`045E:0B13`). Games that use the GameController framework, System Settings › Game Controllers, Safari (Gamepad API) and IOHID apps then see a standard Xbox controller, rumble included.

XPadMac is not a kext and does not need SIP turned off. The current version is 0.1.0, an early release.

Quick links: [Requirements](#requirements) · [Build and install](#build-and-install) · [Using XPadMac](#using-xpadmac) · [Troubleshooting](#troubleshooting) · [Updating](#updating-the-driver) · [Uninstalling](#uninstalling) · [Limitations and distribution](#limitations-and-distribution)

## Why XPadMac exists

**macOS drives only four wired Xbox controllers.** Apple's built-in driver, `/System/Library/DriverExtensions/XboxGamepad.dext`, matches only four Microsoft product IDs: `045E:028E` (Xbox 360 wired), `045E:02EA` (Xbox One S), `045E:0B00` (Elite Series 2) and `045E:0B12` (Xbox Series X|S). No driver takes any other GIP controller: the original Xbox One pads, the Elite Series 1, and the GIP pads, docks and receivers of PDP, PowerA, Hori, Razer, 8BitDo and other vendors. macOS never sets a USB configuration for them, so they have no interfaces at all. To a game, the controller is not plugged in.

**Third-party Xbox 360 pads have no driver either.** macOS configures them (they are usually composite devices), but nothing takes their XInput interface (`FF/5D/01`), so games do not see them.

**A fallback HID mode does not help.** Some third-party pads switch to a plain HID gamepad mode when no GIP host answers them. macOS's generic HID driver takes that mode, but **GameController does not list it**: it is not one of the controllers Apple knows. XPadMac keeps the pad in GIP mode by answering it at once.

**Why not a virtual controller in user space.** The simplest design would read the real pad with IOUSBHost or libusb and create a virtual HID device (`IOHIDUserDevice`, or CoreHID's `HIDVirtualDevice`). That API needs the restricted entitlement `com.apple.developer.hid.virtual.device`:

- Apple must approve it: the team's Account Holder files a Capability Request, and some requests have waited for months;
- it has no "Development" variant;
- the kernel kills a program that lacks it, even one running as root. The only way around that is to turn off AMFI, which means turning off SIP.

**Every DriverKit entitlement has a "Development" variant**, which any paid Apple Developer team may use without asking Apple, and a development-signed dext runs with SIP on. So XPadMac is a DriverKit extension.

**Why it poses as a Bluetooth Xbox controller.** For a HID device created by a dext that is not Apple's, the kernel only lets the GameController personalities marked `GCIOMatchVirtual=true` match. Those are the Bluetooth Xbox IDs (`045E:0B13`, `0B20`, …). Apple's `XboxOneHIDServicePlugin` also requires `VendorID` 0x045E, a `Transport` other than `"USB"`, and a product ID from its list. So the driver takes the identity of an Xbox Wireless Controller over Bluetooth LE. The evidence (disassembly, Apple DTS forum threads, Linux and SDL code) is in `docs/research/`; the detailed design is in [`docs/DESIGN.md`](docs/DESIGN.md).

## What XPadMac does

```text
GIP pad (FF/47/D0)      ─► XPadGIPDevice ─► XPadGIPGamepad ──┐
XInput pad (FF/5D/01)   ─► XPad360Gamepad ───────────────────┼─► HID "Xbox Wireless Controller" 045E:0B13
                                                             │   Transport BluetoothLowEnergy
                                                             ▼
                                              GameController · Safari · IOHID apps
```

XPadMac has three parts:

- **XPadDriver**, the DriverKit extension `<prefix>xpadmac.driver.dext`, embedded in the app at `Contents/Library/SystemExtensions`. It has four classes:
  - **`XPadGIPDevice`** works on the USB device, like Apple's `XboxUSBDevice`. It reads the configuration descriptor; if there is a GIP interface (`FF/47/D0`) it sets the configuration so the interfaces appear, otherwise it declines the device (an Xbox 360 / XInput pad, for example).
  - **`XPadGIPGamepad`** is the GIP host on interface 0 (`FF/47/D0`). It brings the pad up, sends acknowledgements, reassembles chunked frames and keeps the pad alive; the state machine comes from AnyPad and lives in `Shared/gip.c`. It reads input `0x20` at full precision (16-bit sticks, 10-bit triggers), the Guide button and the Share button (Series layout), and sends GIP rumble to all four motors: the two main motors and the two trigger motors. For pads that need it, it performs **real GIP authentication**, the host side of the RSA (v1) and ECDH (v2) handshakes, as Linux's xone does (`Shared/gip_auth.c`).
  - **`XPadGIPAudio`** puts the GIP audio interface (interface 1) on alternate setting 0, as xone does; some third-party pads wait for that request. XPadMac does not carry audio.
  - **`XPad360Gamepad`** takes the XInput interface (`FF/5D/01`) of third-party wired Xbox 360 pads (`Shared/xpad_x360.c`), with two-motor rumble.
- **XPadMac.app**, a SwiftUI app with four sections: **Driver**, **Controllers**, **Polling Rate** and **USB Diagnostics** (see [Using XPadMac](#using-xpadmac)).
- **xpadctl**, a command-line diagnostic tool (`doctor`, `hid`, `gc`, and `gip` when built with libusb).

The driver publishes this identity:

- `VendorID` 0x045E, `ProductID` 0x0B13, `VersionNumber` 0x0517;
- `Transport` `"BluetoothLowEnergy"`, `Product` `"Xbox Wireless Controller"`, `Manufacturer` `"Microsoft"`;
- the 283-byte report descriptor of a real controller over BLE, verbatim;
- the diagnostic property `XPadSource`, `"GIP vvvv:pppp"` or `"X360 vvvv:pppp"`, which names the real pad.

### Supported controllers

The list comes from the 43 `IOKitPersonalities` in `Driver/Info.plist`.

| Vendor | VID | What matches | Personalities |
| --- | --- | --- | --- |
| Microsoft Xbox One (2013, 2015), Elite Series 1 | `045E` | only `02D1`, `02DD`, `02E3` | `XPadGIPDevice_Microsoft`, `XPadGIPGamepad_Microsoft`, `XPadGIPAudio_Microsoft` |
| 8BitDo | `2DC8` | any GIP product | `…_8BitDo` |
| GameSir | `3537` | any GIP product | `…_GameSir` |
| Hori | `0F0D` | any GIP product | `…_Hori` |
| Hyperkin | `2E24` | any GIP product | `…_Hyperkin` |
| Mad Catz | `0738` | any GIP product | `…_MadCatz` |
| PDP | `0E6F` | any GIP product | `…_PDP` |
| PowerA | `24C6`, `20D6` | any GIP product | `…_PowerA`, `…_PowerA_BDA` |
| Razer | `1532` | any GIP product | `…_Razer` |
| Turtle Beach | `10F5` | any GIP product | `…_TurtleBeach` |
| Every vendor above except Microsoft, Xbox 360 (XInput) pads | as in the vendor's row | interface 0 `FF/5D/01` (255/93/1) on a device macOS has configured (composite) | `XPad360Gamepad_<Vendor>` |

"Any GIP product" means: `XPadGIPDevice_<Vendor>` matches the vendor's devices with `bDeviceClass` 255 (vendor-specific), `XPadGIPGamepad_<Vendor>` matches interface 0 and `XPadGIPAudio_<Vendor>` interface 1 with class/subclass/protocol `FF/47/D0` (255/71/208). `XPadGIPDevice` declines a class-255 device that has no GIP interface.

Controllers that need special handling (vendor quirks) are recognised by their VID, as xone and SDL do. A quirk can make the driver wait for the pad's Hello however long it takes, perform real GIP authentication by default, ask for the serial number, or send Windows-style descriptor requests before configuring the device. The quirks are described in `docs/DESIGN.md`.

Most vendors in the table are matched the way Linux xpad and xone match them, by VID and GIP/XInput interface, and **have not been tested on real hardware**. If your controller works (or does not), please [open an issue](https://github.com/hiepkay/Xpadmac/issues) with the output of `xpadctl doctor`.

### Not supported

- **Bluetooth.** macOS already supports Xbox Series and Xbox One S controllers over Bluetooth; XPadMac is USB only (cable, or a USB dock or receiver that speaks GIP).
- **The Xbox 360 wireless receiver** (`FF/5D/81`), and Xbox 360 pads whose USB device is vendor class (`bDeviceClass` 255, as Microsoft's own are): only composite devices, which macOS configures itself, get their XInput interface.
- **Audio** (headset jack, chat), the battery level (the 283-byte descriptor has no battery report), LED control, and vendors' configuration or firmware-update software.
- **Microsoft controllers macOS already drives** (`045E:028E`, `02EA`, `0B00`, `0B12`): Apple's driver keeps them, and `XPad360Gamepad` never matches Microsoft's VID.
- **A third-party pad's fallback HID mode** (after the pad has left GIP): unplug it and plug it in again so it starts in GIP.

## Requirements

- A Mac with **Apple silicon** or an **Intel** processor. XPadMac is developed on macOS 27.2 on Apple silicon. The app declares macOS 14 as its minimum, but versions before 27 have not been tested. The app and the driver are built for both arm64 and x86_64, but Intel Macs have not been tested.
- SIP **on**, as normal. XPadMac does not need it off.
- **Xcode** with the DriverKit SDK (developed with Xcode 27), and the Command Line Tools (`clang`, `python3`).
- **[XcodeGen](https://github.com/yonaskolb/XcodeGen) 2.46 or later**: `brew install xcodegen` ([Homebrew](https://brew.sh)).
- A **paid Apple Developer Program** team, signed in to Xcode. Free personal teams do not get the DriverKit entitlements. For `make build` to register your Mac and create the App IDs and profiles itself, your role in the team must be Admin or Account Holder; otherwise ask an Admin to add your Mac's Provisioning UDID (see [Step 2](#step-2-register-your-mac)).
- An administrator password on the Mac, to allow the driver.
- Optional: `brew install libusb` for the `xpadctl gip` command; `openssl` for the authentication-vector check in `make test` (skipped without it).

## Build and install

Run every command below in the top folder of the source tree (the one with the `Makefile`). `make help` lists the targets and shows the team and prefix in use. You can run `make test` first to check the C core and the personalities; it needs no account.

### Step 0: Get the source

```sh
git clone https://github.com/hiepkay/Xpadmac.git
cd Xpadmac
```

The commands in the steps below run in this folder.

### Step 1: Set your team and bundle ID prefix

1. Open **Xcode › Settings… › Accounts**, click **+**, choose **Apple ID**, and sign in with an account in a paid team.
2. Find your Team ID: [developer.apple.com/account](https://developer.apple.com/account) › Membership details.
3. Create `Config.local.xcconfig` in the top folder. It is git-ignored; do not edit `Config.xcconfig`, which leaves the team empty and uses the placeholder prefix `com.example.`:

   ```text
   DEVELOPMENT_TEAM = ABCDE12345
   BUNDLE_ID_PREFIX = com.yourname.
   ```

   The prefix ends with a dot and must be your own (a reverse domain you control, for example), because an App ID belongs to one team. The bundle IDs are then:
   - app: `<prefix>xpadmac`;
   - driver: `<prefix>xpadmac.driver`.

   The `Makefile` reads both values from `Config.xcconfig`, then from `Config.local.xcconfig`; the command line or the environment wins (`make build TEAM=… PREFIX=…`). `make build` stops at once, with instructions, when there is no team, when the prefix is still `com.example.`, or when it does not end with a dot.

### Step 2: Register your Mac

A development-signed driver runs only on a Mac that its provisioning profile lists by the Mac's **Provisioning UDID**. Find it in **System Information › Hardware › Provisioning UDID**, or with:

```sh
system_profiler SPHardwareDataType | grep 'Provisioning UDID'
```

`make build` passes `-allowProvisioningDeviceRegistration` to Xcode, which registers the Mac in your team for you (Admin or Account Holder role). Otherwise, add it by hand at [Certificates, Identifiers & Profiles › Devices](https://developer.apple.com/account/resources/devices/list) as a **macOS** device.

> **Apple silicon pitfall: Hardware UUID versus Provisioning UDID.** On an Apple silicon Mac, System Information shows both a *Hardware UUID* and a *Provisioning UDID*, and they differ. Profiles must list the **Provisioning UDID**. If the Mac was registered earlier by its Hardware UUID (older tools did this), Xcode's automatic Mac App Development profile for the app is tied to that record and macOS refuses to run the app ("No matching profile found"). Registering the Provisioning UDID afterwards does not fix the app either: the portal then records it as an iOS device ("iPod"), which macOS App Development profiles leave out. The driver's DriverKit profile lists such a record, so the driver is fine; only the app needs one of these ways out:
>
> - `make build APP_PROFILE="<name>"`: sign the app manually with a **macOS App Development** profile from the portal that lists the Provisioning UDID (xcodebuild downloads it by name);
> - `make build APP_DEVID_PROFILE="<name>"`: re-sign the app with a **Developer ID** profile, which lists no devices. Create a Developer ID profile for the app's App ID (`<prefix>xpadmac`, with the System Extension capability), download it and double-click it to install it; a Developer ID Application certificate with its private key must be in your keychain. Xcode will not embed a dext signed with another certificate, so the app and driver are built as usual and then `Tools/resign-app.py` re-signs only the app's outer bundle (with the hardened runtime, without `get-task-allow`). The driver keeps its development signature and profile, and macOS only requires that both are from the same team.

### Step 3: Build signed

```sh
make build
```

This:

1. checks the team and prefix (`check-signing`);
2. generates `XPadMac.xcodeproj` from `project.yml` (XcodeGen);
3. runs `xcodebuild … -allowProvisioningUpdates -allowProvisioningDeviceRegistration`, which registers the Mac if needed and creates the App IDs and development profiles with the `DriverKit`, `DriverKit USB Transport` and `DriverKit Family HID Device` entitlements (Development variants) for the driver and `System Extension` for the app;
4. gives the build its own version number, `CFBundleVersion` = the UTC time as DAYS.HOUR.MINUTE with days counted from 2020-01-01, so macOS sees each build as a new driver;
5. checks that the driver is inside the app with valid personalities (`make check-product`), and that each embedded profile lists this Mac's Provisioning UDID and grants every restricted entitlement its bundle is signed with (`make check-profiles`). Either check failing fails the build.

The first time, macOS may ask whether `codesign` may use your keychain key; choose **Always Allow**. A successful build ends with:

```text
Built (signed for team …, build …): build/DerivedData/Build/Products/Debug/XPadMac.app
```

`make build-unsigned` builds the same code without signing, so it needs no account or team (it works with the `com.example.` prefix). That build only checks that everything compiles; **its driver cannot be installed**.

### Step 4: Install the app in /Applications

```sh
make install
```

This removes any old `/Applications/XPadMac.app`, copies the new build there with `ditto`, and opens it. macOS activates system extensions only from an app in `/Applications`; run from anywhere else, the app's **Driver** section shows "The app is not in /Applications". `make install` also warns when the app or the driver has no embedded provisioning profile (a `make build-unsigned` build).

### Step 5: Turn the driver on and allow it

1. In XPadMac, open **Driver** and turn on the driver switch (or click **Install Driver**). The status changes to **Waiting for your approval**.
2. Click **Open System Settings** and go to **System Settings › General › Login Items & Extensions › Driver Extensions**. Turn on **XPadMac** and enter an administrator password.
3. Back in the app, the status must be **Installed and running**, with the message "The driver is on. Unplug and replug the controller (or the dock)." If the app says a restart is needed to finish, restart the Mac.

### Step 6: Plug the controller in again

1. Quit any program that may be holding the controller's USB device, above all **Google Chrome** (Gamepad API / WebUSB). Only one program can open a USB device at a time; if Chrome holds the pad, the driver cannot.
2. Unplug the USB cable (the pad's, or the dock's or receiver's) and plug it in again. **Do not hold any button** while you plug it in.
3. Wireless pads on a dock or receiver: press the Xbox button to turn the pad on. Some receivers only report a pad once it has connected.

The pad should now appear in the app's **Controllers** section and in System Settings › Game Controllers as an **Xbox Wireless Controller**.

## Using XPadMac

### The app

- **Driver**: the driver's status and an on/off switch (also in the window's toolbar). Turning it on activates the driver; turning it off, after a confirmation, deactivates it. A driver that is installed but switched off in System Settings can only be switched back on there, so the switch opens System Settings in that case. **Install Driver** (**Reinstall / Update Driver** once installed) installs the driver bundled in the app, or replaces the installed one with it. **Technical Details** shows the driver's bundle ID, the version bundled in the app and the app's location. If the bundled driver has the same version as the installed one, the app says so: macOS keeps the running one in that case.
- **Controllers**: the controllers GameController sees, each with a live view in which every button, both sticks, both triggers and the D-pad light up as you use them. **Test Rumble** buzzes the left motor, the right motor, the left trigger, the right trigger and then all of them, one second each (only the motors GameController reports).
- **Polling Rate**: pick a HID gamepad, click **Start**, and measure how many input reports per second reach macOS. It shows the current rate (1 s), the average, the peak and the declared rate (`ReportInterval`), the interval statistics (minimum, mean, median, p99, maximum, jitter) and a chart of the last 30 seconds. Devices labelled **XPadMac** are the controllers the driver publishes, so their rate is the one games receive; unlabelled devices show their own raw rate. Keep both sticks moving while you measure: GIP pads only send reports when something changes, and silences over 250 ms count as pauses, not intervals. The device is opened shared, so games keep working during the measurement.
- **USB Diagnostics**: the Xbox controllers attached over USB (the supported vendors' class-255 devices, and any device with a GIP or XInput interface), how each is configured, and which driver class sits on it (XPadMac's `XPadGIPDevice` / `XPadGIPGamepad` / `XPadGIPAudio` / `XPad360Gamepad`, Apple's, or none), with hints. It refreshes itself when a device is plugged in or removed.

### xpadctl

```sh
make tool                          # builds Tools/xpadctl/xpadctl (add WITH_LIBUSB=1 for the gip command)
Tools/xpadctl/xpadctl doctor       # checks everything, with hints; read-only, sends nothing to the pad
Tools/xpadctl/xpadctl gc           # the controller as GameController sees it, 10 times a second (Ctrl-C quits)
```

| Command | What it does |
| --- | --- |
| `xpadctl doctor` | A ✅/⚠️/❌ verdict, with hints, for: the system, SIP, the USB devices, the HID gamepads, GameController, the XPadMac system extension, and the app (in /Applications, driver embedded, signed so that macOS will install it). It exits with status 1 if any item is ❌. |
| `xpadctl hid [--vid V --pid P] [--all] [--seconds N]` | Prints each raw HID report, and the decoded state for the XPad device. Default: `045E:0B13`; `--vid`/`--pid` pick another device (raw bytes only); `--all` prints every report, not only changes. |
| `xpadctl gc [--seconds N]` | A live GameController view in the terminal: name, buttons, sticks and triggers. |
| `xpadctl gip [options]` | Only with `make tool WITH_LIBUSB=1`. Runs the GIP host in user space with libusb, to test the protocol without the driver. `--auth`, `--fake-auth` and `--serial` force real authentication, the fake "authentication complete", or the serial-number request; `xpadctl gip --help` lists the rest. It seizes the USB device and sends GIP traffic to the pad, so **do not run it while the driver is driving the pad**. |

When everything works, the key lines of `xpadctl doctor` look like this (abridged, one GIP pad `vvvv:pppp`):

```text
== USB devices (Xbox controllers: GIP and XInput) ==
✅ vvvv:pppp "…" — GIP, driver XPadGIPDevice, XPadGIPAudio, XPadGIPGamepad running

== HID controllers (IOHIDManager) ==
✅ 045e:0b13 "Xbox Wireless Controller" transport BluetoothLowEnergy — XPad device
     XPadSource = "GIP vvvv:pppp", driver XPadGIPGamepad

== GameController ==
✅ "Xbox Wireless Controller" — category "…", extendedGamepad

== XPadMac driver (System Extension) ==
✅ The driver is enabled (activated enabled).

== App ==
✅ /Applications/XPadMac.app 0.1.0 (2505.9.41) (driver included, signed)
```

The **App** item reads the signatures of both the app and the driver. If it says the app is not signed so that macOS will install its driver, `/Applications/XPadMac.app` is a `make build-unsigned` build (or lacks an entitlement or profile): rebuild with `make build`, then `make install`.

### Languages

The app follows the macOS language: English, or Vietnamese when Vietnamese is the preferred language. `xpadctl` prints English only.

## Troubleshooting

Always start with `Tools/xpadctl/xpadctl doctor`: it tells you which ID macOS sees the pad under, which driver has it and which program holds it.

> In zsh, the default macOS shell, `log` is a shell built-in. The commands below therefore call `/usr/bin/log`; typing `log stream …` fails with `too many arguments`.

### Driver log

Every line the driver logs starts with `XPad:`. Run this, then unplug and replug the pad. It shows the ID the pad was matched under, the GIP bring-up and authentication, and any USB pipe errors:

```sh
/usr/bin/log stream --predicate 'eventMessage CONTAINS "XPad:"'
```

For a pad that needs authentication, a good handshake logs `auth v1: client finish verified` (or v2), then `tx auth complete`.

### The driver stays "terminating for upgrade"

`systemextensionsctl list` shows the old driver as `[terminating for upgrade via delegate]`, or the app says the change takes effect after a restart: macOS cannot remove a driver while a device is still using it. **Restart the Mac.** Next time, unplug the controllers (and dock or receiver) before you update or turn off the driver; then no restart is needed.

### Another program is holding the controller

The USB section of `doctor` shows "programs using this USB device: …" with each program's name and pid. The usual culprit is Google Chrome; otherwise any program that opens USB devices directly (libusb, WebUSB), `xpadctl gip` included. Quit it, then unplug and replug the pad. The Polling Rate meter reports `kIOReturnExclusiveAccess` when another app has seized a HID device.

### The pad switches to its HID mode

Some third-party pads leave GIP for a fallback HID mode (with another product ID) when no GIP host answers in time, for example when they were plugged in before the driver was running. GameController does not see that mode. Once the driver is running, unplug and replug the pad (for a wireless pad, turn it off and on again), **without holding any button**. If the pad has a button combination to choose its mode (see the vendor's manual), choose Xbox / PC (XInput), not Android / HID.

### The Xbox (Home) button does nothing in an app

That is how macOS treats a real Xbox controller too: it keeps the Home button for a system gesture and hands it only to the **frontmost** app that takes it over. XPadMac's Controllers view takes it, so the button lights there while XPadMac is in front; `xpadctl gc` shows it as `Home`.

### The driver does not load

1. **Status.** Look at the app's **Driver** section, or run:

   ```sh
   systemextensionsctl list | grep -i xpadmac
   ```

   - `[activated enabled]`: the driver is running.
   - `[activated waiting for user]`: not yet allowed; redo [Step 5](#step-5-turn-the-driver-on-and-allow-it).
   - no line: not installed; turn the driver on in the app.
2. **App location.** The app must run from `/Applications/XPadMac.app`. If macOS runs it from a temporary copy (App Translocation), the app warns. Run `make install` again.
3. **Signature, profile and entitlements.** These messages all come from a build that is not signed right:
   - "The app lacks the entitlement to install a system extension…": an unsigned build (`make build-unsigned`);
   - "The driver's code signature is invalid…" or "macOS rejected the driver during validation…": the profile does not list this Mac, or the app and the driver are not from the same team.

   Check with:

   ```sh
   DEXT=$(ls -d /Applications/XPadMac.app/Contents/Library/SystemExtensions/*.dext)
   system_profiler SPHardwareDataType | grep 'Provisioning UDID'       # this Mac's UDID
   security cms -D -i "$DEXT/embedded.provisionprofile" | plutil -p - | grep -A3 -E '"ProvisionedDevices"|"TeamIdentifier"'
   codesign -d --entitlements - "$DEXT"                                # driverkit, transport.usb, family.hid.device
   codesign -dv "$DEXT" 2>&1 | grep -E 'Authority|TeamIdentifier'     # Apple Development, same team as the app
   python3 Tests/check_profiles.py /Applications/XPadMac.app "$DEXT"   # what make build checks
   ```

   If the profile lacks the UDID, add the Mac to the team (see [Step 2](#step-2-register-your-mac), including the Apple silicon pitfall), then `make build && make install` and turn the driver on again.
4. **Activation log.** Run this in a Terminal window, then turn the driver on:

   ```sh
   /usr/bin/log stream --style compact --predicate 'process == "sysextd" OR process == "kernelmanagerd" OR eventMessage CONTAINS[c] "dext"'
   ```

### The driver runs but the pad does not appear

- Read the **USB devices** section of `doctor`:
  - "GIP controller with no driver: macOS ignores it (not configured, no interfaces)": no personality matched. The pad's vendor is not in the [table](#supported-controllers), or the driver is not enabled.
  - "XPadGIPDevice configured it, but XPadGIPGamepad has not attached to interface 0": read the driver log.
  - "configured, but no XPad driver": another program or driver holds the interface; see [above](#another-program-is-holding-the-controller).
- If `doctor` says "The XPad driver created a HID device, but GameController lists no controllers": unplug and replug. If it persists, [open an issue](https://github.com/hiepkay/Xpadmac/issues) with the output of `xpadctl doctor` and the driver log.

## Updating the driver

After changing the code or pulling a new version:

1. Unplug the controllers (and dock or receiver), so macOS can replace the running driver without a restart.
2. Run `make build && make install`. Each `make build` sets a new build number, so macOS sees a new driver version and replaces the old one. macOS replaces a driver only when the version differs: with the same version it keeps the running driver and still reports success (the app warns when that happens, for example after building straight from Xcode; bump `CURRENT_PROJECT_VERSION` or build with `make build`).
3. In the app, click **Reinstall / Update Driver**, then plug the controller in.
4. If the app says a restart is needed, or `systemextensionsctl list` shows `terminating for upgrade`, **restart the Mac**.

## Uninstalling

1. Unplug the controllers. In XPadMac, open **Driver**, turn the driver switch off and confirm with **Turn Off Driver**. If the app says the driver turns off for good after a restart, restart the Mac.
2. Run `make uninstall` to remove `/Applications/XPadMac.app`.
3. Optionally:
   - `make clean` removes the build products;
   - delete `Config.local.xcconfig`;
   - the App IDs, profiles and the registered Mac stay in your developer account; remove them at [Certificates, Identifiers & Profiles](https://developer.apple.com/account/resources/) if you wish.

**Order matters.** Deleting the app first leaves the driver in the system, and with SIP on `systemextensionsctl uninstall` does not work. If you deleted the app first, `make install` it again and turn the driver off.

## Limitations and distribution

- **A development-signed build runs only on registered Macs.** The driver activates only on a Mac whose Provisioning UDID is in the team's development profile. You cannot hand a built `.app` to someone else: each person builds it with their own team, or the team registers their Mac (at most 100 Macs per membership year) and rebuilds. Development certificates and profiles expire; run `make build` again when they do.
- **Wide distribution needs Apple's approval.** To sign with Developer ID and notarize, the team's Account Holder must ask Apple for the distribution variants of the DriverKit entitlements:
  - `com.apple.developer.driverkit`;
  - `com.apple.developer.driverkit.transport.usb`, listing each vendor ID in decimal (for example 3695 for PDP, `0E6F`);
  - `com.apple.developer.driverkit.family.hid.device`.

  Request them at [developer.apple.com/contact/request/system-extension](https://developer.apple.com/contact/request/system-extension/), or on the App ID's Capability Requests tab ([how](https://developer.apple.com/help/account/capabilities/capability-requests/)). Review can take weeks or more. XPadMac does not have these entitlements. (`APP_DEVID_PROFILE` re-signs only the app; the driver stays development-signed, so it does not lift this limit.)
- **The Microsoft identity is borrowed.** The driver publishes `045E:0B13` "Xbox Wireless Controller", Manufacturer "Microsoft", Transport `BluetoothLowEnergy`, although the real pad is on USB and not made by Microsoft. So:
  - games show Xbox names and button glyphs;
  - tools that need the real identity (firmware updaters, vendors' configuration software) do not recognise the pad;
  - apps may treat the pad as wireless;
  - there is no battery level.

  Apple has published no policy on a dext publishing another company's VID/PID; ask Apple DTS about it when you request distribution entitlements.
- **Few controllers tested**: most vendors in the table have not been tried on real hardware.

## Project layout

| Path | Contents |
| --- | --- |
| `Shared/` | the C99 core shared by the driver, `xpadctl` and the tests: `xpad.h` and `gip.h` (the contract between the parts), the Xbox report builder, the 283-byte descriptor, the GIP host `gip.c`, the authentication `gip_auth.c` and its crypto `gip_crypto.c`, the Xbox 360 translator `xpad_x360.c` |
| `Driver/` | the DriverKit extension (`XPadGIPDevice`, `XPadGIPGamepad`, `XPadGIPAudio`, `XPad360Gamepad`), `Info.plist` (personalities), entitlements |
| `App/` | the SwiftUI app XPadMac |
| `Tools/xpadctl/` | the diagnostic tool |
| `Tools/resign-app.py` | re-signs the app with a Developer ID profile (`make build APP_DEVID_PROFILE=…`) |
| `Tests/` | unit tests of the core, and checks of the personalities (`check_personalities.py`), entitlements (`check_entitlements.py`) and profiles (`check_profiles.py`) |
| `docs/` | [`DESIGN.md`](docs/DESIGN.md) and the research evidence (`research/`) |
| `project.yml`, `Config.xcconfig`, `Makefile` | the XcodeGen spec, the shared build settings, the build entry points |

`make` targets:

```sh
make test             # C core tests, DriverKit build check of Shared/, descriptor, auth vectors, personalities, entitlements
make tool             # build Tools/xpadctl/xpadctl  (make tool WITH_LIBUSB=1: adds the gip command)
make project          # generate XPadMac.xcodeproj from project.yml
make build-unsigned   # build the app and driver unsigned (any Mac, no account)
make build            # build signed (Apple Development)
make check-product    # check the driver inside the app and its personalities
make install          # copy to /Applications/XPadMac.app and open it
make uninstall        # remove /Applications/XPadMac.app
make clean            # remove build products
```

## Tests

`make test` (that is, `make -C Tests`) needs only Xcode's tools and no account. It:

- checks that `Shared/*.c` builds for DriverKit and uses nothing beyond libSystem and the two platform hooks;
- checks that `Shared/xpad_xbox_desc.c` is what `Tests/gen_xbox_desc.py` makes from the captured descriptor, and that `Tests/auth_vectors.h` is what `Tests/gen_auth_vectors.py` makes from the committed test keys (cross-checked with `openssl`; skipped without python3 or openssl);
- checks that every personality in `Driver/Info.plist` uses a key combination IOUSBHostFamily matches, and that the driver's entitlements are exactly the four it needs;
- runs the unit tests, built with the address and undefined-behaviour sanitizers: the descriptor, the Xbox report and scaling, the Xbox 360 messages, the GIP host replayed from a real bring-up capture (`docs/research/gip_bringup_capture.log`) plus fuzzing, the crypto primitives against published vectors, and the v1 and v2 authentication handshakes against a simulated pad.

The finer targets are listed at the top of `Tests/Makefile`: for example `make -C Tests test` runs the unit tests only, `SAN=` builds them without the sanitizers, and `XPAD_TEST_VERBOSE=1` prints what the GIP code logs.

## Credits

- **[AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5)** by **elmonomalvad0**, and its **[AnyPad PS4](https://github.com/Vld439/AnyPad-PS4)** port: `Shared/gip.c` and `Shared/gip.h` are derived from a GIP module written on top of AnyPad PS4 and never published in that project (the bring-up state machine, vendor quirks, acknowledgements, chunked frames, module filtering). The module is kept verbatim in `docs/research/anypad_gip.c.orig` and `anypad_gip.h.orig`, with the AnyPad `pad.h` it uses in `anypad_pad.h.orig`; see [`docs/research/README.md`](docs/research/README.md).
- **[xone](https://github.com/medusalix/xone)** by **Severin von Wnuck-Lipinski** and contributors (maintained fork: [dlundqvist/xone](https://github.com/dlundqvist/xone)), GPL-2.0-or-later: `Shared/gip_auth.c` (the host side of GIP authentication) and the PRF in `Shared/gip_crypto.c` are derived from `auth/auth.c` and `auth/crypto.c`; also the chunked framing and acknowledgement rules, the serial-number request, and the audio-interface reset.
- **[Linux xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)**: the GIP and Xbox 360 input layouts, the Share button position, the 360 init and LED messages, the vendor list.
- **[SDL](https://github.com/libsdl-org/SDL)**: the HIDAPI Xbox One and Xbox 360 drivers (the identification step, firmware-dependent input layouts), VID/PID lists.
- **[xpadneo](https://github.com/atar-axis/xpadneo)**: the Xbox Wireless Controller's 283-byte "unified" BLE report descriptor and the rumble report format (motor enable bits).
- Apple DTS's answers on the [Developer Forums](https://developer.apple.com/forums/thread/809202) about the Development variants of the DriverKit entitlements.

See [`NOTICE`](NOTICE) for the full attributions.

## License

XPadMac is released under the **GNU General Public License, version 3 or (at your option) any later version** (GPL-3.0-or-later); see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). It must be: `Shared/gip.c` derives from AnyPad's GPL-3.0-or-later code and `Shared/gip_auth.c` from xone's GPL-2.0-or-later code. If you distribute your own version, binaries included, provide its source code, keep the copyright notices, release your changes under the same license, and credit the original projects.

## Trademarks

Xbox, Xbox One, Xbox Series X|S and Xbox Wireless Controller are trademarks of Microsoft Corporation. Other product and company names are trademarks of their respective owners. XPadMac is an independent project and is **not affiliated with, endorsed by, or certified by Microsoft, Apple, or any controller vendor**.
