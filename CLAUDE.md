# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Hackintosh driver suite for MSI Modern 15 A10M laptops (BIOS 1551EMS1 / EC config CONF_G1_5), tested on macOS 14 Sonoma. The project has four components that interact across kernel and user space:

1. **MSIECToolbox.kext** — Lilu plugin (kernel): hooks `IOACPIPlatformDevice::writeECField`, exposes an IOUserClient for user-space EC access
2. **SMCMSIFan.kext** — VirtualSMC plugin (kernel, not a Lilu plugin): samples fan RPM/temps through MSIECToolbox every second and publishes them as SMC keys
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

The Xcode projects reference these paths directly: `Lilu.kext/Contents/Resources/Library/plugin_start.cpp` (compiled into MSIECToolbox), `Lilu.kext/Contents/Resources/Headers`, `VirtualSMC.kext/Contents/Resources/VirtualSMCSDK`, `MacKernelSDK/Headers` (`KERNEL_FRAMEWORK_HEADERS`) and `MacKernelSDK/Library/x86_64/libkmod.a`.

## Build Commands

### Kexts (requires Xcode 15+)

```bash
# MSIECToolbox.kext → MSIECToolbox/build/Release/MSIECToolbox.kext
xcodebuild -project MSIECToolbox/MSIECToolbox.xcodeproj \
           -target MSIECToolbox -configuration Release build

# SMCMSIFan.kext → SMCMSIFan/build/Release/SMCMSIFan.kext
xcodebuild -project SMCMSIFan/SMCMSIFan.xcodeproj \
           -target SMCMSIFan -configuration Release build
```

