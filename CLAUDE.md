# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Hackintosh driver suite for MSI Modern 15 A10M laptops (BIOS 1551EMS1 / EC config CONF_G1_5), tested on macOS 14 Sonoma. The project has four components that interact across kernel and user space:

1. **MSIECToolbox.kext** — Lilu plugin (kernel): hooks `IOACPIPlatformDevice::writeECField`, exposes an IOUserClient for user-space EC access
2. **SMCMSIFan.kext** — VirtualSMC plugin (kernel): reads fan RPM/temps from EC registers and publishes them as SMC keys
3. **LaunchAgent** (Swift, user space): menu bar app with CGEventTap keyboard interception and CoreAudio mute sync
4. **CLI** (Swift): EC register dump tool for diagnostics
5. **ACPI/SSDT-MSI-KEY_FIX.dsl**: PS2→ADB key remapping hotpatch for VoodooPS2Controller

## Required Dependencies (not in repo)

Place these at `MSI-EC-TOOLBOX/` root before compiling kexts:

```bash
# MacKernelSDK (required by both kexts)
git clone https://github.com/acidanthera/MacKernelSDK

# Lilu and VirtualSMC — must be DEBUG builds
# Download from GitHub Releases and extract to MSI-EC-TOOLBOX/
# Lilu-X.X.X-DEBUG.zip → Lilu.kext/
# VirtualSMC-X.X.X-DEBUG.zip → VirtualSMC.kext/
```

## Build Commands

### Kexts (requires Xcode 15+)

```bash
# MSIECToolbox.kext → build/Release/MSIECToolbox.kext
xcodebuild -project MSIECToolbox/MSIECToolbox.xcodeproj \
           -target MSIECToolbox -configuration Release build

# SMCMSIFan.kext → build/Release/SMCMSIFan.kext
xcodebuild -project SMCMSIFan/SMCMSIFan.xcodeproj \
           -target SMCMSIFan -configuration Release build
```

### LaunchAgent (Swift)

```bash
# Manual compile
cd LaunchAgent
swiftc MSIECToolboxAgent.swift \
  -o MSIECToolboxAgent \
  -framework Foundation -framework AppKit \
  -framework CoreAudio -framework IOKit \
  -framework CoreGraphics -framework UserNotifications \
  -O

# Or compile + register with SMAppService (preferred)
sudo ./build_and_install.sh
```

### CLI Dump Tool

```bash
cd CLI && make          # builds MSIECToolboxDump
make install            # copies to /usr/local/bin
make clean
```

### ACPI SSDT

```bash
brew install acpica     # provides iasl
iasl -ve ACPI/SSDT-MSI-KEY_FIX.dsl   # → SSDT-MSI-KEY_FIX.aml
```

## Architecture — Cross-Component Data Flow

```
Boot (OpenCore)
  Lilu.kext
    └─ MSIECToolbox.kext
         pluginStart() → hook writeECField (intercepts ACPI EC writes system-wide)
         MSIECToolboxDriver (IOService, matches PNP0C09) → publishes UserClient
    └─ SMCMSIFan.kext
         readCpuRPM() polls EC 0xCC–0xCD on every VirtualSMC SMC read
         publishes F0Ac, F0Mn, F0Mx, FNum, TC0P, TG0P

Login
  LaunchAgent (com.msi.MSIECToolboxAgent)
    IOKit open MSIECToolboxDriver → UserClient (16 selectors)
    CoreAudio listener → system mute changes → selector setMuteState → EC 0x2B/0x2C (LED bit 0x04)
    CGEventTap (requires Accessibility) → keycodes 79/111/118 → direct actions
    500ms poll → selector getSystemState (9) → menu bar update
```

**Key architectural constraint**: SMCMSIFan reads the EC directly via raw port I/O (ports `0x62`/`0x66`) — it does NOT go through MSIECToolbox's UserClient. Both kexts use `ecLock` independently within their own scope.

