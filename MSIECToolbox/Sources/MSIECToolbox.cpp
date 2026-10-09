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

// Target of the writeECField hook. Lilu calls loadKinfo() on every
// registered KextInfo when its patcher starts, and MachInfo::init()
// dereferences paths[0] in kernel collection mode (macOS 11+): paths must
// never be null, even though the binary is then read from memory.
// sys[Loaded] = true: AppleACPIPlatform is the platform expert and is always
// loaded before any Lilu plugin, so Lilu must process it as already loaded.
// kextInfo is non-const: onKextLoad() may update loadIndex.
const char *MSIECCore::kextPaths[] {
    "/System/Library/Extensions/AppleACPIPlatform.kext/Contents/MacOS/AppleACPIPlatform",
};

KernelPatcher::KextInfo MSIECCore::kextInfo {
    "com.apple.driver.AppleACPIPlatform",
    kextPaths, arrsize(kextPaths),
    {true},  // sys[KextInfo::Loaded]
    {},
    KernelPatcher::KextInfo::Unloaded
};

mach_vm_address_t               MSIECCore::orgWriteECField  = 0;
IOLock                         *MSIECCore::ecLock           = nullptr;
_Atomic(bool)                   MSIECCore::speakerMuted     = false;
_Atomic(bool)                   MSIECCore::micMuted         = false;
_Atomic(uint32_t)               MSIECCore::hookCallsLogged  = 0;
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
    return allocLockAtomic(&ecLock, "ecLock");
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
// set by setMuteState().
//
// Runs in ACPI context: no BusGuard (AML may hold the ACPI global lock), no
// mutex, no allocation — atomics only.
//
// The signature (offset, value, size) comes from the mangled symbol
// __ZN20IOACPIPlatformDevice12writeECFieldEjPKvl and has not been checked
// against Apple's implementation: offset is assumed to be a byte address and
// size a byte count. The value is only rewritten when the byte also looks
// like the LED register (firmware base value, LED bit aside); anything else
// passes through untouched, so a wrong assumption cannot corrupt other EC
// writes. DEBUG builds log the first calls to confirm the semantics.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::hookedWriteECField(IOACPIPlatformDevice *device,
                                       uint32_t offset,
                                       const void *value,
                                       IOByteCount size)
{
    using Fn = IOReturn (*)(IOACPIPlatformDevice *, uint32_t, const void *, IOByteCount);
    auto org = reinterpret_cast<Fn>(orgWriteECField);

#ifdef DEBUG
    if (atomic_fetch_add_explicit(&hookCallsLogged, 1, memory_order_relaxed) < 64) {
        MSIEC_LOG("writeECField: offset=0x%X size=%llu value[0]=0x%02X",
                  offset, (unsigned long long)size,
                  value ? *static_cast<const uint8_t *>(value) : 0);
    }
#endif

    if (value && size == 1 &&
        (offset == kMSI_EC_OFFSET_SPEAKER || offset == kMSI_EC_OFFSET_MIC))
    {
        bool    isSpeaker = (offset == kMSI_EC_OFFSET_SPEAKER);
        uint8_t base      = isSpeaker ? kMSI_EC_BASE_SPEAKER : kMSI_EC_BASE_MIC;
        uint8_t raw       = *static_cast<const uint8_t *>(value);

        if ((raw & ~kMSI_EC_BIT_LED) == base) {
            bool muted = atomic_load_explicit(isSpeaker ? &speakerMuted : &micMuted,
                                              memory_order_acquire);
            uint8_t modified = muted ? (raw | kMSI_EC_BIT_LED) : (raw & ~kMSI_EC_BIT_LED);
            MSIEC_LOG("EC write 0x%02X: raw=0x%02X patched=0x%02X muted=%d",
                       offset, raw, modified, (int)muted);
            return org(device, offset, &modified, size);
        }
    }

    return org(device, offset, value, size);
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
    atomic_store_explicit(&speakerMuted, newSpk, memory_order_release);
    atomic_store_explicit(&micMuted,     newMic, memory_order_release);

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
    outSpk = atomic_load_explicit(&speakerMuted, memory_order_acquire);
    outMic = atomic_load_explicit(&micMuted,     memory_order_acquire);
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
// percent: 10-99 = stop charging at that level (0x80 | percent, the msi-ec
//          encoding: 60% = 0xBC, 80% = 0xD0)
//          100   = no limit (0x64, firmware default, bit 7 clear)
// The old 0x50 value for 80% had bit 7 clear, i.e. no limit at all.
// Callers: the agent (selector 12) and SMCMSIFan (BCLM key).
// ---------------------------------------------------------------------------

IOReturn MSIECCore::setBatteryCharge(uint8_t percent) {
    if (percent < kMSIBatteryLimitMinPct || percent > 100)
        return kIOReturnBadArgument;
    uint8_t ecVal = (percent == 100) ? kMSI_EC_BATTERY_CHARGE_FULL
                                     : (uint8_t)(kMSI_EC_BATTERY_LIMIT_ENABLE | percent);
    MSIEC_LOG("setBatteryCharge: %d%% -> EC[0xEF]=0x%02X", (int)percent, ecVal);
    return ecWrite(kMSI_EC_BATTERY_CHARGE_ADDR, ecVal);
}

IOReturn MSIECCore::getBatteryCharge(uint8_t &outPercent) {
    uint8_t raw = 0;
    IOReturn r = ecRead(kMSI_EC_BATTERY_CHARGE_ADDR, raw);
    if (r != kIOReturnSuccess) return r;
    outPercent = msiECToBatteryLimit(raw);
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
// Safety floor (MSIECToolboxShared.h): >= 50 % from 70 °C and on the last point.
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
        if (curve.temps[i] >= kMSIFanCurveFloorTempC &&
            curve.speeds[i] < kMSIFanCurveFloorSpeedPct)  return kIOReturnBadArgument;
    }
    if (curve.speeds[kMSI_EC_FAN_CURVE_POINTS - 1] < kMSIFanCurveFloorSpeedPct)
        return kIOReturnBadArgument;

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
// setFnWinSwap — Fn / Windows key swap, EC 0xBF bit 4 (read-modify-write)
// ---------------------------------------------------------------------------

IOReturn MSIECCore::setFnWinSwap(bool swapped, bool *outWasSwapped) {
    BusGuard bus;
    if (bus.status() != kIOReturnSuccess) return bus.status();

    uint8_t current = 0;
    IOReturn ret = ecReadLocked(kMSI_EC_FN_WIN_SWAP_ADDR, current);
    if (ret != kIOReturnSuccess) return ret;

    bool was = (current & kMSI_EC_FN_WIN_SWAP_MASK) != 0;
    if (outWasSwapped) *outWasSwapped = was;
    if (was == swapped) return kIOReturnSuccess;

    uint8_t newVal = swapped
        ? (current |  kMSI_EC_FN_WIN_SWAP_MASK)
        : (current & ~kMSI_EC_FN_WIN_SWAP_MASK);
    MSIEC_LOG("setFnWinSwap: EC[0xBF]: 0x%02X -> 0x%02X", current, newVal);
    return ecWriteLocked(kMSI_EC_FN_WIN_SWAP_ADDR, newVal);
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
// Individual read failures are tolerated so that a single timeout does not
// block the whole snapshot, but they are reported in validMask: a failed
// read must not be shown as 0 °C / "Auto" / "Boost OFF", or the agent's
// toggles would act on a state that was never read.
// ---------------------------------------------------------------------------

IOReturn MSIECCore::getSystemState(MSISystemState &out) {
    static const struct { uint32_t addr; uint8_t bit; } regs[] = {
        { kMSI_EC_CPU_TEMP_ADDR,     kMSIStateValidCpuTemp     },
        { kMSI_EC_GPU_TEMP_ADDR,     kMSIStateValidGpuTemp     },
        { kMSI_EC_CPU_FAN_PCT_ADDR,  kMSIStateValidCpuFanPct   },
        { kMSI_EC_GPU_FAN_PCT_ADDR,  kMSIStateValidGpuFanPct   },
        { kMSI_EC_FAN_MODE_ADDR,     kMSIStateValidFanMode     },
        { kMSI_EC_SHIFT_MODE_ADDR,   kMSIStateValidShiftMode   },
        { kMSI_EC_COOLER_BOOST_ADDR, kMSIStateValidCoolerBoost },
    };
    uint8_t raw[arrsize(regs)] = {};
    uint8_t valid = 0;

    {
        BusGuard bus;
        if (bus.status() != kIOReturnSuccess) return bus.status();
        for (size_t i = 0; i < arrsize(regs); i++) {
            if (ecReadLocked(static_cast<uint8_t>(regs[i].addr), raw[i]) == kIOReturnSuccess)
                valid |= regs[i].bit;
            else
                raw[i] = 0;
        }
    }

    if (valid == 0) return kIOReturnTimeout;

    out.cpuTempC    = raw[0];
    out.gpuTempC    = raw[1];
    out.cpuFanPct   = raw[2];
    out.gpuFanPct   = raw[3];
    out.coolerBoost = (raw[6] & kMSI_EC_COOLER_BOOST_MASK) ? 1 : 0;

    switch (raw[4]) {
        case kMSI_EC_FAN_SILENT:   out.fanMode = kMSIFanModeSilent;   break;
        case kMSI_EC_FAN_ADVANCED: out.fanMode = kMSIFanModeAdvanced; break;
        default:                   out.fanMode = kMSIFanModeAuto;     break;
    }
    switch (raw[5]) {
        case kMSI_EC_SHIFT_ECO:   out.shiftMode = kMSIShiftEco;    break;
        case kMSI_EC_SHIFT_TURBO: out.shiftMode = kMSIShiftTurbo;  break;
        default:                  out.shiftMode = kMSIShiftComfort; break;
    }
    out.validMask = valid;

    MSIEC_LOG("getSystemState: CPU=%d°C fan=%d%% GPU=%d°C fanMode=%d shift=%d boost=%d valid=0x%02X",
               out.cpuTempC, out.cpuFanPct, out.gpuTempC,
               (int)out.fanMode, (int)out.shiftMode, (int)out.coolerBoost, valid);
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

// ---------------------------------------------------------------------------
// setTouchpad — enable/disable the touchpad from user space
//
// VoodooPS2's keyboard toggles the touchpad (PrtSc) by sending two IOKit
// messages to every service published with RM,deliverNotifications = true.
// VoodooI2CHID (I2C touchpads) and the VoodooPS2 trackpads handle them; the
// keyboard ignores them. No user-space interface exists, so the kext relays
// the same messages. The state is read from the first touchpad driver found
// and the new state is sent to every consumer, as VoodooPS2 does.
// ---------------------------------------------------------------------------

static constexpr UInt32 kTouchpadMsgSetStatus = iokit_vendor_specific_msg(100);  // data: bool* enable
static constexpr UInt32 kTouchpadMsgGetStatus = iokit_vendor_specific_msg(101);  // data: bool* enabled

// Touchpad driver classes that handle the messages. Class matching includes
// subclasses (e.g. VoodooI2CPrecisionTouchpadHIDEventDriver).
static const char *const kTouchpadClasses[] = {
    "VoodooI2CMultitouchHIDEventDriver",  // VoodooI2CHID (precision and generic multitouch)
    "ApplePS2SynapticsTouchPad",
    "ApplePS2Elan",
    "ApplePS2ALPSGlidePoint",
    "ApplePS2SentelicFSP",
};

static bool isTouchpadDriver(IOService *svc) {
    for (const char *name : kTouchpadClasses)
        if (svc->metaCast(name)) return true;
    return false;
}

// Adds every service matching `match` to `set` (consumes `match`).
static void addMatchingServices(OSSet *set, OSDictionary *match) {
    if (!match) return;
    if (OSIterator *it = IOService::getMatchingServices(match)) {
        while (OSObject *obj = it->getNextObject())
            if (IOService *svc = OSDynamicCast(IOService, obj)) set->setObject(svc);
        it->release();
    }
    match->release();
}

IOReturn MSIECCore::setTouchpad(uint8_t request, bool &outEnabled) {
    if (request > kMSITouchpadToggle) return kIOReturnBadArgument;

    // Same audience as VoodooPS2 (RM,deliverNotifications) plus the touchpad
    // drivers found by class: in VoodooI2CHID only the Precision Touchpad
    // personality sets RM,deliverNotifications, while the generic Multitouch
    // driver handles the same messages without it. OSSet removes duplicates.
    OSSet *consumers = OSSet::withCapacity(4);
    if (!consumers) return kIOReturnNoMemory;
    if (const OSSymbol *key = OSSymbol::withCString("RM,deliverNotifications")) {
        addMatchingServices(consumers, IOService::propertyMatching(key, kOSBooleanTrue));
        key->release();
    }
    for (const char *name : kTouchpadClasses)
        addMatchingServices(consumers, IOService::serviceMatching(name));

    IOService *touchpad = nullptr;
    if (OSCollectionIterator *it = OSCollectionIterator::withCollection(consumers)) {
        while (OSObject *obj = it->getNextObject()) {
            IOService *svc = OSDynamicCast(IOService, obj);
            if (svc && isTouchpadDriver(svc)) { touchpad = svc; break; }
        }
        it->release();
    }
    if (!touchpad) {
        consumers->release();
        MSIEC_ERR("setTouchpad: no touchpad driver found (VoodooI2CHID / VoodooPS2)");
        return kIOReturnNotFound;
    }

    bool enabled = true;
    touchpad->message(kTouchpadMsgGetStatus, nullptr, &enabled);

    if (request != kMSITouchpadQuery) {
        bool want = (request == kMSITouchpadEnable)  ? true
                  : (request == kMSITouchpadDisable) ? false
                  : !enabled;
        if (OSCollectionIterator *it = OSCollectionIterator::withCollection(consumers)) {
            while (OSObject *obj = it->getNextObject()) {
                bool value = want;  // a consumer may write through the pointer
                if (IOService *svc = OSDynamicCast(IOService, obj))
                    svc->message(kTouchpadMsgSetStatus, nullptr, &value);
            }
            it->release();
        }
        enabled = true;
        touchpad->message(kTouchpadMsgGetStatus, nullptr, &enabled);
        MSIEC_LOG("setTouchpad: request=%d -> enabled=%d (%u consumers)",
                  request, (int)enabled, consumers->getCount());
    }

    consumers->release();
    outEnabled = enabled;
    return kIOReturnSuccess;
}

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
