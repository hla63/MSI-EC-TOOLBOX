// ---------------------------------------------------------------------------
// SMCMSIFan.cpp
//
// Lilu + VirtualSMC plugin — exposes fan RPM and temperatures for the
// MSI Modern 15 A10M as standard SMC keys.
//
// Published keys:
//   F0Ac / F0Mn / F0Mx / FNum  — CPU fan RPM (ISW formula, dynamic read)
//   TC0P                        — CPU package temperature (EC 0x68, sp78)
//   TG0P                        — integrated GPU temperature (EC 0x80, sp78)
//
// Flow:
//   pluginStart() registers the 6 SMC keys via VirtualSMCAPI.
//   readValue() on each key reads the EC on demand (pull model, no timer).
// ---------------------------------------------------------------------------

#include "SMCMSIFan.h"

// ---------------------------------------------------------------------------
// Lilu plugin registration
// ---------------------------------------------------------------------------

static const char *bootargOff[]   = { "-smcmsifan.off"  };
static const char *bootargDebug[] = { "-smcmsifan.dbg"  };
static const char *bootargBeta[]  = { "-smcmsifan.beta" };

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion("1.0.0"),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
    bootargOff,   arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta,  arrsize(bootargBeta),
    KernelVersion::Sonoma,
    KernelVersion::Tahoe,
    []() { SMCMSIFan::pluginStart(); }
};

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

IOLock                 *SMCMSIFan::ecLock      = nullptr;
VirtualSMCAPI::Plugin   SMCMSIFan::vsmcPlugin  {};

// ---------------------------------------------------------------------------
// pluginStart — Lilu entry point
// ---------------------------------------------------------------------------

void SMCMSIFan::pluginStart() {
    DBGLOG("SMCMSIFan", "pluginStart");
    ecLock = IOLockAlloc();
    if (!ecLock) { DBGLOG("SMCMSIFan", "IOLockAlloc ecLock failed"); return; }

    // Keys must be registered in strictly ascending uint32_t order.
    // VirtualSMCAPI::addKey() inserts into a sorted array; out-of-order
    // insertions cause a registration failure at boot.
    //
    // SmcKeyTypeFpe2 = 0x66706532 ('fpe2') — defined in SMCMSIFanKeys.h
    // if absent from kern_smcinfo.hpp.
    //
    // sortPlugin / registerPlugin do not exist in this SDK:
    //   - keys are already sorted at insertion time
    //   - registration happens via the ADDPR(vsmcPlugin) export below

    // F0Ac — current RPM, re-read on every SMC request via SMCFanRPMValue
    VirtualSMCAPI::addKey(KeyF0Ac, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(
            kSMCFanMinRPM,
            SmcKeyTypeFpe2,
            new SMCFanRPMValue(),
            SMC_KEY_ATTRIBUTE_READ));

    // F0Mn — minimum RPM (static)
    VirtualSMCAPI::addKey(KeyF0Mn, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(
            kSMCFanMinRPM,
            SmcKeyTypeFpe2,
            nullptr,
            SMC_KEY_ATTRIBUTE_READ));

    // F0Mx — maximum RPM (static)
    VirtualSMCAPI::addKey(KeyF0Mx, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(
            kSMCFanMaxRPM,
            SmcKeyTypeFpe2,
            nullptr,
            SMC_KEY_ATTRIBUTE_READ));

    // FNum — number of fans (1 on A10M)
    VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data,
        VirtualSMCAPI::valueWithUint8(1));

    // TC0P — CPU package temperature (sp78, dynamic read from EC 0x68)
    VirtualSMCAPI::addKey(KeyTC0P, vsmcPlugin.data,
        VirtualSMCAPI::valueWithSp(
            0,
            SmcKeyTypeSp78,
            new SMCCpuTempValue(),
            SMC_KEY_ATTRIBUTE_READ));

    // TG0P — integrated GPU temperature (sp78, dynamic read from EC 0x80)
    // Returns 0 when the iGPU is idle (normal at rest on A10M)
    VirtualSMCAPI::addKey(KeyTG0P, vsmcPlugin.data,
        VirtualSMCAPI::valueWithSp(
            0,
            SmcKeyTypeSp78,
            new SMCGpuTempValue(),
            SMC_KEY_ATTRIBUTE_READ));

    DBGLOG("SMCMSIFan", "SMC keys registered: F0Ac/Mn/Mx/FNum/TC0P/TG0P");
}

// VirtualSMC discovers this plugin via the global ADDPR(vsmcPlugin) symbol.
// Lilu (plugin_start.hpp) scans loaded kexts for this symbol.
// Must be exported as a value (not a pointer) — VirtualSMC dereferences
// the struct directly; exporting a pointer would produce an invalid read.
EXPORT VirtualSMCAPI::Plugin ADDPR(vsmcPlugin) {};  // filled by pluginStart()