Kext build settings follow the acidanthera plugin template: `ARCHS = x86_64` (inline `inb`/`outb`), `MODULE_NAME`, `MODULE_START = $(PRODUCT_NAME)_kern_start`, `MODULE_STOP`, `MODULE_VERSION` (single source of the version, also used by Info.plist), and `PRODUCT_NAME` / `MODULE_VERSION` passed as preprocessor macros (required by Lilu's `ADDPR` / `xStringify`).

There are no tests and no CI. Kernel sources can be syntax-checked off-Mac with clang against MacKernelSDK + Lilu/VirtualSMC headers (`-target x86_64-apple-macos10.15 -mkernel -fapple-kext -nostdinc -DKERNEL -DPRODUCT_NAME=… -DMODULE_VERSION=…`), but only `xcodebuild` on macOS produces a loadable kext.

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

# Or compile + sign + register with SMAppService (preferred).
# Run WITHOUT sudo (it refuses root): SMAppService registers the agent in the
# caller's session; the script calls sudo itself for the copies.
./build_and_install.sh
SIGN_IDENTITY="<codesign identity>" ./build_and_install.sh   # stable signature keeps Accessibility across rebuilds
```

The script builds in a private `mktemp -d` directory, signs the agent and the installer bundle with the hardened runtime (as the user, before the sudo copy; the agent with `agent.entitlements`, whose `com.apple.security.device.audio-input` is required — without it the hardened runtime hides Core Audio input devices and the mic mute key, its OSD and the mic LED sync silently stop working), installs the agent root-owned in `/Library/Application Support/MSIECToolbox/` and the bundle in `/Applications/MSIECToolbox.app`. The LaunchAgent plist has no StandardOut/ErrorPath: logs are in the unified log (`log stream --predicate 'process == "MSIECToolboxAgent"'`).

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
         class MSIECToolbox (Lilu plugin_start.cpp, matches IOResources)
           → kern_start → pluginStart() → hook writeECField
         MSIECToolboxDriver (IOService, matches PNP0C09)
           → publishes UserClient + serves "MSIECReadRegisters" (callPlatformFunction)
  VirtualSMC.kext
    └─ SMCMSIFan.kext (IOService on IOResources)
         registerHandler → SubmitPlugin → 1s IOTimerEventSource on its own workloop
         refreshSensors() → MSIECToolboxDriver::callPlatformFunction → cache
         readAccess() returns the cache: F0Ac, F0Mn, F0Mx, FNum, TC0P, TG0P

Login
  LaunchAgent (com.msi.MSIECToolboxAgent)
    IOKit open MSIECToolboxDriver → UserClient (16 selectors)
    CoreAudio listener → system mute changes → selector setMuteState → EC 0x2B/0x2C (LED bit 0x04)
    CGEventTap (requires Accessibility) → keycodes 79/111/118/100 → direct actions
    500ms poll → selectors 4 (getAllState), 11 (getKbBacklight), 9 (getSystemState)
    2s timer   → selector 5 (readFanRPM)
```

**Agent threading**: every IOKit call goes through `MSIECToolboxClient.queue` (serial, `.utility`), usually via `client.run({ work on queue }) { result on main }`. Never call a client method from the main thread: the CGEventTap runs on the main run loop and every keystroke of the system waits for it, while a kext call can block for tens of ms on the EC. The poll/RPM timers fire on that queue and hand a snapshot to `applyPoll` on main, where all agent state (`lastSent*`, `lastCamState`, menu items) lives. Because the queue is serial, a poll queued after a write reads the written state and its result reaches main after the write's completion.

**Mute sync direction**: CoreAudio is the source of truth. EC → CoreAudio only propagates *muting*; if the EC LED reads unmuted while the agent last sent muted, the agent rewrites the LED instead of unmuting CoreAudio (any local process can write the LED bits through the kext).

**UserClient access**: `initWithTask` accepts only the console user (`kIOClientPrivilegeLocalUser`, i.e. the agent) or root (CLI under sudo).

**EC bus ownership** — the most important invariant. All EC access in both kexts goes through `MSIECCore` (`MSIECToolbox.cpp`):
- `MSIECCore::BusGuard` (RAII) takes `ecLock`, then the ACPI global lock of the PNP0C09 device (`acquireGlobalLock`, 50 ms timeout → `kIOReturnBusy`). AML takes the same global lock around EC fields declared with the `Lock` rule. If the platform has no global lock, the guard degrades to `ecLock` only (logged once).
- `ecReadLocked` / `ecWriteLocked` are the only raw RD_EC/WR_EC sequences; call them only while holding a `BusGuard`. Multi-register operations (fan curve, Cooler Boost RMW, system state, RPM hi/lo) hold one guard for the whole sequence; `dumpEC` takes one per register so the global lock is never held for 256 reads.
- SMCMSIFan never touches ports `0x62`/`0x66`: it calls `MSIECToolboxDriver::callPlatformFunction("MSIECReadRegisters", offsets, values, count)` (contract in `MSIECToolboxShared.h`). No symbol is linked across kexts; it looks the driver up by class name, so load order between the two does not matter.
- `hookedWriteECField` runs in ACPI context and must never take a `BusGuard`.
- Residual risk: AppleACPIEC's own non-AML EC traffic (SCI query handling) is not covered by the global lock.

**Naming**: the static EC class is `MSIECCore`, not `MSIECToolbox` — Lilu's `plugin_start.hpp` declares `class PRODUCT_NAME : IOService`, i.e. `class MSIECToolbox`. SMCMSIFan defines its own IOService and `kern_start`/`kern_stop` (VirtualSMC sensor template), so it does not compile `plugin_start.cpp`.

**Lilu hook (`writeECField`)** — boot-critical, a mistake here panics every boot:
- `KextInfo` must have a non-null `paths` array (`pathNum` ≥ 1). Lilu calls `loadKinfo()` on every registered `KextInfo` when its patcher starts, and `MachInfo::init()` dereferences `paths[0]` in kernel collection mode (macOS 11+). The original `{id, nullptr, 0, …}` panicked at boot as soon as the plugin actually started.
- Target is `com.apple.driver.AppleACPIPlatform` with `sys[KextInfo::Loaded] = true` (the platform expert is always loaded before Lilu plugins).
- The symbol `__ZN20IOACPIPlatformDevice12writeECFieldEjPKvl` and its (offset, value, size) semantics are inferred, not verified: if the symbol is missing the hook is skipped (logged); the value is only rewritten when `size == 1`, the offset is 0x2B/0x2C and the byte matches the firmware base value (0x80 / 0xE0, LED bit aside). DEBUG builds log the first 64 calls — check them with `-msiec.dbg` before trusting the hook.
- The hook runs in ACPI context: atomics only, no `BusGuard`, no mutex, no allocation.
- `-msiec.off` disables the whole Lilu part (hook included) while the EC driver, UserClient and SMCMSIFan keep working.

**Mute LEDs**: `setMuteState` always writes the LED bit (0x04 of 0x2B/0x2C) with a read-modify-write, whether or not the hook is installed. The hook only re-applies the bit when firmware rewrites those registers; without the direct write the agent's 500 ms poll would read the old bit and revert the CoreAudio mute.

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

**Fan curve write rule**: `setFanCurve` writes 12 registers (`0x6A–0x6F` + `0x72–0x77`) under one `BusGuard`; `0x78` is never written. Validation: temperatures strictly increasing in 20–95 °C, speeds non-decreasing in 0–100 %, plus a thermal floor (`kMSIFanCurveFloorTempC` / `kMSIFanCurveFloorSpeedPct`: ≥ 50 % from 70 °C and on the last point). The agent mirrors the floor in `FanCurveFloor` to explain refusals — keep both in sync.

**System state validity**: `MSISystemState.validMask` (`kMSIStateValid*`) flags which of the 7 reads succeeded; a cleared bit means the field is 0 and must be ignored (the agent keeps the previous value). The selector fails only when every read failed.

**Cooler Boost write rule**: Always read `0x98` first, mask/unmask only bit 7, write back — bits 0–6 contain persistent firmware state.

## Code Conventions

- **Constants**: `k` prefix, `constexpr` in C++ (`kMSI_EC_*`, `kECCommandPort`, etc.)
- **Thread safety**: `ecLock` (EC bus, via `BusGuard`) is allocated once by whichever of `pluginStart()` / `MSIECToolboxDriver::start()` runs first, and is never freed. The mute state (`speakerMuted` / `micMuted`) and other cross-thread flags are `_Atomic` with acquire/release — no mutex, because the hook reads them in ACPI context.
- **Logging**: MSIECToolbox uses its own `MSIEC_LOG` / `MSIEC_ERR` macros, compiled in by `#ifdef DEBUG` (Debug configuration), not by a bootarg; `MSIEC_ERR` is always on. SMCMSIFan uses Lilu `DBGLOG`/`SYSLOG`; `DBGLOG` needs a DEBUG build and `-smcmsifan.dbg` (or `-vsmcdbg` / `-liludbgall`).
- **IOReturn**: All UserClient selectors return `IOReturn`; check against `kIOReturnSuccess`
- **Shared header**: `MSIECToolboxShared.h` is C++ (`constexpr`, typed enums) and is included by both kexts (SMCMSIFan via a header search path to `MSIECToolbox/Sources`). It is **not** imported into Swift: the agent and the CLI mirror selectors and structs by hand, so any change to a selector or struct layout must be replicated in `LaunchAgent/MSIECToolboxAgent.swift` and `CLI/MSIECToolboxDump.swift`.

## OpenCore Load Order

```
Lilu.kext → VirtualSMC.kext → MSIECToolbox.kext → SMCMSIFan.kext
```

## Debugging

| Bootarg | Effect |
|---------|--------|
| `-msiec.dbg` | MSIECToolbox verbose logs in Console.app |
| `-msiec.beta` | Force-load on unsupported macOS versions |
| `-msiec.off` | Disable the Lilu part (writeECField hook); MSIECToolboxDriver and the UserClient still load |
| `-smcmsifan.dbg` | SMCMSIFan verbose logs |
| `-smcmsifan.off` | Disable SMCMSIFan entirely |

The CLI dump tool is the primary debugging aid for EC register changes:
```bash
MSIECToolboxDump                  # single dump of all 256 registers
MSIECToolboxDump --watch          # continuous polling, changed registers in red
MSIECToolboxDump --diff           # two snapshots (press Enter between them), changes listed
MSIECToolboxDump --offset 0x2B    # single register
MSIECToolboxDump --json           # structured output
```