**UserClient selectors** (defined in `MSIECToolboxShared.h`, dispatched in `MSIECToolboxUserClient.cpp`):
0=setMuteState, 1=getMuteState (reserved, superseded by 4), 2=dumpEC, 3=setCameraState, 4=getAllState, 5=readFanRPM, 6=setFanMode, 7=setCoolerBoost, 8=setShiftMode, 9=getSystemState, 10=setKbBacklight, 11=getKbBacklight, 12=setBatteryCharge, 13=getBatteryCharge, 14=setFanCurve, 15=getFanCurve

## EC Register Reference (CONF_G1_5)

All registers accessed via ACPI port I/O: command port `0x66`, data port `0x62`.

| Register | Values | Purpose |
|----------|--------|---------|
| `0xF4` | `0x0D`=auto / `0x1D`=silent / `0x8D`=advanced | Fan mode |
| `0x98` bit 7 | `0x80`=ON / `0x00`=OFF | Cooler Boost — always read-modify-write |
| `0x68` | direct °C | CPU temp |
| `0x71` | 0–150% | CPU fan speed % |
| `0xCC–0xCD` | big-endian | CPU fan RPM (ISW formula: `RPM = ((325 - val) * 16) + 1480`) |
| `0xF3` | `0x80–0x83` | Keyboard backlight (0x80=off, 0x83=high) |
| `0xEF` | `0x64`/`0x50`/`0xBC` | Battery charge limit (100%/80%/60%) — non-linear encoding |
| `0xF2` | `0xC0`/`0xC1`/`0xC2` | Shift mode (Turbo/Comfort/Eco) |
| `0x6A–0x6F` | temps °C | CPU fan curve temp thresholds (6 breakpoints) |
| `0x72–0x77` | speed % | CPU fan curve speed targets (6 breakpoints) |
| `0x78` | `0x64` | CPU fan curve fixed point 6 (100%, do not modify) |

**Fan curve write rule**: All 13 registers (`0x6A–0x6F` + `0x72–0x78`) must be written atomically. Validation: temperatures strictly increasing, speeds non-decreasing, temp 0–100°C, speed 0–100%.

**Cooler Boost write rule**: Always read `0x98` first, mask/unmask only bit 7, write back — bits 0–6 contain persistent firmware state.

## Code Conventions

- **Constants**: `k` prefix, `constexpr` in C++ (`kMSI_EC_*`, `kECCommandPort`, etc.)
- **Thread safety**: `IOLock` for `stateLock` (mute state) and `ecLock` (EC bus); atomic variables with `memory_order_acquire/release` for cross-thread visibility
- **Logging**: Conditional on `-msiec.dbg` / `-smcmsifan.dbg` bootargs; use `DBGLOG`/`SYSLOG` macros from Lilu
- **IOReturn**: All UserClient selectors return `IOReturn`; check against `kIOReturnSuccess`
- **Shared header**: `MSIECToolboxShared.h` is included by both kernel C++ and user-space Swift (via bridging); keep it C-compatible (`#ifndef` guard, no C++ types)

## OpenCore Load Order

```
Lilu.kext → VirtualSMC.kext → MSIECToolbox.kext → SMCMSIFan.kext
```

## Debugging

| Bootarg | Effect |
|---------|--------|
| `-msiec.dbg` | MSIECToolbox verbose logs in Console.app |
| `-msiec.beta` | Force-load on unsupported macOS versions |
| `-msiec.off` | Disable MSIECToolbox entirely |
| `-smcmsifan.dbg` | SMCMSIFan verbose logs |
| `-smcmsifan.off` | Disable SMCMSIFan entirely |

The CLI dump tool is the primary debugging aid for EC register changes:
```bash
MSIECToolboxDump                  # single dump of all 256 registers
MSIECToolboxDump --watch          # continuous polling
MSIECToolboxDump --watch --diff   # show only changed registers
MSIECToolboxDump --json           # structured output
```
