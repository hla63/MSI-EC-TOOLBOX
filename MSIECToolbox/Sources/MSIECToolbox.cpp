// ---------------------------------------------------------------------------
// MSIECToolbox.cpp
//
// Lilu plugin — mute LED control for MSI Modern 15
// Hook: IOACPIPlatformDevice::writeECField (offsets 0x2B / 0x2C, bit 0x04)
// Fallback: raw ACPI EC port I/O when hook is not installed
//
// Tested: Lilu 1.7.1, macOS 14 Sonoma → macOS 26 Tahoe
// ---------------------------------------------------------------------------

#include "MSIECToolbox.h"

// ---------------------------------------------------------------------------
// Lilu plugin registration
// ---------------------------------------------------------------------------

static const char *bootargOff[]   = { "-msiec.off"  };
static const char *bootargDebug[] = { "-msiec.dbg"  };
static const char *bootargBeta[]  = { "-msiec.beta" };

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion("3.1.9"),
    // AllowNormal | AllowInstallerRecovery only.
    // AllowSafeMode intentionally omitted: kernel patches must not apply in
    // safe boot — an unlit LED is acceptable in that context.
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,   arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta,  arrsize(bootargBeta),
    KernelVersion::Sonoma,  // macOS 14 — minimum supported
    KernelVersion::Tahoe,   // macOS 26 — defined natively in Lilu 1.7.1
    []() { MSIECToolbox::pluginStart(); }
};

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

// kextInfo is non-const: onKextLoad() takes a KextInfo* (non-const) — the
// Lilu API may update loadIndex during the callback. Lifetime is guaranteed
// by static storage (kext __DATA segment).
KernelPatcher::KextInfo MSIECToolbox::kextInfo {
    "com.apple.driver.AppleACPIPlatformExpert",
    nullptr, 0, {}, {},
    KernelPatcher::KextInfo::Unloaded
};

mach_vm_address_t MSIECToolbox::orgWriteECField = 0;
IOLock           *MSIECToolbox::ecLock           = nullptr;
IOLock           *MSIECToolbox::stateLock        = nullptr;
bool              MSIECToolbox::speakerMuted      = false;
bool              MSIECToolbox::micMuted          = false;
// hookInstalled is written from patcherCallback() (Lilu thread) and read
// from setMuteState() (IOKit UserClient thread) without a lock.
// On x86_64 bool is de-facto atomic, but that is strict C++ UB.
// _Atomic guarantees visibility across threads without measurable overhead.
_Atomic(bool)     MSIECToolbox::hookInstalled     = false;

// ---------------------------------------------------------------------------
// pluginStart
// ---------------------------------------------------------------------------

// Allocates an IOLock atomically via CAS — prevents double-alloc if
// pluginStart() (Lilu thread) and Driver::start() (IOKit thread) run
// concurrently at boot.
// Pattern: null check → alloc outside lock → CAS(nullptr → candidate).
// If another thread wins the race, free our candidate.
static void allocLockAtomic(IOLock **lockPtr, const char *name) {
    if (!*lockPtr) {
        IOLock *candidate = IOLockAlloc();
        if (!candidate) { MSIEC_ERR("%s IOLockAlloc failed", name); return; }
        if (!OSCompareAndSwapPtr(nullptr, candidate, lockPtr)) {
            // Another thread already installed the lock — discard ours
            IOLockFree(candidate);
        }
    }
}

void MSIECToolbox::pluginStart() {
    // Atomic lock initialisation via CAS.
    // Prevents double-alloc if pluginStart() and Driver::start() run
    // simultaneously at boot.
    allocLockAtomic(&stateLock, "stateLock");
    if (!stateLock) return;
    allocLockAtomic(&ecLock, "ecLock");
    if (!ecLock) return;

    // onKextLoad() instead of onKextLoadForce().
    // onKextLoadForce() would panic the kernel on registration failure,
    // defeating the purpose of the raw EC I/O fallback.
    // onKextLoad() logs the error — the plugin continues and setMuteState()
    // falls back to fallbackECWrite() (hookInstalled remains false).
    auto err = lilu.onKextLoad(&kextInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index,
           mach_vm_address_t address, size_t size) {
            patcherCallback(user, patcher, index, address, size);
        }, nullptr);

    if (err != LiluAPI::Error::NoError) {
        MSIEC_ERR("onKextLoad failed (%d) – fallback raw I/O only", (int)err);
        // hookInstalled stays false: setMuteState() will use fallbackECWrite()
    }
}

