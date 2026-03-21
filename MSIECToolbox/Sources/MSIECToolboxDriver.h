// ---------------------------------------------------------------------------
// MSIECToolboxDriver.h
//
// Minimal IOService subclass that exposes the UserClient endpoint.
// Actual EC writes are performed by MSIECToolbox.cpp (Lilu plugin).
// IOKit instantiates the UserClient automatically via the IOUserClientClass
// key in Info.plist (preferred over the deprecated newUserClient() override).
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
};

#endif /* MSIECToolboxDriver_h */
