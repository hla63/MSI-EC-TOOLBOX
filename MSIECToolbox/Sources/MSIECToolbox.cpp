// ---------------------------------------------------------------------------
// MSIECToolbox.cpp
//
// Lilu plugin — EC control for MSI Modern 15
// Hook: IOACPIPlatformDevice::writeECField (offsets 0x2B / 0x2C, bit 0x04)
// EC access: raw ACPI EC port I/O, serialised by MSIECCore::BusGuard
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
    parseModuleVersion(xStringify(MODULE_VERSION)),
    // AllowNormal | AllowInstallerRecovery only.
    // AllowSafeMode intentionally omitted: kernel patches must not apply in
    // safe boot — an unlit LED is acceptable in that context.
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,   arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta,  arrsize(bootargBeta),
    KernelVersion::Sonoma,  // macOS 14 — minimum supported
    KernelVersion::Tahoe,   // macOS 26 — defined natively in Lilu 1.7.1
    []() { MSIECCore::pluginStart(); }
};

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

// kextInfo is non-const: onKextLoad() takes a KextInfo* (non-const) — the
// Lilu API may update loadIndex during the callback. Lifetime is guaranteed
// by static storage (kext __DATA segment).
KernelPatcher::KextInfo MSIECCore::kextInfo {
    "com.apple.driver.AppleACPIPlatformExpert",
    nullptr, 0, {}, {},
    KernelPatcher::KextInfo::Unloaded
};

mach_vm_address_t               MSIECCore::orgWriteECField  = 0;
IOLock                         *MSIECCore::ecLock           = nullptr;
IOLock                         *MSIECCore::stateLock        = nullptr;
bool                            MSIECCore::speakerMuted     = false;
bool                            MSIECCore::micMuted         = false;
_Atomic(bool)                   MSIECCore::hookInstalled    = false;
_Atomic(IOACPIPlatformDevice *) MSIECCore::ecDevice         = nullptr;
_Atomic(bool)                   MSIECCore::globalLockUsable = true;

// Upper bound on how long the ACPI global lock may be awaited. AML holds it
// only for the duration of a field access, so a longer wait means firmware
// is stuck — give up rather than stall the caller.
static const mach_timespec_t kGlobalLockTimeout = { 0, 50 * 1000 * 1000 };  // 50 ms

// ---------------------------------------------------------------------------
// Lock allocation
// ---------------------------------------------------------------------------

// Allocates an IOLock atomically via CAS — prevents double-alloc if
// pluginStart() (Lilu thread) and Driver::start() (IOKit thread) run
// concurrently at boot.
// Pattern: null check → alloc outside lock → CAS(nullptr → candidate).
// If another thread wins the race, free our candidate.
static bool allocLockAtomic(IOLock **lockPtr, const char *name) {
    if (!*lockPtr) {
        IOLock *candidate = IOLockAlloc();
        if (!candidate) { MSIEC_ERR("%s IOLockAlloc failed", name); return false; }
        if (!OSCompareAndSwapPtr(nullptr, candidate, lockPtr)) {
            // Another thread already installed the lock — discard ours
            IOLockFree(candidate);
        }
    }
    return true;
}

bool MSIECCore::allocLocks() {
    return allocLockAtomic(&stateLock, "stateLock") && allocLockAtomic(&ecLock, "ecLock");
}

void MSIECCore::setECDevice(IOACPIPlatformDevice *device) {
    if (!device) return;
    device->retain();  // never released: BusGuard may use it until shutdown
    IOACPIPlatformDevice *expected = nullptr;
    if (!atomic_compare_exchange_strong(&ecDevice, &expected, device))
        device->release();
}

// ---------------------------------------------------------------------------
// pluginStart
// ---------------------------------------------------------------------------