// ---------------------------------------------------------------------------
// patcherCallback
// ---------------------------------------------------------------------------

void MSIECToolbox::patcherCallback(void *, KernelPatcher &patcher,
                                  size_t index, mach_vm_address_t, size_t)
{
    if (index != kextInfo.loadIndex) return;

    // Single candidate — correct C++ mangled symbol name.
    // Using only one candidate avoids polluting logs with unresolvable names.
    static const char *const candidates[] = {
        "__ZN20IOACPIPlatformDevice12writeECFieldEjPKvl",
    };

    for (const char *sym : candidates) {
        mach_vm_address_t addr = patcher.solveSymbol(kextInfo.loadIndex, sym);
        if (addr) {
            MSIEC_LOG("Symbol resolved: %s @ 0x%llX", sym, addr);
            KernelPatcher::RouteRequest route{sym,
                reinterpret_cast<mach_vm_address_t>(hookedWriteECField),
                orgWriteECField};
            if (patcher.routeMultiple(kextInfo.loadIndex, &route, 1)) {
                atomic_store_explicit(&hookInstalled, true, memory_order_release);
                MSIEC_LOG("Hook installed successfully");
            } else {
                MSIEC_ERR("Hook installation failed");
                patcher.clearError();
            }
            return;
        }
        patcher.clearError();
    }

    MSIEC_ERR("No EC write symbol resolved – raw I/O fallback active");
}

// ---------------------------------------------------------------------------
// hookedWriteECField
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::hookedWriteECField(IOACPIPlatformDevice *device,
                                         uint32_t offset,
                                         const void *value,
                                         IOByteCount size)
{
    // orgWriteECField: must be non-null — a null call would kernel panic.
    // routeMultiple() fills orgWriteECField before patching the vtable,
    // so it is always valid if the hook was installed successfully.
    // device: not checked — always non-null (it is the ACPI `this` dispatched
    // by IOKit; a null device would indicate a corrupted kernel state).
    // value: checked here for the general case (already checked in mic/spk block).
    if (!orgWriteECField) return kIOReturnInternalError;
    if (!value)           return kIOReturnBadArgument;

    using Fn = IOReturn (*)(IOACPIPlatformDevice *, uint32_t, const void *, IOByteCount);

    if ((offset == kMSI_EC_OFFSET_SPEAKER || offset == kMSI_EC_OFFSET_MIC)
        && size == 1 && value && stateLock)
    {
        bool muted;
        IOLockLock(stateLock);
        muted = (offset == kMSI_EC_OFFSET_SPEAKER) ? speakerMuted : micMuted;
        IOLockUnlock(stateLock);

        uint8_t raw      = *static_cast<const uint8_t *>(value);
        uint8_t modified = (raw & ~kMSI_EC_BIT_LED) | (muted ? kMSI_EC_BIT_LED : 0);

        MSIEC_LOG("EC write 0x%02X: raw=0x%02X patched=0x%02X muted=%d",
                   offset, raw, modified, (int)muted);

        return reinterpret_cast<Fn>(orgWriteECField)(device, offset, &modified, size);
    }

    return reinterpret_cast<Fn>(orgWriteECField)(device, offset, value, size);
}

