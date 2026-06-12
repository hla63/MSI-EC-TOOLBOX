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
    // Never free stateLock / ecLock here.
    // The Lilu hook (when installed) and any in-flight UserClient call read
    // these static pointers with a null-check-then-lock sequence: freeing the
    // lock between the check and IOLockLock() is a use-after-free → panic.
    // The agent polls every 500ms, so the race window is real whenever the
    // driver terminates (sleep/wake, kextunload). Leaving two IOLocks
    // allocated for the kext lifetime is an acceptable micro-leak.
    IOService::stop(provider);
}