void MSIECCore::pluginStart() {
    if (!allocLocks()) return;

    // onKextLoad() instead of onKextLoadForce().
    // onKextLoadForce() would panic the kernel on registration failure.
    // onKextLoad() logs the error — the plugin continues and LED state is
    // still driven by setMuteState()'s direct EC writes.
    auto err = lilu.onKextLoad(&kextInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index,
           mach_vm_address_t address, size_t size) {
            patcherCallback(user, patcher, index, address, size);
        }, nullptr);

    if (err != LiluAPI::Error::NoError)
        MSIEC_ERR("onKextLoad failed (%d) – writeECField hook unavailable", (int)err);
}

// ---------------------------------------------------------------------------
// patcherCallback
// ---------------------------------------------------------------------------

void MSIECCore::patcherCallback(void *, KernelPatcher &patcher,
                                size_t index, mach_vm_address_t, size_t)
{
    if (index != kextInfo.loadIndex) return;

    static const char *const sym = "__ZN20IOACPIPlatformDevice12writeECFieldEjPKvl";

    mach_vm_address_t addr = patcher.solveSymbol(kextInfo.loadIndex, sym);
    if (!addr) {
        patcher.clearError();
        MSIEC_ERR("writeECField not resolved – hook unavailable");
        return;
    }

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
}

// ---------------------------------------------------------------------------
// hookedWriteECField
//
// Firmware (AML) rewrites 0x2B / 0x2C on its own, e.g. on Fn keys or resume,
// and would clear the mute LED bit. The hook forces the bit to the state last
// set by setMuteState(). Runs in ACPI context: it must not take BusGuard
// (AML may already hold the ACPI global lock here).
// ---------------------------------------------------------------------------

IOReturn MSIECCore::hookedWriteECField(IOACPIPlatformDevice *device,
                                       uint32_t offset,
                                       const void *value,
                                       IOByteCount size)
{
    // orgWriteECField is filled by routeMultiple() before the route goes live.
    if (!orgWriteECField) return kIOReturnInternalError;
    if (!value)           return kIOReturnBadArgument;

    using Fn = IOReturn (*)(IOACPIPlatformDevice *, uint32_t, const void *, IOByteCount);

    if ((offset == kMSI_EC_OFFSET_SPEAKER || offset == kMSI_EC_OFFSET_MIC)
        && size == 1 && stateLock)
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
// BusGuard — exclusive EC bus ownership
// ---------------------------------------------------------------------------

MSIECCore::BusGuard::BusGuard() {
    heldLock = ecLock;
    if (!heldLock) return;  // st stays kIOReturnNotReady
    IOLockLock(heldLock);

    device = atomic_load_explicit(&ecDevice, memory_order_acquire);
    if (device && atomic_load_explicit(&globalLockUsable, memory_order_relaxed)) {
        IOReturn r = device->acquireGlobalLock(&globalLockToken, &kGlobalLockTimeout);
        if (r == kIOReturnSuccess) {
            haveGlobalLock = true;
        } else if (r == kIOReturnTimeout) {
            // AML is holding the lock: do not touch the ports concurrently.
            MSIEC_ERR("BusGuard: ACPI global lock timeout");
            st = kIOReturnBusy;
            return;
        } else {
            // No global lock on this platform: ecLock alone still serialises
            // this kext and SMCMSIFan.
            atomic_store_explicit(&globalLockUsable, false, memory_order_relaxed);
            MSIEC_ERR("BusGuard: acquireGlobalLock unsupported (0x%08X), using ecLock only", r);
        }
    }
    st = kIOReturnSuccess;
}

MSIECCore::BusGuard::~BusGuard() {
    if (!heldLock) return;
    if (haveGlobalLock)
        device->releaseGlobalLock(globalLockToken);
    IOLockUnlock(heldLock);
}

// ---------------------------------------------------------------------------
// Raw EC port I/O — ACPI spec: command port 0x66, data port 0x62
// ---------------------------------------------------------------------------

// Waits for IBF (Input Buffer Full) to clear: EC ready to receive a command.
bool MSIECCore::ecWaitIBF() {
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
bool MSIECCore::ecWaitOBF() {
    for (int i = 0; i < 1000; i++) {  // 10ms max (1000 × 10µs)
        uint8_t status;
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"(kECCommandPort));
        if (status & 0x01) return true;
        IODelay(10);
    }
    MSIEC_ERR("ecWaitOBF: timeout (10ms)");
    return false;
}

IOReturn MSIECCore::ecReadLocked(uint8_t offset, uint8_t &outValue) {
    if (!ecWaitIBF()) return kIOReturnTimeout;
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) return kIOReturnTimeout;
    asm volatile("outb %0, %1" :: "a"(offset), "Nd"(kECDataPort));
    if (!ecWaitOBF()) return kIOReturnTimeout;
    asm volatile("inb %1, %0" : "=a"(outValue) : "Nd"(kECDataPort));
    return kIOReturnSuccess;
}

IOReturn MSIECCore::ecWriteLocked(uint8_t offset, uint8_t value) {
    if (!ecWaitIBF()) return kIOReturnTimeout;
    asm volatile("outb %0, %1" :: "a"(kECOpWrite), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) return kIOReturnTimeout;
    asm volatile("outb %0, %1" :: "a"(offset), "Nd"(kECDataPort));
    if (!ecWaitIBF()) return kIOReturnTimeout;
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(kECDataPort));
    return kIOReturnSuccess;
}