// ---------------------------------------------------------------------------
// setMuteState — called by MSIECToolboxUserClient
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setMuteState(bool newSpk, bool newMic) {
    if (!stateLock) return kIOReturnNotReady;

    IOLockLock(stateLock);
    speakerMuted = newSpk;
    micMuted     = newMic;
    IOLockUnlock(stateLock);

    MSIEC_LOG("setMuteState: speaker=%d mic=%d hookInstalled=%d",
               (int)newSpk, (int)newMic,
               (int)atomic_load_explicit(&hookInstalled, memory_order_acquire));

    // If the hook is not installed, write directly via raw EC I/O.
    // EC values confirmed by dump on MSI Modern 15:
    //   0x2C (speaker): base=0xE0, muted=0xE4, active=0xE0
    //   0x2B (mic):     base=0x80, muted=0x84, active=0x80
    if (!atomic_load_explicit(&hookInstalled, memory_order_acquire)) {
        uint8_t spkVal = kMSI_EC_BASE_SPEAKER | (newSpk ? kMSI_EC_BIT_LED : 0);
        uint8_t micVal = kMSI_EC_BASE_MIC     | (newMic ? kMSI_EC_BIT_LED : 0);

        IOReturn r1 = fallbackECWrite(kMSI_EC_OFFSET_SPEAKER, spkVal);
        IOReturn r2 = fallbackECWrite(kMSI_EC_OFFSET_MIC,     micVal);

        if (r1 != kIOReturnSuccess) MSIEC_ERR("fallbackECWrite speaker: 0x%08X", r1);
        if (r2 != kIOReturnSuccess) MSIEC_ERR("fallbackECWrite mic: 0x%08X", r2);

        return (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess)
            ? kIOReturnSuccess : kIOReturnIOError;
    }

    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// getMuteState
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::getMuteState(bool &outSpk, bool &outMic) {
    if (!stateLock) return kIOReturnNotReady;
    IOLockLock(stateLock);
    outSpk = speakerMuted;
    outMic = micMuted;
    IOLockUnlock(stateLock);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// dumpEC — reads all 256 EC registers via RD_EC (opcode 0x80)
// Called by MSIECToolboxUserClient (selector kMSIDumpEC).
// Returns kIOReturnTimeout if too many registers fail to respond.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::dumpEC(uint8_t outData[256]) {
    if (!outData) return kIOReturnBadArgument;

    // Abort threshold: 32 failures out of 256 (12.5%) indicates an
    // unresponsive EC. Below that, mark unreadable registers as 0xFF
    // so the caller still gets the readable portion (useful for diagnosis).
    static const int kMaxDumpErrors = 32;
    int failCount = 0;

    for (int i = 0; i < 256; i++) {
        uint8_t val = 0;
        IOReturn r = fallbackECRead(static_cast<uint32_t>(i), val);
        if (r != kIOReturnSuccess) {
            MSIEC_ERR("dumpEC: read offset 0x%02X failed (0x%08X)", i, r);
            outData[i] = 0xFF;
            if (++failCount >= kMaxDumpErrors) {
                MSIEC_ERR("dumpEC: too many errors (%d), EC unresponsive — aborting", failCount);
                return kIOReturnTimeout;
            }
        } else {
            outData[i] = val;
        }
    }

    MSIEC_LOG("dumpEC: 256 registers read (%d errors)", failCount);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// EC I/O helpers
// ---------------------------------------------------------------------------

// Waits for IBF (Input Buffer Full) to clear: EC ready to receive a command.
bool MSIECToolbox::ecWaitIBF() {
    for (int i = 0; i < 1000; i++) {  // 10ms max (1000 × 10µs)
        uint8_t status;
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"(kECCommandPort));
        if (!(status & 0x02)) return true;
        IODelay(10);
    }
    MSIEC_ERR("ecWaitIBF: timeout (10ms)");
    return false;
}

// Waits for OBF (Output Buffer Full) to set: data available for reading.
bool MSIECToolbox::ecWaitOBF() {
    for (int i = 0; i < 1000; i++) {  // 10ms max (1000 × 10µs)
        uint8_t status;
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"(kECCommandPort));
        if (status & 0x01) return true;
        IODelay(10);
    }
    MSIEC_ERR("ecWaitOBF: timeout (10ms)");
    return false;
}

// ---------------------------------------------------------------------------
// fallbackECRead — raw ACPI EC port I/O
// Command port 0x66, data port 0x62, opcode RD_EC = 0x80
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::fallbackECRead(uint32_t offset, uint8_t &outValue) {
    // The EC bus is non-reentrant — only one thread at a time.
    // Without a mutex, two concurrent sequences (cmd+offset+data) can
    // interleave and corrupt the EC state or lock it indefinitely.
    if (!ecLock) return kIOReturnNotReady;
    IOLockLock(ecLock);

    IOReturn ret = kIOReturnSuccess;
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)offset), "Nd"(kECDataPort));
    if (!ecWaitOBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("inb %1, %0" : "=a"(outValue) : "Nd"(kECDataPort));

done:
    IOLockUnlock(ecLock);
    return ret;
}

