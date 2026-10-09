# MSI-EC-TOOLBOX

> **Caution — use at your own risk.** This project targets the MSI Modern 15 A10M. It may work on other MSI Modern laptops, but check your EC registers first (see the CLI dump tool below).

Hackintosh kext + menu bar for **MSI Modern 15 A10M** (1551EMS1 / CONF_G1_5).  
Tested: macOS 14 Sonoma — Lilu 1.7.x, VirtualSMC 1.3.x.

---

## Project Structure

The **DEBUG** releases of Lilu and VirtualSMC, as well as MacKernelSDK, must be placed **directly in `MSI-EC-TOOLBOX/`**, at the same level as the subprojects.

- `Lilu.kext` and `VirtualSMC.kext`: download the **DEBUG** zip from the GitHub Releases.
- `MacKernelSDK`

```
MSI-EC-TOOLBOX/
├── Lilu.kext               ← zip DEBUG Lilu (bundle, headers inside)
├── VirtualSMC.kext         ← zip DEBUG VirtualSMC (bundle, headers inside)
├── MacKernelSDK/           ← git clone acidanthera/MacKernelSDK
├── MSIECToolbox/
│   ├── MSIECToolbox.xcodeproj/
│   └── Sources/
│       ├── MSIECToolbox.h / .cpp          Lilu plugin (writeECField hook) + MSIECCore (all EC access)
│       ├── MSIECToolboxDriver.h / .cpp    IOService on the EC (PNP0C09): UserClient provider, SMCMSIFan entry points
│       ├── MSIECToolboxUserClient.h / .cpp IOKit selector dispatch
│       ├── MSIECToolboxShared.h           EC registers, selectors, structs (shared by both kexts)
│       └── Info.plist
├── SMCMSIFan/              ← VirtualSMC Plugin (RPM fan → SMC Keys)
│   ├── SMCMSIFan.xcodeproj/
│   └── Sources/
│       ├── SMCMSIFan.h / .cpp             EC polling (fans, iGPU temp, charge limit) → SMC keys
│       ├── SMCMSIFanKeys.h                SMC keys (BCLM, F0xx, F1xx, FNum, TG0P)
│       └── Info.plist
├── LaunchAgent/            ← App (Swift, macOS userspace)
│   ├── Sources/                           Agent sources (main.swift = entry point)
│   ├── MSIECToolboxInstaller.swift        SMAppService register/unregister helper
│   ├── agent.entitlements                 Hardened runtime entitlements (microphone access)
│   └── build_and_install.sh               Build, sign, install, register
├── CLI/
│   ├── MSIECToolboxDump.swift             EC register dump tool
│   └── Makefile
└── ACPI/
    └── SSDT-MSI-KEY_FIX.dsl               Key remapping PS2 → ADB (VoodooPS2)
```

