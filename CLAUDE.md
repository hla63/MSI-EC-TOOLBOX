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
swiftc Sources/*.swift -module-name MSIECToolboxAgent \
  -o MSIECToolboxAgent \
  -framework Foundation -framework AppKit \
  -framework CoreAudio -framework IOKit \
  -framework CoreGraphics \
  -O

# Or compile + sign + register with SMAppService (preferred).
# Run WITHOUT sudo (it refuses root): SMAppService registers the agent in the
# caller's session; the script calls sudo itself for the copies.
./build_and_install.sh
SIGN_IDENTITY="<codesign identity>" ./build_and_install.sh   # stable signature keeps Accessibility across rebuilds
```

The script builds in a private `mktemp -d` directory, signs the agent and the installer bundle with the hardened runtime (as the user, before the sudo copy; the agent with `agent.entitlements`, whose `com.apple.security.device.audio-input` is required — without it the hardened runtime hides Core Audio input devices and the mic mute key, its OSD and the mic LED sync silently stop working), installs the agent root-owned in `/Library/Application Support/MSIECToolbox/` and the bundle in `/Applications/MSIECToolbox.app`. The LaunchAgent plist has no StandardOut/ErrorPath: logs are in the unified log (`log stream --predicate 'process == "MSIECToolboxAgent"'`).

#### Install / rebuild checklist (user side)

The kexts and the agent are independent: rebuilding the agent never needs a reboot, rebuilding a kext does (OpenCore `EFI/OC/Kexts` + `config.plist`, see load order below). The agent needs MSIECToolbox.kext loaded to find `MSIECToolboxDriver`; without it the menu bar shows but every EC action fails.

1. **Execute permission**: the script is tracked as `100755`. If it was lost (ZIP download, copy from a FAT/exFAT volume, `git config core.fileMode false`), `./build_and_install.sh` fails with `permission denied`: run `chmod +x LaunchAgent/build_and_install.sh` (or `bash build_and_install.sh`). It must be run from a Mac with the Xcode command line tools (`swiftc`, `codesign`), as the normal user, never with `sudo`.
2. **Signing identity**: without `SIGN_IDENTITY` the signature is ad-hoc (`-`) and changes on every build. To keep Accessibility across rebuilds, create once a self-signed certificate (Keychain Access › Certificate Assistant › Create a Certificate…, identity type *Self-Signed Root*, certificate type *Code Signing*) and always pass the same name: `SIGN_IDENTITY="MSIECToolbox Local" ./build_and_install.sh`. The script checks the identity with `security find-identity -p codesigning` before compiling and stops if it is missing.
3. **Login item approval**: if the installer prints `En attente approbation`, enable MSIECToolbox in System Settings › General › Login Items (the installer opens that pane). The script only unregisters/re-registers through SMAppService when the agent's SHA-256 changed (`/Library/Application Support/MSIECToolbox/.agent_checksum`), to avoid a repeated "Background Items" notification.
4. **Accessibility after a rebuild**: the TCC entry is bound to the code signature (the designated requirement). After an ad-hoc rebuild — or any change of identity — the toggle for `MSIECToolboxAgent` still looks enabled but no longer matches the binary: `AXIsProcessTrusted()` returns false, the Fn keys do nothing, and the menu shows the Accessibility warning. Toggling it off and on is not enough. Fix: System Settings › Privacy & Security › Accessibility, select `MSIECToolboxAgent`, remove it with **−**, then **+**, `Cmd+Shift+G`, `/Library/Application Support/MSIECToolbox/MSIECToolboxAgent`, enable it. No restart is needed: the agent re-checks every 10 s and installs the key tap as soon as it is trusted. With a stable `SIGN_IDENTITY` this step is only needed once.
5. **Check**: `log stream --predicate 'process == "MSIECToolboxAgent"'` should show `Accessibilité accordée — installation du tap`; `"/Applications/MSIECToolbox.app/Contents/MacOS/MSIECToolboxInstaller" status` should print `✅ Agent actif`.

Claude cannot compile or run any of this in a Linux cloud session (no `swiftc`, no Xcode, no macOS frameworks): Swift and kext changes are only checked by reading, and the user validates them by running the script and rebooting for kexts.

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

The `.dsl` must stay **pure ASCII, comments in English**: macOS `iasl` rejects a source containing non-ASCII characters (accents, arrows, em dashes) with `Input file does not appear to be an ASL or data table source file`. Check with `grep -nP '[^\x00-\x7F]' ACPI/*.dsl` (no output expected).

## Architecture — Cross-Component Data Flow

```
Boot (OpenCore)
  Lilu.kext
    └─ MSIECToolbox.kext
         class MSIECToolbox (Lilu plugin_start.cpp, matches IOResources)
           → kern_start → pluginStart() → hook writeECField
         MSIECToolboxDriver (IOService, matches PNP0C09)
           → publishes UserClient + serves "MSIECReadRegisters" and
             "MSIECSetBatteryCharge" (callPlatformFunction)
  VirtualSMC.kext
    └─ SMCMSIFan.kext (IOService on IOResources)
         registerHandler → SubmitPlugin → 1s IOTimerEventSource on its own workloop
         refreshSensors() → MSIECToolboxDriver::callPlatformFunction → cache
         readAccess() returns the cache: F0Ac/ID/Md/Mn/Mx (fan "CPU", EC 0xCC-0xCD),
         F1Ac/ID/Md/Mn/Mx (fan "GPU", EC 0xCA-0xCB), FNum = 2, TG0P, BCLM (EC 0xEF)
         (FxMd read-only: 1 while Cooler Boost forces the fans)
         BCLM is writable (AlDente, bclm): update() runs in VirtualSMC's MMIO/PMIO
         trap and only stores the request; the next tick writes EC 0xEF, then reads it
         (TC0P is left to SMCProcessor — never publish a key another plugin owns)

Login
  LaunchAgent (com.msi.MSIECToolboxAgent)
    IOKit open MSIECToolboxDriver → UserClient (16 selectors)
    CoreAudio listener → system mute changes → selector setMuteState → EC 0x2B/0x2C (LED bit 0x04)
    CGEventTap (requires Accessibility) → keycodes 79/111/80/90/100 → direct actions
    EC poll, menu closed → selector 4 (getAllState) every 1 s (icon + mute sync)
    EC poll, menu open   → immediate, then 4 + 11 (getKbBacklight) + 9 (getSystemState)
                           every 500 ms, and 5 (readFanRPM) + 13 (getBatteryCharge) every 2 s
```

**Agent source layout** (`LaunchAgent/Sources/`, one module, `main.swift` is the only file allowed top-level code): `ECTypes` (mirror of the kext ABI), `ECClient` (IOKit client + its serial queue), `MenuBarController` (status item, menu, preference state), `MuteObserver` (app delegate: key tap and Fn keycodes, CoreAudio sync, EC polling, rotation, menu actions), `MuteOSD`, `PreferencesPanel`, `ECDumpPanel`, `FanCurvePanel`, `FanProfiles`, `Preferences` (typed UserDefaults keys and enums — raw values are the strings stored by earlier versions, never rename them; `MenuBarController` owns the values, read-only elsewhere, and is the only writer). File-scope `private` declarations (keycodes, `WatchBox`, `OSDView`) are only visible in their file — keep them next to their only user. `MSIECToolboxInstaller.swift` is a separate executable, not part of the agent.

**Agent threading**: every IOKit call goes through `MSIECToolboxClient.queue` (serial, `.utility`), usually via `client.run({ work on queue }) { result on main }`. Never call a client method from the main thread: the CGEventTap runs on the main run loop and every keystroke of the system waits for it, while a kext call can block for tens of ms on the EC. The poll/RPM timers fire on that queue and hand a snapshot to `applyPoll` on main, where all agent state (`lastSent*`, `lastCamState`, menu items) lives. Because the queue is serial, a poll queued after a write reads the written state and its result reaches main after the write's completion.

**Adaptive EC polling**: one `DispatchSourceTimer` on the EC queue. `MenuOpenTracker` (the status menu's `NSMenuDelegate`) switches it between closed (1 s, mute/camera only — ~3 EC reads/s) and open (500 ms, full state, RPM every 4th tick). Anything not polled while the menu is closed must be read on demand before it is used: e.g. F8 reads the backlight level from the EC in the same queue operation as the write, because the firmware can change it.

**CGEventTap health**: macOS disables the tap when the main run loop is too slow (e.g. during a display reconfiguration — rotating to 90°/270°) or on Secure Input, and only reports it with the next event, which is then lost. `MuteObserver` re-enables it on `NSApplication.didChangeScreenParametersNotification` and from a 2 s watchdog, which also shows the menu warning (`setAccessibilityWarning`) when `AXIsProcessTrusted()` is false — the case after a re-signature, where the Accessibility entry looks checked but must be removed and re-added.

**Display rotation (F12)**: `CGDisplayRotation` of the built-in panel is the only source of truth — the next angle is computed from it and the menu item is refreshed from it (at launch, after each rotation, on `didChangeScreenParametersNotification`). Never trust the displayplacer exit code: it rotates first, then looks up `res:` in the *new* orientation (portrait = `1080x1920`), so a resolution miss returns 1 although the screen turned; `res:` cannot be omitted either (width/height are uninitialised without it). One rotation at a time (`rotationInProgress`): presses during a rotation, including auto-repeat, are ignored.

**Keyboard map (SSDT ↔ agent)**: the SSDT maps MSI hotkey scancodes to ADB codes and the agent intercepts the resulting keycodes: `e071`→`0x4F` (79, mic), `e072`→`0x6F` (111, rotation), `e06e`→`0x50` (80, F19, camera), `76`→`0x5A` (90, F20, touchpad — the MSI F4 hotkey sends Ctrl+Win+F24, F24 being PS2 `0x76`, unmapped by VoodooPS2), plus the standard F8 (100, backlight). An ADB code shared with a standard key is intercepted for both: the camera used `0x76` (118 = standard F4) until v4 of the SSDT, so Fn+F4 toggled the camera. When adding an entry, update the `Package (n)` count (iasl rejects a mismatch), and keep the agent's `k*KeyCode` constants in sync. To identify a key that macOS never receives, enable VoodooPS2's `LogScanCodes` (set it to 1 on `ApplePS2Keyboard` with `IORegistryEntrySetCFProperties`, as root) and read `sudo dmesg | grep "sending key"` — `xx=80` means the scancode is unmapped; turn it back to 0 afterwards. For keys macOS does receive: `defaults write MSIECToolboxAgent debug_keys -bool true`, restart the agent, read `[debug_keys]` lines, then turn it off — both log every keystroke.

**Touchpad (selector 16)**: not an EC register. The kext relays VoodooPS2's keyboard→touchpad messages (`iokit_vendor_specific_msg(100)` set, `101` get, data `bool*`) to every service with `RM,deliverNotifications = true` and to the touchpad drivers found by class (`VoodooI2CMultitouchHIDEventDriver` and subclasses, VoodooPS2 trackpads). Class matching is required: in VoodooI2CHID only the Precision Touchpad personality sets `RM,deliverNotifications`, the generic Multitouch driver handles the messages without it. The menu item "Trackpad" and the MSI F4 hotkey (keycode 90) toggle it.

**Mute sync direction**: CoreAudio is the source of truth. EC → CoreAudio only propagates *muting*; if the EC LED reads unmuted while the agent last sent muted, the agent rewrites the LED instead of unmuting CoreAudio (any local process can write the LED bits through the kext).

**UserClient access**: `initWithTask` accepts only the console user (`kIOClientPrivilegeLocalUser`, i.e. the agent) or root (CLI under sudo).

**EC bus ownership** — the most important invariant. All EC access in both kexts goes through `MSIECCore` (`MSIECToolbox.cpp`):
- `MSIECCore::BusGuard` (RAII) takes `ecLock`, then the ACPI global lock of the PNP0C09 device (`acquireGlobalLock`, 50 ms timeout → `kIOReturnBusy`). AML takes the same global lock around EC fields declared with the `Lock` rule. If the platform has no global lock, the guard degrades to `ecLock` only (logged once).
- `ecReadLocked` / `ecWriteLocked` are the only raw RD_EC/WR_EC sequences; call them only while holding a `BusGuard`. Multi-register operations (fan curve, Cooler Boost RMW, system state, RPM hi/lo) hold one guard for the whole sequence; `dumpEC` takes one per register so the global lock is never held for 256 reads.
- SMCMSIFan never touches ports `0x62`/`0x66`: it calls `MSIECToolboxDriver::callPlatformFunction("MSIECReadRegisters", offsets, values, count)` and `("MSIECSetBatteryCharge", percent)` (contract in `MSIECToolboxShared.h`), only from its poller workloop — never from a `readAccess()`/`update()`, which run in VirtualSMC's trap handler. No symbol is linked across kexts; it looks the driver up by class name, so load order between the two does not matter.
- `hookedWriteECField` runs in ACPI context and must never take a `BusGuard`.
- Residual risk: AppleACPIEC's own non-AML EC traffic (SCI query handling) is not covered by the global lock.

**Naming**: the static EC class is `MSIECCore`, not `MSIECToolbox` — Lilu's `plugin_start.hpp` declares `class PRODUCT_NAME : IOService`, i.e. `class MSIECToolbox`. SMCMSIFan defines its own IOService and `kern_start`/`kern_stop` (VirtualSMC sensor template), so it does not compile `plugin_start.cpp`.

**Lilu hook (`writeECField`)** — boot-critical, a mistake here panics every boot:
- `KextInfo` must have a non-null `paths` array (`pathNum` ≥ 1). Lilu calls `loadKinfo()` on every registered `KextInfo` when its patcher starts, and `MachInfo::init()` dereferences `paths[0]` in kernel collection mode (macOS 11+). The original `{id, nullptr, 0, …}` panicked at boot as soon as the plugin actually started.
- Target is `com.apple.driver.AppleACPIPlatform` with `sys[KextInfo::Loaded] = true` (the platform expert is always loaded before Lilu plugins).
- The symbol `__ZN20IOACPIPlatformDevice12writeECFieldEjPKvl` and its (offset, value, size) semantics are inferred, not verified: if the symbol is missing the hook is skipped (logged); the value is only rewritten when `size == 1`, the offset is 0x2B/0x2C and the byte matches the firmware base value (0x80 / 0xE0, LED bit aside). DEBUG builds log the first 64 calls — check them with `-msiec.dbg` before trusting the hook.
- The hook runs in ACPI context: atomics only, no `BusGuard`, no mutex, no allocation.
- `-msiec.off` disables the whole Lilu part (hook included) while the EC driver, UserClient and SMCMSIFan keep working.

**Fn/Win swap (EC 0xBF bit 4)**: `MSIECToolboxDriver::start()` clears it with a read-modify-write (`MSIECCore::setFnWinSwap`) and, only if it was set, registers `registerPrioritySleepWakeInterest` to set it back on `kIOMessageSystemWillPowerOff` / `kIOMessageSystemWillRestart`, so Windows keeps the user's Creator Center choice. Sleep is ignored. Not restored after a panic or a forced power-off. The boot-time outcome is published as the driver property `FnWinSwap` (`ioreg -l | grep FnWinSwap`), because early-boot kernel log lines were not found on the user's machine. The handler runs synchronously on the PM thread before the halt, ACPI still up, so a `BusGuard` is allowed there.

**Mute LEDs**: `setMuteState` always writes the LED bit (0x04 of 0x2B/0x2C) with a read-modify-write, whether or not the hook is installed. The hook only re-applies the bit when firmware rewrites those registers; without the direct write the agent's 500 ms poll would read the old bit and revert the CoreAudio mute.

**UserClient selectors** (defined in `MSIECToolboxShared.h`, dispatched in `MSIECToolboxUserClient.cpp`):
0=setMuteState, 1=getMuteState (reserved, superseded by 4), 2=dumpEC, 3=setCameraState, 4=getAllState, 5=readFanRPM, 6=setFanMode, 7=setCoolerBoost, 8=setShiftMode, 9=getSystemState, 10=setKbBacklight, 11=getKbBacklight, 12=setBatteryCharge, 13=getBatteryCharge, 14=setFanCurve, 15=getFanCurve, 16=setTouchpad

## EC Register Reference (CONF_G1_5)

All registers accessed via ACPI port I/O: command port `0x66`, data port `0x62`.

| Register | Values | Purpose |
|----------|--------|---------|
| `0xF4` | `0x0D`=auto / `0x1D`=silent / `0x8D`=advanced | Fan mode |
| `0x98` bit 7 | `0x80`=ON / `0x00`=OFF | Cooler Boost — always read-modify-write |
| `0x68` | direct °C | CPU temp |
| `0x71` | 0–150% | CPU fan speed % |
| `0xCC–0xCD` | big-endian | CPU fan RPM (ISW formula: `RPM = ((325 - val) * 16) + 1480`) |
| `0xCA–0xCB` | big-endian | Second ("GPU") fan RPM, same formula — the A10M has **two fans** despite having no discrete GPU (confirmed by MSI Creator Center and HWiNFO) |
| `0xF3` | `0x80–0x83` | Keyboard backlight (0x80=off, 0x83=high) |
| `0xEF` | bit 7 + % (`0xBC`=60 %, `0xD0`=80 %), bit 7 clear (`0x64`) = no limit | Battery charge limit — msi-ec encoding, `0x80 \| percent` |
| `0xF2` | `0xC0`/`0xC1`/`0xC2` | Shift mode (Turbo/Comfort/Eco) |
| `0xBF` bit 4 | `0x10` set = swapped | Fn/Win key swap (Creator Center option). Survives a reboot and applies on macOS (Win/Command becomes Fn) — see "Fn/Win swap" below |
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
- **Shared header**: `MSIECToolboxShared.h` is C++ (`constexpr`, typed enums) and is included by both kexts (SMCMSIFan via a header search path to `MSIECToolbox/Sources`). It is **not** imported into Swift: the agent and the CLI mirror selectors and structs by hand, so any change to a selector or struct layout must be replicated in `LaunchAgent/Sources/ECTypes.swift` and `CLI/MSIECToolboxDump.swift`.

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
MSIECToolboxDump --watch          # continuous polling, changed registers in red (--interval ≥ 0.5 s)
MSIECToolboxDump --diff           # two snapshots (press Enter between them), changes listed
MSIECToolboxDump --offset 0x2B    # single register
MSIECToolboxDump --json           # structured output
```
