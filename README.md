# MSI-EC-TOOLBOX

Hackintosh kext + menu bar for **MSI Modern 15 A10M** (1551EMS1 / CONF_G1_5).  
Tested: macOS 14 Sonoma — Lilu 1.7.x, VirtualSMC 1.3.x.

---

## Project Structure

The **debug kexts** for Lilu and VirtualSMC, as well as MacKernelSDK, should be placed **directly in `MSI-EC-TOOLBOX/`**, at the same level as the subprojects.

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
│       ├── MSIECToolbox.h / .cpp          Lilu Plugin, hook EC, fallback raw I/O
│       ├── MSIECToolboxDriver.h / .cpp    IOService minimal (provider UserClient)
│       ├── MSIECToolboxUserClient.h / .cpp Dispatch IOKit table
│       ├── MSIECToolboxShared.h           EC Registers, structs
│       └── Info.plist
├── SMCMSIFan/              ← VirtualSMC Plugin (RPM fan → SMC Keys)
│   ├── SMCMSIFan.xcodeproj/
│   └── Sources/
│       ├── SMCMSIFan.h / .cpp             Polling EC 0xCC–0xCD → F0Ac / FNum
│       ├── SMCMSIFanKeys.h                SMC keys (F0Ac, F0Mn, F0Mx, FNum)
│       └── Info.plist
├── LaunchAgent/            ← App (Swift, macOS userspace)
│   ├── MSIECToolboxAgent.swift
│   └── com.msi.MSIECToolboxAgent.plist    LaunchAgent plist
└── ACPI/
    └── SSDT-MSI-KEY_FIX.dsl              Key Remapping PS2 → ADB (VoodooPS2)
```

---

## Dépendency

| Dependency | Source | Usage |
|---|---|---|
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | `git clone` |
| [Lilu **DEBUG**](https://github.com/acidanthera/Lilu/releases) | zip Release → `Lilu.kext` |
| [VirtualSMC **DEBUG**](https://github.com/acidanthera/VirtualSMC/releases) | zip Release → `VirtualSMC.kext` |
| [VoodooPS2Controller](https://github.com/acidanthera/VoodooPS2) | — | SSDT remapping keys|
| Xcode | 15.0+ | Compilation kext + LaunchAgent |
| [iASL / iasl](https://acpica.org/downloads) | — | Compilation SSDT `.dsl` → `.aml` |


---

### Installation des dépendances

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
# Résult : build/Release/MSIECToolbox.kext
```

### SMCMSIFan.kext

```bash
open SMCMSIFan/SMCMSIFan.xcodeproj

xcodebuild -project SMCMSIFan/SMCMSIFan.xcodeproj \
           -target SMCMSIFan \
           -configuration Release \
           build
# Résult : build/Release/SMCMSIFan.kext
```

### Loading order in OpenCore `config.plist`
```
Lilu.kext
VirtualSMC.kext
MSIECToolbox.kext
SMCMSIFan.kext     
```

---

## Compiling and installing the LaunchAgent

### 1. Compile

```bash
cd LaunchAgent

swiftc MSIECToolboxAgent.swift \
  -o MSIECToolboxAgent \
  -framework Foundation \
  -framework AppKit \
  -framework CoreAudio \
  -framework IOKit \
  -framework CoreGraphics \
  -framework UserNotifications \
  -O
```

### 2. Install (LaunchAgent)

```bash
chmod +x build_and_install.sh
sudo ./build_and_install.sh
```

### Accessibility Permission (required for CGEventTap)
The LaunchAgent intercepts keystrokes via `CGEvent.tapCreate`. macOS requires explicit permission:

```
System Settings › Privacy & Security › Accessibility
→ Add MSIECToolboxAgent ✅
```

Without this permission, keyboard interception is disabled, but the menu bar functions (fan mode, shift, cooler boost) remain active.

---

## EC Register — MSI Modern 15 A10M (1551EMS)

All these registers are accessible via ACPI raw port I/O: command `0x66`, data `0x62`.  
Source: msi-ec BeardOverflow + personal EC RW-Everything dump.

### Modes and Performance

| Register | Values | Description ||---|---|---|
| `0xF4` | `0x0D`=auto / `0x1D`=silent / `0x8D`=advanced | Mode fan |
| `0x98` bit 7 | `0x80`=ON / `0x00`=OFF (masque bit 7) | Cooler Boost (fans 100%) |