---

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | `git clone` | Kernel headers for both kexts |
| [Lilu **DEBUG**](https://github.com/acidanthera/Lilu/releases) | Release zip → `Lilu.kext` | Plugin API (MSIECToolbox, SMCMSIFan) |
| [VirtualSMC **DEBUG**](https://github.com/acidanthera/VirtualSMC/releases) | Release zip → `VirtualSMC.kext` | SMC plugin SDK (SMCMSIFan) |
| [VoodooPS2Controller](https://github.com/acidanthera/VoodooPS2) | OpenCore kext | Applies the SSDT key remapping |
| Xcode | 15.0+ | Builds the kexts, the agent and the CLI |
| [iASL](https://acpica.org/downloads) | `brew install acpica` | Compiles the SSDT `.dsl` → `.aml` |
| [displayplacer](https://github.com/jakehilborn/displayplacer) | `brew install displayplacer` | Screen rotation (F12), expected at `/usr/local/bin/displayplacer` |

---

### Installing the dependencies

```bash
cd MSI-EC-TOOLBOX

# MacKernelSDK
git clone https://github.com/acidanthera/MacKernelSDK

# Lilu — download the DEBUG zip from GitHub Releases
# https://github.com/acidanthera/Lilu/releases → Lilu-X.X.X-DEBUG.zip
# Extract to MSI-EC-TOOLBOX/

# VirtualSMC — download the DEBUG zip from GitHub Releases
# https://github.com/acidanthera/VirtualSMC/releases → VirtualSMC-X.X.X-DEBUG.zip
#  Extract to MSI-EC-TOOLBOX/
```

## Compilation

### MSIECToolbox.kext

```bash
# Open the project
open MSIECToolbox/MSIECToolbox.xcodeproj

# Or compile from the command line (Release)
xcodebuild -project MSIECToolbox/MSIECToolbox.xcodeproj \
           -target MSIECToolbox \
           -configuration Release \
           build
# Result: MSIECToolbox/build/Release/MSIECToolbox.kext
```

### SMCMSIFan.kext

```bash
open SMCMSIFan/SMCMSIFan.xcodeproj

xcodebuild -project SMCMSIFan/SMCMSIFan.xcodeproj \
           -target SMCMSIFan \
           -configuration Release \
           build
# Result: SMCMSIFan/build/Release/SMCMSIFan.kext
```

### Loading order in OpenCore `config.plist`
```
Lilu.kext
VirtualSMC.kext
MSIECToolbox.kext
SMCMSIFan.kext
```

Rebuilding a kext requires copying it to `EFI/OC/Kexts/` and rebooting. The agent can be rebuilt without a reboot.

---

## Compiling and installing the LaunchAgent

### 1. Compile (optional — the install script compiles too)

```bash
cd LaunchAgent

swiftc Sources/*.swift \
  -module-name MSIECToolboxAgent \
  -o MSIECToolboxAgent \
  -framework Foundation \
  -framework AppKit \
  -framework CoreAudio \
  -framework IOKit \
  -framework CoreGraphics \
  -O
```

### 2. Install (LaunchAgent)

Run it **without sudo**, from the account that will use the agent: it registers the agent in your session and asks for your password only for the copies into `/Applications` and `/Library`.

```bash
chmod +x build_and_install.sh
./build_and_install.sh
```

If MSIECToolbox appears as *waiting for approval*, enable it in System Settings › General › Login Items.

The agent is signed with the hardened runtime. By default the signature is ad-hoc, which means the Accessibility permission below must be granted again after every rebuild. To keep it across rebuilds, create a code signing certificate once (Keychain Access › Certificate Assistant › Create a Certificate, identity type *Self-Signed Root*, certificate type *Code Signing*) and pass its name:

```bash
SIGN_IDENTITY="MSIECToolbox Local" ./build_and_install.sh
```

Agent logs: `log stream --predicate 'process == "MSIECToolboxAgent"'`.

### Accessibility Permission (required for CGEventTap)
The LaunchAgent intercepts keystrokes via `CGEvent.tapCreate`. macOS requires explicit permission:

```
System Settings › Privacy & Security › Accessibility
→ + → Cmd+Shift+G → /Library/Application Support/MSIECToolbox/MSIECToolboxAgent ✅
```

Without this permission, keyboard interception is disabled, but the menu bar functions (fan mode, shift, cooler boost) remain active, and the menu shows an Accessibility warning.

After a rebuild with a different signature (always the case with ad-hoc signing), the entry still looks enabled but no longer matches the binary: select `MSIECToolboxAgent`, remove it with **−**, then add it again with **+**. Toggling it off and on is not enough. No restart is needed: the agent picks the permission up within 10 seconds.

---

## CLI dump tool

`MSIECToolboxDump` reads the EC through the kext (run it with `sudo`). It is the main tool to check a register before and after an action.

```bash
cd CLI && make && make install     # installs to /usr/local/bin

sudo MSIECToolboxDump                  # dump of all 256 registers
sudo MSIECToolboxDump --offset 0xEF    # single register
sudo MSIECToolboxDump --watch          # continuous refresh, changed registers in red
sudo MSIECToolboxDump --diff           # two snapshots (press Enter between them)
sudo MSIECToolboxDump --json           # structured output
```

---

## EC Registers — MSI Modern 15 A10M (1551EMS1)

All these registers are accessed via ACPI port I/O: command `0x66`, data `0x62`.
Sources: [msi-ec](https://github.com/BeardOverflow/msi-ec) (`CONF_G1_5`) and EC dumps of this laptop.

### Modes and Performance

| Register | Values | Description |
|---|---|---|
| `0xF4` | `0x0D`=auto / `0x1D`=silent / `0x8D`=advanced | Fan mode |
| `0x98` bit 7 | `0x80`=ON / `0x00`=OFF (bit 7 mask) | Cooler Boost (fans 100%) |
| `0xF2` | `0xC0`=Turbo / `0xC1`=Comfort / `0xC2`=Eco | Shift mode |
| `0xF3` | `0x80`=off … `0x83`=high | Keyboard backlight |

> **Cooler Boost:** always read `0x98`, mask bit 7, then write back. The other bits
> contain persistent firmware values (`0x02`/`0x03`/`0x05` observed in the dumps).

| Registers | Content | Note |
|---|---|---|
| `0x68` | CPU Temp (°C direct) | |
| `0x71` | CPU Fan Speed (%, 0–150) | 0 = fan off (< 50°C) |
| `0x80` | GPU Temperature (°C direct) | 0 if iGPU inactive |
| `0x89` | GPU fan speed (%) | Second fan (the A10M has two fans) |
| `0xCC–0xCD` | CPU fan RPM (big-endian) | ISW formula below |
| `0xCA–0xCB` | GPU fan RPM (big-endian) | Second fan, same ISW formula |

**RPM Formula (ISW)**:
```
val = (0xCC << 8) | 0xCD
if val == 0: fan stopped
RPM = ((325 - val) * 16) + 1480
```

### Mute LEDs and camera

| Register | Values | Description |
|---|---|---|
| `0x2B` | base `0x80`, bit `0x04` = LED on | Microphone mute LED |
| `0x2C` | base `0xE0`, bit `0x04` = LED on | Speaker mute LED |
| `0x2E` | `0x4B`=on / `0x49`=off | Webcam |

The LED bits are always written with a read-modify-write. On Windows, these LEDs only light up when MSI Creator Center is installed: the firmware does not drive them itself, the application writes the bit. On macOS the agent does the same.

### Fn / Windows key swap (0xBF)

| Register | Values | Description |
|---|---|---|
| `0xBF` bit 4 | `0x10` set = swapped | Fn and Windows keys swapped by the EC (MSI Creator Center option) |

The setting survives a reboot and the EC applies it under macOS too: the Windows key (used as Command) becomes Fn, and Command is lost. MSIECToolbox therefore clears the bit when macOS starts and sets it back at shutdown or restart, so Windows keeps the Creator Center choice. On macOS, `sudo MSIECToolboxDump --offset 0xBF` should read `0x00`, and `ioreg -l | grep FnWinSwap` tells what the kext found at boot (`not set at boot`, or `cleared at boot, restored at shutdown/restart`). On Windows, after a normal restart from macOS, the keys are swapped again.

After a kernel panic or a forced power-off the bit is not restored: re-enable the swap in Creator Center if needed. Check the current state with `sudo MSIECToolboxDump --offset 0xBF`.

### CPU Fan Curve — Advanced Mode (0x6A–0x78)

The curve is active only if `0xF4 = 0x8D` (advanced mode).
The kext writes the 12 editable registers (`0x6A`–`0x6F` and `0x72`–`0x77`) in one locked sequence; `0x78` is never written.

| Registers | Role | Default firmware values |
|---|---|---|
| `0x6A`–`0x6F` | Temperature thresholds (6 points) | 50, 58, 65, 70, 90, 95 °C |
| `0x72`–`0x77` | Fan speeds (6 points) | 0, 58, 65, 72, 80, 85% |
| `0x78` | Point 6 — Fixed 100% | 0x64 (do not modify) |

**Validation constraints (checked by the kext, a refused curve is not written):**
- Strictly increasing temperatures: `T[n] < T[n+1]`
- Increasing or equal speeds: `V[n] ≤ V[n+1]`
- Ranges: temperatures 20–95 °C, speeds 0–100 %
- Thermal floor: at least 50 % from 70 °C, and on the last point

### GPU fan curve (0x82–0x90)

Curve of the second fan. Its effect has not been validated yet: do not write it without an EC dump.

| Registers | Firmware values |
|---|---|
| `0x82`–`0x87` (temp thresholds) | 50, 60, 70, 82, 90, 93 °C |
| `0x8A`–`0x8F` (speeds) | 45, 50, 65, 72, 80, 85% |
| `0x90` (fixed point) | 0x64 = 100% |

### Battery Charge (0xEF)

Same encoding as the Linux [msi-ec](https://github.com/BeardOverflow/msi-ec) driver for `CONF_G1_5`: bit 7 enables the limit, bits 0–6 hold the stop percentage.

| Value | Behavior |
|---|---|
| `0x64` (bit 7 clear) | No limit, full charge (firmware default) |
| `0xD0` (`0x80 \| 80`) | Stop at 80% — set by the menu item |
| `0xBC` (`0x80 \| 60`) | Stop at 60% — MSI Center Super Battery mode (confirmed by dump) |

SMCMSIFan also publishes this limit as the SMC key `BCLM` (read/write, 10–100 %), so macOS tools that set the charge limit of Intel Macs (AlDente, `bclm`) drive the EC directly. A write is applied within a second; the agent's menu picks it up while it is open. macOS's own "Optimized Battery Charging" does not use `BCLM` and has no effect on this laptop's charger.

---

## SSDT-MSI-KEY_FIX.dsl

### Purpose

This ACPI hotpatch remaps the PS2 scancodes of the Fn keys on the MSI Modern 15 to ADB keycodes that macOS understands. It is processed by **VoodooPS2Controller** at boot.

Without this SSDT, the MSI Fn keys do not generate usable events in macOS.

### Compilation

```bash
# Install iASL (included in MaciASL or via brew)
brew install acpica

# Compile the SSDT
iasl -ve ACPI/SSDT-MSI-KEY_FIX.dsl
# Generates: SSDT-MSI-KEY_FIX.aml
```

The `.dsl` must stay pure ASCII: macOS `iasl` rejects accents, arrows or em dashes, even in comments.
### Installation

Copy `SSDT-MSI-KEY_FIX.aml` to `EFI/OC/ACPI/` and add it to `config.plist`:

```xml
<dict>
    <key>Path</key>
    <string>SSDT-MSI-KEY_FIX.aml</string>
    <key>Enabled</key>
    <true/>
    <key>Comment</key>
    <string>MSI Modern 15 — remapping Fn PS2 keys to ADB</string>
</dict>
```

### Key mapping

| PS2 scancode | Physical key | ADB keycode | CGEventTap (agent) |
|---|---|---|---|
| `e071` | F5 mute mic | `0x4F` (ADB F18) | keycode 79 → toggle mic mute |
| `e072` | F12 rotation | `0x6F` (ADB F12) | keycode 111 → rotate the screen (0°↔180°, or 90° steps — see Preferences) |
| `e06e` | F6 camera | `0x50` (ADB F19) | keycode 80 → toggle camera (was `0x76` = keycode 118, the standard F4: Fn+F4 toggled the camera) |
| `76` (F24, with Ctrl+Win) | F4 touchpad | `0x5A` (ADB F20) | keycode 90 → toggle touchpad (VoodooI2C/VoodooPS2, via the kext) |
| `e077` | Volume − | `0x6B` (ADB) | natively supported by macOS |
| `e078` | Volume + | `0x71` (ADB) | natively supported by macOS |
| `e037` | Snapshot | `0x64` (PS2→PS2) | remapped to Screenshot |
| — | F8 backlight | standard F8 | keycode 100 → cycle keyboard backlight |

> The LaunchAgent intercepts keycodes 79, 111, 80, 90 and 100 via `CGEvent.tapCreate`
> at the session level (`cgSessionEventTap`). The **Accessibility** permission is
> required for this tap to be active.

---

## General Operation

```
Boot OpenCore
 └─ Lilu.kext loaded
     └─ MSIECToolbox.kext loaded
         └─ pluginStart() → hook IOACPIPlatformDevice::writeECField
         └─ MSIECToolboxDriver (IOService) published → UserClient available
     └─ SMCMSIFan.kext loaded (VirtualSMC plugin)
         └─ BCLM / F0xx (CPU fan) / F1xx (GPU fan) / FNum=2 / TG0P published in VirtualSMC (TC0P comes from SMCProcessor)
         └─ EC sampled every 1s through MSIECToolboxDriver (shared EC lock), SMC reads return the cache

Login
 └─ LaunchAgent started via com.msi.MSIECToolboxAgent.plist
     └─ IOKit UserClient connected to MSIECToolboxDriver
     └─ CoreAudio listener → mute changes → kext → EC (mute LEDs)
     └─ CGEventTap → F4/F5/F6/F8/F12 → direct actions
     └─ EC poll, menu closed: every 1 s, mute and camera state only
     └─ EC poll, menu open: every 500 ms (temperatures, fan %, modes, backlight),
        fan RPM and charge limit every 2 s
```

---

## Available Bootargs

| Bootarg | Effect |
|---|---|
| `-msiec.off` | Disables the Lilu part (writeECField hook); the EC driver, the agent and SMCMSIFan keep working |
| `-msiec.dbg` | Enables MSIECToolbox verbose logs (DEBUG build) |
| `-msiec.beta` | Forces loading on unsupported macOS versions |
| `-smcmsifan.off` | Disables SMCMSIFan |
| `-smcmsifan.dbg` | Enables SMCMSIFan verbose logs (DEBUG build) |
