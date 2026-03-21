// ---------------------------------------------------------------------------
// SMCMSIFan.h
//
// VirtualSMC plugin for MSI Modern 15 A10M.
// Publishes the following SMC keys (readable by iStatMenus, HWMonitorSMC2, etc.):
//   F0Ac / F0Mn / F0Mx / FNum  — CPU fan RPM
//   TC0P                        — CPU package temperature (°C)
//   TG0P                        — integrated GPU temperature (°C)
//
// Dependencies: Lilu >= 1.6.0, VirtualSMC >= 1.3.0
// ---------------------------------------------------------------------------

#ifndef SMCMSIFan_h
#define SMCMSIFan_h

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>

#include "SMCMSIFanKeys.h"

class SMCMSIFan {
public:
    static void pluginStart();

    // Exposed for the VirtualSMC export symbol ADDPR(vsmcPlugin)
    static VirtualSMCAPI::Plugin vsmcPlugin;

    // Temperature reads — called by the SMC value classes below
    static uint8_t readCpuTemp();
    static uint8_t readGpuTemp();

private:
    // Raw EC port I/O (same protocol as MSIECToolbox, duplicated intentionally
    // to keep SMCMSIFan independent — no cross-kext symbols at runtime)
    static IOReturn ecRead(uint32_t offset, uint8_t &outVal);
    static bool     ecWaitIBF();
    static bool     ecWaitOBF();

    // Reads CPU fan RPM from EC 0xCC-0xCD using the ISW formula
    static uint16_t readCpuRPM();

    friend class SMCFanRPMValue;
    friend class SMCCpuTempValue;
    friend class SMCGpuTempValue;

    // Local EC lock — SMCMSIFan is a separate kext with no access to
    // MSIECToolbox symbols at runtime. ecLock serialises raw port I/O
    // sequences against concurrent readValue() calls from VirtualSMC.
    static IOLock *ecLock;
};

// ---------------------------------------------------------------------------
// SMC value classes — VirtualSMC calls readValue() on each SMC key read
// ---------------------------------------------------------------------------

// F0Ac — current CPU fan RPM (fpe2 format)
class SMCFanRPMValue : public VirtualSMCValue {
public:
    SMC_RESULT readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src);
};

// TC0P — CPU package temperature (sp78 format)
class SMCCpuTempValue : public VirtualSMCValue {
public:
    SMC_RESULT readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src);
};

// TG0P — integrated GPU temperature (sp78 format)
class SMCGpuTempValue : public VirtualSMCValue {
public:
    SMC_RESULT readValue(const VirtualSMCKeyValue &kv, VirtualSMCValue *&src);
};

#endif /* SMCMSIFan_h */
