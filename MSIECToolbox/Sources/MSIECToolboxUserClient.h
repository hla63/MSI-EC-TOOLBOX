#ifndef MSIECToolboxUserClient_h
#define MSIECToolboxUserClient_h

#include <IOKit/IOUserClient.h>
#include "MSIECToolboxShared.h"

class MSIECToolboxUserClient : public IOUserClient {
    OSDeclareDefaultStructors(MSIECToolboxUserClient)

public:
    bool     initWithTask(task_t owningTask, void *securityToken,
                          UInt32 type, OSDictionary *props) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    IOReturn clientClose() override;

    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    static IOReturn sActionSetMuteState   (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetMuteState   (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionDumpEC         (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetCameraState (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetAllState    (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionReadFanRPM     (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetFanMode     (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetCoolerBoost (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetShiftMode   (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetSystemState (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetKbBacklight (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetKbBacklight    (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetBatteryCharge  (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetBatteryCharge  (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionSetFanCurve       (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);
    static IOReturn sActionGetFanCurve       (OSObject *target, void *ref,
                                            IOExternalMethodArguments *args);

    static IOExternalMethodDispatch sMethods[kMSISelectorCount];
};

#endif /* MSIECToolboxUserClient_h */