// ---------------------------------------------------------------------------
// fallbackECWrite — raw ACPI EC port I/O
// Command port 0x66, data port 0x62, opcode WR_EC = 0x81
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::fallbackECWrite(uint32_t offset, uint8_t value) {
    if (!ecLock) return kIOReturnNotReady;
    IOLockLock(ecLock);

    IOReturn ret = kIOReturnSuccess;
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpWrite), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)offset), "Nd"(kECDataPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(kECDataPort));

done:
    MSIEC_LOG("fallbackECWrite: offset=0x%02X val=0x%02X ret=0x%08X", offset, value, ret);
    IOLockUnlock(ecLock);
    return ret;
}

// ---------------------------------------------------------------------------
// setFanMode — sets the fan control profile via EC 0xF4
//
// auto     (0x0D): EC drives fans according to its internal curve
// silent   (0x1D): fans held at minimum until critical threshold
// advanced (0x8D): custom curve active (6 breakpoints, see setFanCurve)
//
// Note: advanced mode only takes effect if the curve (0x6A–0x77) has been
// programmed first. Without a validated EC dump, this mode falls back to
// the firmware default curve.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setFanMode(MSIFanModeValue mode) {
    uint8_t ecVal;
    switch (mode) {
        case kMSIFanModeAuto:     ecVal = kMSI_EC_FAN_AUTO;     break;
        case kMSIFanModeSilent:   ecVal = kMSI_EC_FAN_SILENT;   break;
        case kMSIFanModeAdvanced: ecVal = kMSI_EC_FAN_ADVANCED; break;
        default: return kIOReturnBadArgument;
    }
    MSIEC_LOG("setFanMode: mode=%d -> EC[0xF4]=0x%02X", (int)mode, ecVal);
    return fallbackECWrite(kMSI_EC_FAN_MODE_ADDR, ecVal);
}

// ---------------------------------------------------------------------------
// setBatteryCharge — battery charge stop threshold via EC 0xEF
//
// percent: 80 or 100 only (values confirmed on A10M 1551EMS1)
//   0x50 (80)  = stop at 80% — recommended for long-term battery health
//   0x64 (100) = full charge (firmware default behaviour)
//
// Note: 60% (0xBC) is not exposed here — its encoding is non-linear and
// requires further validation before being added to the UI.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setBatteryCharge(uint8_t percent) {
    uint8_t ecVal;
    switch (percent) {
        case 80:  ecVal = 0x50; break;
        case 100: ecVal = 0x64; break;
        default:  return kIOReturnBadArgument;
    }
    MSIEC_LOG("setBatteryCharge: %d%% -> EC[0xEF]=0x%02X", (int)percent, ecVal);
    return fallbackECWrite(kMSI_EC_BATTERY_CHARGE_ADDR, ecVal);
}

