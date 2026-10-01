// ---------------------------------------------------------------------------
// MSIECToolboxDriver.cpp
//
// Minimal IOService that publishes the UserClient endpoint.
// newUserClient() is omitted (deprecated in macOS 12+); IOKit instantiates
// the UserClient automatically via the IOUserClientClass key in Info.plist.
// ---------------------------------------------------------------------------

#include "MSIECToolboxDriver.h"

OSDefineMetaClassAndStructors(MSIECToolboxDriver, IOService)

bool MSIECToolboxDriver::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    // Locks are also allocated here, independently of pluginStart(), so that
    // EC access works even when Lilu does not start the plugin (-msiec.off,
    // unsupported macOS, ...). Only the writeECField hook needs Lilu.
    if (!MSIECCore::allocLocks()) return false;

    // provider is the PNP0C09 ACPI device (IONameMatch in Info.plist): its
    // ACPI global lock is what BusGuard takes around raw port I/O.
    MSIECCore::setECDevice(OSDynamicCast(IOACPIPlatformDevice, provider));

    MSIEC_LOG("MSIECToolboxDriver started, provider=%s", provider->getName());
    registerService();
    return true;
}

void MSIECToolboxDriver::stop(IOService *provider) {
    // Never free stateLock / ecLock here.
    // The Lilu hook (when installed), SMCMSIFan and any in-flight UserClient
    // call read these static pointers with a null-check-then-lock sequence:
    // freeing the lock between the check and IOLockLock() is a use-after-free
    // → panic. Leaving two IOLocks allocated for the kext lifetime is an
    // acceptable micro-leak.
    IOService::stop(provider);
}

IOReturn MSIECToolboxDriver::callPlatformFunction(const OSSymbol *functionName,
                                                  bool waitForFunction,
                                                  void *param1, void *param2,
                                                  void *param3, void *param4)
{
    if (functionName && functionName->isEqualTo(kMSIECReadRegistersFunction)) {
        auto count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param3));
        return MSIECCore::readRegisters(static_cast<const uint8_t *>(param1),
                                        static_cast<uint8_t *>(param2), count);
    }
    return IOService::callPlatformFunction(functionName, waitForFunction,
                                           param1, param2, param3, param4);
}