// ---------------------------------------------------------------------------
// EC I/O helpers
// Duplicated from MSIECToolbox.cpp intentionally — SMCMSIFan must remain
// an independent kext with no runtime dependency on MSIECToolbox symbols.
// ---------------------------------------------------------------------------

bool SMCMSIFan::ecWaitIBF() {
    for (int i = 0; i < 1000; i++) {  // 10ms max (1000 x 10us)
        uint8_t status;
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"(kECCommandPort));
        if (!(status & 0x02)) return true;
        IODelay(10);
    }
    DBGLOG("SMCMSIFan", "ecWaitIBF: timeout (10ms)");
    return false;
}

bool SMCMSIFan::ecWaitOBF() {
    for (int i = 0; i < 1000; i++) {  // 10ms max (1000 x 10us)
        uint8_t status;
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"(kECCommandPort));
        if (status & 0x01) return true;
        IODelay(10);
    }
    DBGLOG("SMCMSIFan", "ecWaitOBF: timeout (10ms)");
    return false;
}

IOReturn SMCMSIFan::ecRead(uint32_t offset, uint8_t &outVal) {
    if (!ecLock) return kIOReturnNotReady;
    IOLockLock(ecLock);

    IOReturn ret = kIOReturnSuccess;
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)offset), "Nd"(kECDataPort));
    if (!ecWaitOBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("inb %1, %0" : "=a"(outVal) : "Nd"(kECDataPort));

done:
    IOLockUnlock(ecLock);
    return ret;
}

// ---------------------------------------------------------------------------
// readCpuRPM — reads EC 0xCC-0xCD and applies the ISW formula
// Both bytes are read under a single ecLock to prevent interleaving with
// other EC accesses, which would produce an inconsistent hi/lo pair.
// Returns 0 on timeout (fan stopped or EC unavailable).
// ---------------------------------------------------------------------------

uint16_t SMCMSIFan::readCpuRPM() {
    if (!ecLock) return 0;
    IOLockLock(ecLock);

    uint8_t hi = 0, lo = 0;
    IOReturn ret = kIOReturnSuccess;

    // Read 0xCC (high byte) — ecLock held
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)0xCC), "Nd"(kECDataPort));
    if (!ecWaitOBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("inb %1, %0" : "=a"(hi) : "Nd"(kECDataPort));

    // Read 0xCD (low byte) — ecLock still held
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"(kECOpRead), "Nd"(kECCommandPort));
    if (!ecWaitIBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("outb %0, %1" :: "a"((uint8_t)0xCD), "Nd"(kECDataPort));
    if (!ecWaitOBF()) { ret = kIOReturnTimeout; goto done; }
    asm volatile("inb %1, %0" : "=a"(lo) : "Nd"(kECDataPort));

done:
    IOLockUnlock(ecLock);
    if (ret != kIOReturnSuccess) return 0;

    uint16_t val = ((uint16_t)hi << 8) | lo;
    if (val == 0)  return 0;
    if (val > 325) val = 325;
    return (uint16_t)(((325 - val) * 16) + 1480);
}

// ---------------------------------------------------------------------------
// SMC value readValue() implementations
// VirtualSMC calls these on every userspace SMC key read request.
// ---------------------------------------------------------------------------

SMC_RESULT SMCFanRPMValue::readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src) {
    // fpe2 big-endian: stored value = RPM * 4
    uint16_t rpm = SMCMSIFan::readCpuRPM();
    *reinterpret_cast<uint16_t *>(data) = encodeFpe2(rpm);
    return SmcSuccess;
}

// readCpuTemp / readGpuTemp delegate to ecRead() which holds ecLock.
// No additional locking needed here.

uint8_t SMCMSIFan::readCpuTemp() {
    uint8_t val = 0;
    ecRead(0x68, val);
    DBGLOG("SMCMSIFan", "readCpuTemp: %d C", val);
    return val;
}

uint8_t SMCMSIFan::readGpuTemp() {
    uint8_t val = 0;
    ecRead(0x80, val);
    DBGLOG("SMCMSIFan", "readGpuTemp: %d C", val);
    return val;
}

// TC0P — CPU package temperature (sp78 big-endian: stored = T * 256)
SMC_RESULT SMCCpuTempValue::readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src) {
    uint8_t temp = SMCMSIFan::readCpuTemp();
    *reinterpret_cast<uint16_t *>(data) = encodeSp78(temp);
    return SmcSuccess;
}

// TG0P — integrated GPU temperature (sp78)
SMC_RESULT SMCGpuTempValue::readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src) {
    uint8_t temp = SMCMSIFan::readGpuTemp();
    *reinterpret_cast<uint16_t *>(data) = encodeSp78(temp);
    return SmcSuccess;
}