IOReturn MSIECToolbox::getBatteryCharge(uint8_t &outPercent) {
    uint8_t raw = 0;
    IOReturn r = fallbackECRead(kMSI_EC_BATTERY_CHARGE_ADDR, raw);
    if (r != kIOReturnSuccess) return r;
    switch (raw) {
        case 0x50: outPercent = 80;  break;
        case 0x64: outPercent = 100; break;
        default:   outPercent = 100; break;  // unknown value → safe default
    }
    MSIEC_LOG("getBatteryCharge: EC[0xEF]=0x%02X -> %d%%", raw, (int)outPercent);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// setFanCurve — programs the 6 CPU fan curve breakpoints
//
// Writes EC registers 0x6A-0x6F (temperatures) and 0x72-0x77 (speeds).
// The fixed 100% point at 0x78 is preserved (not modified here).
//
// Firmware constraints validated by dump on A10M 1551EMS1:
//   - Temperatures: strictly increasing order, range [20, 95] °C
//   - Speeds:       non-decreasing order, range [0, 100] %
//   - Speed[0] = 0 is valid (fan off below first temperature threshold)
//
// Only effective when fan mode is kMSIFanModeAdvanced.
// In auto or silent mode the EC ignores the custom curve.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setFanCurve(const MSIFanCurve &curve) {
    for (int i = 0; i < 6; i++) {
        if (curve.temps[i]  < 20 || curve.temps[i]  > 95) return kIOReturnBadArgument;
        if (curve.speeds[i] > 100)                         return kIOReturnBadArgument;
        if (i > 0 && curve.temps[i]  <= curve.temps[i-1]) return kIOReturnBadArgument;
        if (i > 0 && curve.speeds[i] <  curve.speeds[i-1])return kIOReturnBadArgument;
    }

    // Hold ecLock across all 12 writes to prevent a partial update.
    // If any write fails mid-way (e.g. EC timeout), the curve registers
    // would be in an inconsistent state (some temps written, no speeds).
    // Inline the port I/O sequence directly, as fallbackECWrite() also
    // acquires ecLock and we must not nest locks.
    if (!ecLock) return kIOReturnNotReady;
    IOLockLock(ecLock);

    IOReturn ret = kIOReturnSuccess;
    for (int i = 0; i < 6 && ret == kIOReturnSuccess; i++) {
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"(kECOpWrite), "Nd"(kECCommandPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"((uint8_t)(kMSI_EC_FAN_CPU_TEMP_BASE + i)), "Nd"(kECDataPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"(curve.temps[i]), "Nd"(kECDataPort));
        if (ret != kIOReturnSuccess)
            MSIEC_ERR("setFanCurve: temp[%d] write failed: 0x%08X", i, ret);
    }
    for (int i = 0; i < 6 && ret == kIOReturnSuccess; i++) {
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"(kECOpWrite), "Nd"(kECCommandPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"((uint8_t)(kMSI_EC_FAN_CPU_SPD_BASE + i)), "Nd"(kECDataPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; break; }
        asm volatile("outb %0, %1" :: "a"(curve.speeds[i]), "Nd"(kECDataPort));
        if (ret != kIOReturnSuccess)
            MSIEC_ERR("setFanCurve: speed[%d] write failed: 0x%08X", i, ret);
    }

    IOLockUnlock(ecLock);
    MSIEC_LOG("setFanCurve: 12 registers written (T=%d/%d/%d/%d/%d/%d S=%d/%d/%d/%d/%d/%d)",
        curve.temps[0], curve.temps[1], curve.temps[2],
        curve.temps[3], curve.temps[4], curve.temps[5],
        curve.speeds[0], curve.speeds[1], curve.speeds[2],
        curve.speeds[3], curve.speeds[4], curve.speeds[5]);
    return kIOReturnSuccess;
}

IOReturn MSIECToolbox::getFanCurve(MSIFanCurve &out) {
    for (int i = 0; i < 6; i++) {
        IOReturn r = fallbackECRead(kMSI_EC_FAN_CPU_TEMP_BASE + i, out.temps[i]);
        if (r != kIOReturnSuccess) return r;
    }
    for (int i = 0; i < 6; i++) {
        IOReturn r = fallbackECRead(kMSI_EC_FAN_CPU_SPD_BASE + i, out.speeds[i]);
        if (r != kIOReturnSuccess) return r;
    }
    MSIEC_LOG("getFanCurve: T=%d/%d/%d/%d/%d/%d S=%d/%d/%d/%d/%d/%d",
        out.temps[0], out.temps[1], out.temps[2],
        out.temps[3], out.temps[4], out.temps[5],
        out.speeds[0], out.speeds[1], out.speeds[2],
        out.speeds[3], out.speeds[4], out.speeds[5]);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// setKbBacklight — keyboard backlight level via EC 0xF3
//
// level: 0=off 1=low 2=medium 3=high
// Simple write — no RMW needed, the register is fully owned by this field.
// The value survives sleep but is reset to off by the firmware at boot.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setKbBacklight(uint8_t level) {
    if (level > 3) return kIOReturnBadArgument;
    uint8_t ecVal = kMSI_EC_KB_BACKLIGHT_OFF + level;  // 0x80 + level
    MSIEC_LOG("setKbBacklight: level=%d -> EC[0xF3]=0x%02X", (int)level, ecVal);
    return fallbackECWrite(kMSI_EC_KB_BACKLIGHT_ADDR, ecVal);
}

IOReturn MSIECToolbox::getKbBacklight(uint8_t &outLevel) {
    uint8_t raw = 0;
    IOReturn r = fallbackECRead(kMSI_EC_KB_BACKLIGHT_ADDR, raw);
    if (r != kIOReturnSuccess) return r;
    outLevel = (raw >= kMSI_EC_KB_BACKLIGHT_OFF && raw <= kMSI_EC_KB_BACKLIGHT_MAX)
               ? (raw - kMSI_EC_KB_BACKLIGHT_OFF)
               : 0;
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// setCoolerBoost — forces all fans to 100% via EC 0x98 bit 7
//
// Read-modify-write to preserve bits 0–6 (unknown firmware use).
// The entire sequence is held under ecLock to prevent a concurrent
// getSystemState() read from interleaving between the read and write.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setCoolerBoost(bool enable) {
    if (!ecLock) return kIOReturnNotReady;
    IOLockLock(ecLock);

    IOReturn ret = kIOReturnSuccess;
    uint8_t current = 0;

    // Direct read (ecLock already held — do not call fallbackECRead here)
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)kMSI_EC_COOLER_BOOST_ADDR), "Nd"(kECDataPort));
    if (!ecWaitOBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("inb %1, %0" : "=a"(current) : "Nd"(kECDataPort));

    {
        uint8_t newVal = enable
            ? (current |  kMSI_EC_COOLER_BOOST_MASK)
            : (current & ~kMSI_EC_COOLER_BOOST_MASK);

        MSIEC_LOG("setCoolerBoost: %s EC[0x98]: 0x%02X -> 0x%02X",
                   enable ? "ON" : "OFF", current, newVal);

        // Direct write (ecLock still held)
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
        asm volatile("outb %0, %1" :: "a"(kECOpWrite), "Nd"(kECCommandPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
        asm volatile("outb %0, %1" :: "a"((uint8_t)kMSI_EC_COOLER_BOOST_ADDR), "Nd"(kECDataPort));
        if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
        asm volatile("outb %0, %1" :: "a"(newVal), "Nd"(kECDataPort));
    }

done:
    IOLockUnlock(ecLock);
    return ret;
}

// ---------------------------------------------------------------------------
// setShiftMode — CPU+GPU performance profile via EC 0xF2
//
// eco     (0xC2): minimum frequencies, lowest power draw
// comfort (0xC1): balanced frequencies (firmware default)
// turbo   (0xC0): maximum frequencies
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::setShiftMode(MSIShiftModeValue mode) {
    uint8_t ecVal;
    switch (mode) {
        case kMSIShiftEco:     ecVal = kMSI_EC_SHIFT_ECO;     break;
        case kMSIShiftComfort: ecVal = kMSI_EC_SHIFT_COMFORT; break;
        case kMSIShiftTurbo:   ecVal = kMSI_EC_SHIFT_TURBO;   break;
        default: return kIOReturnBadArgument;
    }
    MSIEC_LOG("setShiftMode: mode=%d -> EC[0xF2]=0x%02X", (int)mode, ecVal);
    return fallbackECWrite(kMSI_EC_SHIFT_MODE_ADDR, ecVal);
}

// ---------------------------------------------------------------------------
// getSystemState — aggregated single-pass read (temp + fan% + modes)
//
// Returns the 7 most useful EC registers in one series of reads.
// Used by the LaunchAgent polling loop (selector 9).
//
// Individual read failures are tolerated (value stays 0) so that a single
// timeout does not block the entire state snapshot.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::getSystemState(MSISystemState &out) {
    uint8_t cpuTemp = 0, gpuTemp = 0;
    uint8_t cpuPct  = 0, gpuPct  = 0;
    uint8_t fanModeRaw = 0, shiftRaw = 0, boostRaw = 0;

    fallbackECRead(kMSI_EC_CPU_TEMP_ADDR,    cpuTemp);
    fallbackECRead(kMSI_EC_GPU_TEMP_ADDR,    gpuTemp);
    fallbackECRead(kMSI_EC_CPU_FAN_PCT_ADDR, cpuPct);
    fallbackECRead(kMSI_EC_GPU_FAN_PCT_ADDR, gpuPct);
    fallbackECRead(kMSI_EC_FAN_MODE_ADDR,    fanModeRaw);
    fallbackECRead(kMSI_EC_SHIFT_MODE_ADDR,  shiftRaw);
    fallbackECRead(kMSI_EC_COOLER_BOOST_ADDR,boostRaw);

    out.cpuTempC    = cpuTemp;
    out.gpuTempC    = gpuTemp;
    out.cpuFanPct   = cpuPct;
    out.gpuFanPct   = gpuPct;
    out.coolerBoost = (boostRaw & kMSI_EC_COOLER_BOOST_MASK) ? 1 : 0;

    switch (fanModeRaw) {
        case kMSI_EC_FAN_SILENT:   out.fanMode = kMSIFanModeSilent;   break;
        case kMSI_EC_FAN_ADVANCED: out.fanMode = kMSIFanModeAdvanced; break;
        default:                   out.fanMode = kMSIFanModeAuto;     break;
    }
    switch (shiftRaw) {
        case kMSI_EC_SHIFT_ECO:   out.shiftMode = kMSIShiftEco;    break;
        case kMSI_EC_SHIFT_TURBO: out.shiftMode = kMSIShiftTurbo;  break;
        default:                  out.shiftMode = kMSIShiftComfort; break;
    }
    out.reserved = 0;

    MSIEC_LOG("getSystemState: CPU=%d°C fan=%d%% GPU=%d°C fanMode=%d shift=%d boost=%d",
               cpuTemp, cpuPct, gpuTemp,
               (int)out.fanMode, (int)out.shiftMode, (int)out.coolerBoost);
    return kIOReturnSuccess;
}

IOReturn MSIECToolbox::setCameraState(bool cameraOff) {
    uint8_t val = cameraOff ? kMSI_EC_CAM_OFF : kMSI_EC_CAM_ACTIVE;
    MSIEC_LOG("setCameraState: cameraOff=%d -> EC[0x2E]=0x%02X", (int)cameraOff, val);
    return fallbackECWrite(kMSI_EC_OFFSET_CAMERA, val);
}

// ---------------------------------------------------------------------------
// readFanRPM — reads the 4 fan RPM registers (0xCA–0xCD)
// Each register is checked individually: a timeout on any one of them
// returns an error rather than computing RPM from stale zero values.
// ---------------------------------------------------------------------------

IOReturn MSIECToolbox::readFanRPM(uint16_t &outCpuRPM, uint16_t &outGpuRPM) {
    uint8_t gpuHi = 0, gpuLo = 0, cpuHi = 0, cpuLo = 0;
    if (fallbackECRead(kMSI_EC_FAN_GPU_HI, gpuHi) != kIOReturnSuccess) return kIOReturnTimeout;
    if (fallbackECRead(kMSI_EC_FAN_GPU_LO, gpuLo) != kIOReturnSuccess) return kIOReturnTimeout;
    if (fallbackECRead(kMSI_EC_FAN_CPU_HI, cpuHi) != kIOReturnSuccess) return kIOReturnTimeout;
    if (fallbackECRead(kMSI_EC_FAN_CPU_LO, cpuLo) != kIOReturnSuccess) return kIOReturnTimeout;
    outGpuRPM = msiECToRPM(gpuHi, gpuLo);
    outCpuRPM = msiECToRPM(cpuHi, cpuLo);
    return kIOReturnSuccess;
}
