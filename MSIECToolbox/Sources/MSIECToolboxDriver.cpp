// ---------------------------------------------------------------------------
// MSIECToolboxDriver.cpp
//
// Minimal IOService that publishes the UserClient endpoint.
// newUserClient() is omitted (deprecated in macOS 12+); IOKit instantiates
// the UserClient automatically via the IOUserClientClass key in Info.plist.
// ---------------------------------------------------------------------------

#include "MSIECToolboxDriver.h"
#include "MSIECToolbox.h"

OSDefineMetaClassAndStructors(MSIECToolboxDriver, IOService)

bool MSIECToolboxDriver::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    // Initialise stateLock and ecLock here, independently of pluginStart()
    // (Lilu thread). This ensures that setMuteState() never returns
    // kIOReturnNotReady if Lilu fails to call pluginStart() — e.g. when the
    // kext loads too late or Lilu rejects the plugin for any reason.
    // The raw EC I/O fallback works without the Lilu hook.
    //
    // Pattern: null check → alloc → CAS(nullptr → candidate).
    // If pluginStart() already installed the lock concurrently, free ours.
    if (!MSIECToolbox::stateLock) {
        IOLock *lk = IOLockAlloc();
        if (!lk) { MSIEC_ERR("IOLockAlloc stateLock failed"); return false; }
        if (!OSCompareAndSwapPtr(nullptr, lk, &MSIECToolbox::stateLock))
            IOLockFree(lk);  // another thread won the race
        else
            MSIEC_LOG("stateLock initialised by MSIECToolboxDriver::start()");
    }
    if (!MSIECToolbox::ecLock) {
        IOLock *lk = IOLockAlloc();
        if (!lk) { MSIEC_ERR("IOLockAlloc ecLock failed"); return false; }
        if (!OSCompareAndSwapPtr(nullptr, lk, &MSIECToolbox::ecLock))
            IOLockFree(lk);
        else
            MSIEC_LOG("ecLock initialised by MSIECToolboxDriver::start()");
    }

    MSIEC_LOG("MSIECToolboxDriver started, provider=%s", provider->getName());
    registerService();
    return true;
}

void MSIECToolboxDriver::stop(IOService *provider) {
    // Do not free stateLock if the Lilu hook is active.
    // hookedWriteECField() accesses stateLock after hook installation without
    // a null check — freeing the lock while the hook is live would kernel panic.
    // If hookInstalled=false (raw EC fallback only), we can free safely.
    // Leaving the locks allocated when the hook is active is an acceptable
    // micro-leak preferable to a kernel panic.
    if (!MSIECToolbox::isHookInstalled()) {
        if (MSIECToolbox::stateLock) {
            IOLockFree(MSIECToolbox::stateLock);
            MSIECToolbox::stateLock = nullptr;
        }
        if (MSIECToolbox::ecLock) {
            IOLockFree(MSIECToolbox::ecLock);
            MSIECToolbox::ecLock = nullptr;
        }
    }
    IOService::stop(provider);
}
