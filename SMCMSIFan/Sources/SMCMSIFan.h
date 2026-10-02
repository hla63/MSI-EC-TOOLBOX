// ---------------------------------------------------------------------------
// SMCMSIFan.h
//
// VirtualSMC plugin for MSI Modern 15 A10M.
// Publishes the following SMC keys (readable by iStatMenus, HWMonitorSMC2, etc.):
//   F0Ac / F0Mn / F0Mx / FNum  — CPU fan RPM
//   TC0P                        — CPU package temperature (°C)
//   TG0P                        — integrated GPU temperature (°C)
//
// EC values are sampled every second through MSIECToolboxDriver (shared EC
// bus lock) and cached; SMC reads only return the cache.
//
// Dependencies: Lilu >= 1.6.0, VirtualSMC >= 1.3.0, MSIECToolbox.kext
// ---------------------------------------------------------------------------

#ifndef SMCMSIFan_h
#define SMCMSIFan_h

#include <Headers/kern_util.hpp>
#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <stdatomic.h>

#include "SMCMSIFanKeys.h"

class EXPORT SMCMSIFan : public IOService {
    OSDeclareDefaultStructors(SMCMSIFan)

    IONotifier         *vsmcNotifier   {nullptr};
    IOWorkLoop         *workloop       {nullptr};
    IOTimerEventSource *poller         {nullptr};
    IOService          *ecService      {nullptr};  // MSIECToolboxDriver, retained
    const OSSymbol     *readRegsSymbol {nullptr};

    // Must stay unchanged and allocated after submission (VirtualSMC API).
    VirtualSMCAPI::Plugin vsmcPlugin {
        xStringify(PRODUCT_NAME),
        parseModuleVersion(xStringify(MODULE_VERSION)),
        VirtualSMCAPI::Version,
    };

    static constexpr uint32_t PollIntervalMS {1000};

    void refreshSensors();

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    static bool vsmcNotificationHandler(void *sensors, void *refCon,
                                        IOService *vsmc, IONotifier *notifier);

    // Latest EC samples, read by the SMC value classes below. Static so that
    // the value objects owned by VirtualSMC never point into a freed instance.
    static _Atomic(uint16_t) cpuRPM;
    static _Atomic(uint8_t)  cpuTempC;
    static _Atomic(uint8_t)  gpuTempC;
};

// ---------------------------------------------------------------------------
// SMC value classes — VirtualSMC calls readAccess() before returning a key.
// They only read the cache: no EC access, no lock, safe in any context.
// ---------------------------------------------------------------------------

// F0Ac — current CPU fan RPM (fpe2 format)
class SMCFanRPMValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override;
};

// TC0P — CPU package temperature (sp78 format)
class SMCCpuTempValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override;
};

// TG0P — integrated GPU temperature (sp78 format)
class SMCGpuTempValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override;
};

#endif /* SMCMSIFan_h */