IOReturn MSIECCore::ecRead(uint32_t offset, uint8_t &outValue) {
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();
    return ecReadLocked(static_cast<uint8_t>(offset), outValue);
}

IOReturn MSIECCore::ecWrite(uint32_t offset, uint8_t value) {
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();
    IOReturn ret = ecWriteLocked(static_cast<uint8_t>(offset), value);
    MSIEC_LOG("ecWrite: offset=0x%02X val=0x%02X ret=0x%08X", offset, value, ret);
    return ret;
}

IOReturn MSIECCore::readRegisters(const uint8_t *offsets, uint8_t *outValues, uint32_t count) {
    if (!offsets || !outValues || count == 0 || count > kMSIECMaxBatchRead)
        return kIOReturnBadArgument;

    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();
    for (uint32_t i = 0; i < count; i++) {
        IOReturn r = ecReadLocked(offsets[i], outValues[i]);
        if (r != kIOReturnSuccess) return r;
    }
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Mute LEDs
// ---------------------------------------------------------------------------

// Read-modify-write of the LED bit only: the other bits of 0x2B / 0x2C
// belong to firmware.
IOReturn MSIECCore::setLEDBit(BusGuard &, uint8_t offset, bool on) {
    uint8_t cur = 0;
    IOReturn r = ecReadLocked(offset, cur);
    if (r != kIOReturnSuccess) return r;
    uint8_t want = on ? (cur | kMSI_EC_BIT_LED) : (cur & ~kMSI_EC_BIT_LED);
    if (want == cur) return kIOReturnSuccess;
    return ecWriteLocked(offset, want);
}

IOReturn MSIECCore::setMuteState(bool newSpk, bool newMic) {
    if (!stateLock) return kIOReturnNotReady;

    IOLockLock(stateLock);
    speakerMuted = newSpk;
    micMuted     = newMic;
    IOLockUnlock(stateLock);

    // Always write the LED bits now, hook or not. The hook only rewrites
    // them when firmware happens to write 0x2B / 0x2C, which is rare: without
    // a direct write the agent's 500ms poll would read the old bit back and
    // revert the CoreAudio mute it just applied.
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();

    IOReturn r1 = setLEDBit(bus, kMSI_EC_OFFSET_SPEAKER, newSpk);
    IOReturn r2 = setLEDBit(bus, kMSI_EC_OFFSET_MIC,     newMic);

    if (r1 != kIOReturnSuccess) MSIEC_ERR("setMuteState speaker LED: 0x%08X", r1);
    if (r2 != kIOReturnSuccess) MSIEC_ERR("setMuteState mic LED: 0x%08X", r2);

    MSIEC_LOG("setMuteState: speaker=%d mic=%d hookInstalled=%d",
               (int)newSpk, (int)newMic,
               (int)atomic_load_explicit(&hookInstalled, memory_order_acquire));

    return (r1 == kIOReturnSuccess && r2 == kIOReturnSuccess)
        ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn MSIECCore::getMuteState(bool &outSpk, bool &outMic) {
    if (!stateLock) return kIOReturnNotReady;
    IOLockLock(stateLock);
    outSpk = speakerMuted;
    outMic = micMuted;
    IOLockUnlock(stateLock);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// dumpEC — reads all 256 EC registers
// One BusGuard per register: holding the ACPI global lock for the whole dump
// would block firmware EC accesses (battery, thermal) for ~100ms or more.
// Returns kIOReturnTimeout if too many registers fail to respond.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::dumpEC(uint8_t outData[256]) {
    if (!outData) return kIOReturnBadArgument;

    // Abort threshold: 32 failures out of 256 (12.5%) indicates an
    // unresponsive EC. Below that, mark unreadable registers as 0xFF
    // so the caller still gets the readable portion (useful for diagnosis).
    static const int kMaxDumpErrors = 32;
    int failCount = 0;

    for (int i = 0; i < 256; i++) {
        uint8_t val = 0;
        IOReturn r = ecRead(static_cast<uint32_t>(i), val);
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

IOReturn MSIECCore::setFanMode(MSIFanModeValue mode) {
    uint8_t ecVal;
    switch (mode) {
        case kMSIFanModeAuto:     ecVal = kMSI_EC_FAN_AUTO;     break;
        case kMSIFanModeSilent:   ecVal = kMSI_EC_FAN_SILENT;   break;
        case kMSIFanModeAdvanced: ecVal = kMSI_EC_FAN_ADVANCED; break;
        default: return kIOReturnBadArgument;
    }
    MSIEC_LOG("setFanMode: mode=%d -> EC[0xF4]=0x%02X", (int)mode, ecVal);
    return ecWrite(kMSI_EC_FAN_MODE_ADDR, ecVal);
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

IOReturn MSIECCore::setBatteryCharge(uint8_t percent) {
    uint8_t ecVal;
    switch (percent) {
        case 80:  ecVal = kMSI_EC_BATTERY_CHARGE_80;  break;
        case 100: ecVal = kMSI_EC_BATTERY_CHARGE_100; break;
        default:  return kIOReturnBadArgument;
    }
    MSIEC_LOG("setBatteryCharge: %d%% -> EC[0xEF]=0x%02X", (int)percent, ecVal);
    return ecWrite(kMSI_EC_BATTERY_CHARGE_ADDR, ecVal);
}

IOReturn MSIECCore::getBatteryCharge(uint8_t &outPercent) {
    uint8_t raw = 0;
    IOReturn r = ecRead(kMSI_EC_BATTERY_CHARGE_ADDR, raw);
    if (r != kIOReturnSuccess) return r;
    switch (raw) {
        case kMSI_EC_BATTERY_CHARGE_80:  outPercent = 80;  break;
        case kMSI_EC_BATTERY_CHARGE_100: outPercent = 100; break;
        default:                         outPercent = 100; break;  // unknown value → safe default
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

IOReturn MSIECCore::setFanCurve(const MSIFanCurve &curve) {
    for (int i = 0; i < kMSI_EC_FAN_CURVE_POINTS; i++) {
        if (curve.temps[i]  < 20 || curve.temps[i]  > 95) return kIOReturnBadArgument;
        if (curve.speeds[i] > 100)                         return kIOReturnBadArgument;
        if (i > 0 && curve.temps[i]  <= curve.temps[i-1]) return kIOReturnBadArgument;
        if (i > 0 && curve.speeds[i] <  curve.speeds[i-1])return kIOReturnBadArgument;
    }

    // One BusGuard across all 12 writes so no other EC access interleaves
    // with the curve update.
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();

    IOReturn ret = kIOReturnSuccess;
    for (int i = 0; i < kMSI_EC_FAN_CURVE_POINTS && ret == kIOReturnSuccess; i++) {
        ret = ecWriteLocked(kMSI_EC_FAN_CPU_TEMP_BASE + i, curve.temps[i]);
        if (ret != kIOReturnSuccess) MSIEC_ERR("setFanCurve: temp[%d] write timeout", i);
    }
    for (int i = 0; i < kMSI_EC_FAN_CURVE_POINTS && ret == kIOReturnSuccess; i++) {
        ret = ecWriteLocked(kMSI_EC_FAN_CPU_SPD_BASE + i, curve.speeds[i]);
        if (ret != kIOReturnSuccess) MSIEC_ERR("setFanCurve: speed[%d] write timeout", i);
    }

    if (ret != kIOReturnSuccess) {
        MSIEC_ERR("setFanCurve: aborted, EC curve registers may be inconsistent (0x%08X)", ret);
        return ret;
    }
    MSIEC_LOG("setFanCurve: 12 registers written (T=%d/%d/%d/%d/%d/%d S=%d/%d/%d/%d/%d/%d)",
        curve.temps[0], curve.temps[1], curve.temps[2],
        curve.temps[3], curve.temps[4], curve.temps[5],
        curve.speeds[0], curve.speeds[1], curve.speeds[2],
        curve.speeds[3], curve.speeds[4], curve.speeds[5]);
    return kIOReturnSuccess;
}

IOReturn MSIECCore::getFanCurve(MSIFanCurve &out) {
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();

    for (int i = 0; i < kMSI_EC_FAN_CURVE_POINTS; i++) {
        IOReturn r = ecReadLocked(kMSI_EC_FAN_CPU_TEMP_BASE + i, out.temps[i]);
        if (r != kIOReturnSuccess) return r;
    }
    for (int i = 0; i < kMSI_EC_FAN_CURVE_POINTS; i++) {
        IOReturn r = ecReadLocked(kMSI_EC_FAN_CPU_SPD_BASE + i, out.speeds[i]);
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

IOReturn MSIECCore::setKbBacklight(uint8_t level) {
    if (level > 3) return kIOReturnBadArgument;
    uint8_t ecVal = kMSI_EC_KB_BACKLIGHT_OFF + level;  // 0x80 + level
    MSIEC_LOG("setKbBacklight: level=%d -> EC[0xF3]=0x%02X", (int)level, ecVal);
    return ecWrite(kMSI_EC_KB_BACKLIGHT_ADDR, ecVal);
}

IOReturn MSIECCore::getKbBacklight(uint8_t &outLevel) {
    uint8_t raw = 0;
    IOReturn r = ecRead(kMSI_EC_KB_BACKLIGHT_ADDR, raw);
    if (r != kIOReturnSuccess) return r;
    outLevel = (raw >= kMSI_EC_KB_BACKLIGHT_OFF && raw <= kMSI_EC_KB_BACKLIGHT_MAX)
               ? (raw - kMSI_EC_KB_BACKLIGHT_OFF)
               : 0;
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// setCoolerBoost — forces all fans to 100% via EC 0x98 bit 7
//
// Read-modify-write to preserve bits 0–6 (unknown firmware use), under a
// single BusGuard so nothing can write 0x98 between the read and the write.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::setCoolerBoost(bool enable) {
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();

    uint8_t current = 0;
    IOReturn ret = ecReadLocked(kMSI_EC_COOLER_BOOST_ADDR, current);
    if (ret != kIOReturnSuccess) return ret;

    uint8_t newVal = enable
        ? (current |  kMSI_EC_COOLER_BOOST_MASK)
        : (current & ~kMSI_EC_COOLER_BOOST_MASK);

    MSIEC_LOG("setCoolerBoost: %s EC[0x98]: 0x%02X -> 0x%02X",
               enable ? "ON" : "OFF", current, newVal);

    return ecWriteLocked(kMSI_EC_COOLER_BOOST_ADDR, newVal);
}

// ---------------------------------------------------------------------------
// setShiftMode — CPU+GPU performance profile via EC 0xF2
//
// eco     (0xC2): minimum frequencies, lowest power draw
// comfort (0xC1): balanced frequencies (firmware default)
// turbo   (0xC0): maximum frequencies
// ---------------------------------------------------------------------------

IOReturn MSIECCore::setShiftMode(MSIShiftModeValue mode) {
    uint8_t ecVal;
    switch (mode) {
        case kMSIShiftEco:     ecVal = kMSI_EC_SHIFT_ECO;     break;
        case kMSIShiftComfort: ecVal = kMSI_EC_SHIFT_COMFORT; break;
        case kMSIShiftTurbo:   ecVal = kMSI_EC_SHIFT_TURBO;   break;
        default: return kIOReturnBadArgument;
    }
    MSIEC_LOG("setShiftMode: mode=%d -> EC[0xF2]=0x%02X", (int)mode, ecVal);
    return ecWrite(kMSI_EC_SHIFT_MODE_ADDR, ecVal);
}

// ---------------------------------------------------------------------------
// getSystemState — aggregated single-pass read (temp + fan% + modes)
//
// Returns the 7 most useful EC registers in one bus transaction.
// Used by the LaunchAgent polling loop (selector 9).
//
// Individual read failures are tolerated (value stays 0) so that a single
// timeout does not block the entire state snapshot.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::getSystemState(MSISystemState &out) {
    uint8_t cpuTemp = 0, gpuTemp = 0;
    uint8_t cpuPct  = 0, gpuPct  = 0;
    uint8_t fanModeRaw = 0, shiftRaw = 0, boostRaw = 0;

    {
        BusGuard bus;
        if (bus.status() != kIOReturnSuccess) return bus.status();
        ecReadLocked(kMSI_EC_CPU_TEMP_ADDR,     cpuTemp);
        ecReadLocked(kMSI_EC_GPU_TEMP_ADDR,     gpuTemp);
        ecReadLocked(kMSI_EC_CPU_FAN_PCT_ADDR,  cpuPct);
        ecReadLocked(kMSI_EC_GPU_FAN_PCT_ADDR,  gpuPct);
        ecReadLocked(kMSI_EC_FAN_MODE_ADDR,     fanModeRaw);
        ecReadLocked(kMSI_EC_SHIFT_MODE_ADDR,   shiftRaw);
        ecReadLocked(kMSI_EC_COOLER_BOOST_ADDR, boostRaw);
    }

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

IOReturn MSIECCore::setCameraState(bool cameraOff) {
    uint8_t val = cameraOff ? kMSI_EC_CAM_OFF : kMSI_EC_CAM_ACTIVE;
    MSIEC_LOG("setCameraState: cameraOff=%d -> EC[0x2E]=0x%02X", (int)cameraOff, val);
    return ecWrite(kMSI_EC_OFFSET_CAMERA, val);
}

// ---------------------------------------------------------------------------
// readFanRPM — reads the 4 fan RPM registers (0xCA–0xCD) in one transaction
// so the hi/lo bytes of each fan are consistent. Any timeout returns an error
// rather than computing RPM from stale zero values.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::readFanRPM(uint16_t &outCpuRPM, uint16_t &outGpuRPM) {
    static const uint8_t offsets[4] = {
        kMSI_EC_FAN_GPU_HI, kMSI_EC_FAN_GPU_LO, kMSI_EC_FAN_CPU_HI, kMSI_EC_FAN_CPU_LO
    };
    uint8_t v[4] = {};
    IOReturn r = readRegisters(offsets, v, 4);
    if (r != kIOReturnSuccess) return r;
    outGpuRPM = msiECToRPM(v[0], v[1]);
    outCpuRPM = msiECToRPM(v[2], v[3]);
    return kIOReturnSuccess;
}
