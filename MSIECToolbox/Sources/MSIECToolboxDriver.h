// ---------------------------------------------------------------------------
// MSIECToolboxDriver.h
//
// Minimal IOService attached to the EC ACPI device (PNP0C09).
//   - exposes the UserClient endpoint (IOUserClientClass key in Info.plist)
//   - serves kMSIECReadRegistersFunction to SMCMSIFan, so both kexts go
//     through the same EC bus lock
// EC access itself lives in MSIECCore (MSIECToolbox.cpp).
// ---------------------------------------------------------------------------

#ifndef MSIECToolboxDriver_h
#define MSIECToolboxDriver_h

#include <IOKit/IOService.h>
#include "MSIECToolbox.h"

class MSIECToolboxDriver : public IOService {
    OSDeclareDefaultStructors(MSIECToolboxDriver)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider)  override;

    using IOService::callPlatformFunction;
    IOReturn callPlatformFunction(const OSSymbol *functionName,
                                  bool waitForFunction,
                                  void *param1, void *param2,
                                  void *param3, void *param4) override;
};

#endif /* MSIECToolboxDriver_h */