> **Cooler Boost :** toujours lire `0x98`, masquer le bit 7, réécrire. Les autres bits
> contiennent des valeurs firmware persistantes (`0x02`/`0x03`/`0x05` observés dans les dumps).

### Lecture temps réel

| Registers | Content | Note |
|---|---|---|
| `0x68` | CPU Temp (°C direct) | |
| `0x71` | CPU Fan Speed (%, 0–150) | 0 = fan off (< 50°C) |
| `0x80` | GPU Temperature (°C direct) | 0 if iGPU inactive |
| `0x89` | GPU fan speed (%) | Residual value on A10M (iGPU only), unreliable |
| `0xCC–0xCD` | CPU fan RPM (big-endian) | ISW formula below |
| `0xCA–0xCB` | GPU fan RPM (big-endian) | 0 on A10M |

**RPM Formula (ISW)** :
```
val = (0xCC << 8) | 0xCD
si val == 0 : fan stopped
RPM = ((325 - val) * 16) + 1480
```
### CPU Fan Curve — Advanced Mode (0x6A–0x78)

The curve is active only if `0xF4 = 0x8D` (advanced mode).  
Atomic write required: all 13 registers or none.

| Registers | Role | Default firmware values |
|---|---|---|
| `0x6A`–`0x6F` | Temperature thresholds (6 points) | 50, 58, 65, 70, 90, 95 °C |
| `0x72`–`0x77` | Fan speeds (6 points) | 0, 58, 65, 72, 80, 85% |
| `0x78` | Point 6 — Fixed 100% | 0x64 (do not modify) |

**Validation constraints before writing:**
- Strictly increasing temperatures: `T[n] < T[n+1]`
- Increasing or equal speeds: `V[n] ≤ V[n+1]`
- Ranges: temperatures 0–100°C, speeds 0–100%

### GPU fan curve (0x82–0x90)

Present in the EC but with no measurable effect on A10M (iGPU only, 1 physical fan).

| Registers | Firmware values |
|---|---|
| `0x82`–`0x87` (temp thresholds) | 50, 60, 70, 82, 90, 93 °C |
| `0x8A`–`0x8F` (speeds) | 45, 50, 65, 72, 80, 85% |
| `0x90` (fixed point) | 0x64 = 100% |

### Battery Charge (0xEF)

| Value | Behavior |
|---|---|
| `0x64` (100) | Fully charged (default) |
| `0xBC` (188) | Stop at 60% — MSI Center Super Battery mode (confirmed by dump) |
| `0x50` (80)  | Stop at 80% — to be validated by dump before use |

> Non-linear encoding — validate each threshold with an EC dump before displaying in the UI.

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
iasl -ve SSDT-MSI-KEY_FIX.dsl
# Generates: SSDT-MSI-KEY_FIX.aml
```
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
| `e071` | F5 mute mic | `0x4F` (ADB F14) | keycode 79 → toggle mic mute |
| `e072` | F12 rotation | `0x6F` (ADB) | keycode 111 → rotate screen 180° |
| `e06e` | F6 camera | `0x76` (ADB) | keycode 118 → toggle camera |
| `e077` | Volume − | `0x6B` (ADB) | natively supported by macOS |
| `e078` | Volume + | `0x71` (ADB) | natively supported by macOS |
| `e037` | Snapshot | `0x64` (PS2→PS2) | remapped to Screenshot |

> The LaunchAgent intercepts keycodes 79, 111, and 118 via `CGEvent.tapCreate`
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
     └─ SMCMSIFan.kext loaded
         └─ F0Ac / FNum / F0Mn / F0Mx published in VirtualSMC
         └─ readCpuRPM() called on every SMC read

Login
 └─ LaunchAgent started via com.msi.MSIECToolboxAgent.plist
     └─ IOKit UserClient connected to MSIECToolboxDriver
     └─ CoreAudio listener → mute changes → kext → EC
     └─ CGEventTap → F5/F6/F12 → direct actions
     └─ Poll 500ms via getSystemState (selector 9)
         └─ Menu bar update (temp, fan%, modes)
```

---

## Available Bootargs

| Bootarg | Effect |
|---|---|
| `-msiec.off` | Disables MSIECToolbox |
| `-msiec.dbg` | Enables DEBUG logs in Console.app |
| `-msiec.beta` | Forces loading on unsupported macOS versions |
| `-smcmsifan.off` | Disables SMCMSIFan |
| `-smcmsifan.dbg` | Enables SMCMSIFan DEBUG logs |
