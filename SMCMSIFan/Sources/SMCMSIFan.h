// ---------------------------------------------------------------------------
// SMCMSIFan.h
//
// VirtualSMC plugin for MSI Modern 15 A10M.
// Publishes the following SMC keys (readable by iStatMenus, HWMonitorSMC2, etc.):
//   F0Ac / F0ID / F0Md / F0Mn / F0Mx  — fan 0, "CPU" (EC 0xCC-0xCD)
//   F1Ac / F1ID / F1Md / F1Mn / F1Mx  — fan 1, "GPU" (EC 0xCA-0xCB)
//   FNum                               — number of fans (2)
//   FxMd: 1 while Cooler Boost forces the fans, else 0 (read-only)
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
    static _Atomic(uint16_t) fanRPM[2];   // [0] CPU fan, [1] GPU fan
    static _Atomic(uint8_t)  gpuTempC;
    static _Atomic(uint8_t)  fanForced;   // Cooler Boost bit of EC 0x98
};

// ---------------------------------------------------------------------------
// SMC value classes — VirtualSMC calls readAccess() before returning a key.
// They only read the cache: no EC access, no lock, safe in any context.
// ---------------------------------------------------------------------------

// FxAc — current fan RPM (fpe2 format)
class SMCFanRPMValue : public VirtualSMCValue {
    const size_t fan;
public:
    explicit SMCFanRPMValue(size_t fanIndex) : fan(fanIndex) {}
protected:
    SMC_RESULT readAccess() override;
};

// FxMd — fan mode (ui8): 1 = forced (Cooler Boost, both fans), 0 = automatic curve
class SMCFanModeValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override;
};

// TG0P — integrated GPU temperature (sp78 format)
class SMCGpuTempValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override;
};

#endif /* SMCMSIFan_h */
