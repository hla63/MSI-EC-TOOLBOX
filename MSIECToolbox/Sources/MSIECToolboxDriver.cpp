// ---------------------------------------------------------------------------
// MSIECToolboxDriver.cpp
//
// Minimal IOService that publishes the UserClient endpoint.
// newUserClient() is omitted (deprecated in macOS 12+); IOKit instantiates
// the UserClient automatically via the IOUserClientClass key in Info.plist.
// ---------------------------------------------------------------------------

#include "MSIECToolboxDriver.h"
#include <IOKit/pwr_mgt/RootDomain.h>

OSDefineMetaClassAndStructors(MSIECToolboxDriver, IOService)

bool MSIECToolboxDriver::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    // ecLock is also allocated here, independently of pluginStart(), so that
    // EC access works even when Lilu does not start the plugin (-msiec.off,
    // unsupported macOS, ...). Only the writeECField hook needs Lilu.
    if (!MSIECCore::allocLocks()) return false;

    // provider is the PNP0C09 ACPI device (IONameMatch in Info.plist): its
    // ACPI global lock is what BusGuard takes around raw port I/O.
    MSIECCore::setECDevice(OSDynamicCast(IOACPIPlatformDevice, provider));

    // MSI Creator Center's Fn/Win swap survives a reboot and turns the Win
    // key (Command) into Fn on macOS. Clear it for macOS and give it back to
    // Windows at shutdown/restart. Not restored after a panic or a forced
    // power-off: Windows then keeps the keys unswapped until it is set again.
    bool wasSwapped = false;
    IOReturn swapRet = MSIECCore::setFnWinSwap(false, &wasSwapped);
    if (swapRet != kIOReturnSuccess) {
        MSIEC_ERR("Fn/Win swap: EC 0xBF not accessible (0x%08X)", swapRet);
    } else if (wasSwapped) {
        restoreFnWinSwap = true;
        // Priority interest: the clients IOPMrootDomain notifies before a halt or restart.
        haltNotifier = registerPrioritySleepWakeInterest(haltRestartHandler, this);
        if (haltNotifier)
            MSIEC_INFO("Fn/Win swap cleared for macOS, restored at shutdown/restart");
        else
            MSIEC_ERR("Fn/Win swap cleared, but restore at shutdown is unavailable");
    }

    MSIEC_LOG("MSIECToolboxDriver started, provider=%s", provider->getName());
    registerService();
    return true;
}

void MSIECToolboxDriver::stop(IOService *provider) {
    // Never free ecLock here.
    // SMCMSIFan and any in-flight UserClient call read this static pointer
    // with a null-check-then-lock sequence: freeing the lock between the
    // check and IOLockLock() is a use-after-free → panic. Leaving one IOLock
    // allocated for the kext lifetime is an acceptable micro-leak.
    if (haltNotifier) {
        haltNotifier->remove();
        haltNotifier = nullptr;
    }
    IOService::stop(provider);
}

// Runs on the power management thread, synchronously, before the platform
// halts or restarts; ACPI and the EC are still up. Sleep is ignored: macOS
// resumes from it.
IOReturn MSIECToolboxDriver::haltRestartHandler(void *target, void *, UInt32 messageType,
                                                IOService *, void *messageArgument, vm_size_t)
{
    if (messageType != kIOMessageSystemWillPowerOff && messageType != kIOMessageSystemWillRestart)
        return kIOReturnUnsupported;

    auto self = static_cast<MSIECToolboxDriver *>(target);
    if (self && self->restoreFnWinSwap) {
        self->restoreFnWinSwap = false;
        IOReturn r = MSIECCore::setFnWinSwap(true);
        if (r == kIOReturnSuccess)
            MSIEC_INFO("Fn/Win swap restored for the next OS");
        else
            MSIEC_ERR("Fn/Win swap not restored (0x%08X)", r);
    }

    // returnValue 0 + success: no extra time needed, counts as acknowledged.
    if (messageArgument)
        static_cast<IOPowerStateChangeNotification *>(messageArgument)->returnValue = 0;
    return kIOReturnSuccess;
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
    if (functionName && functionName->isEqualTo(kMSIECSetBatteryChargeFunction)) {
        auto percent = reinterpret_cast<uintptr_t>(param1);
        if (percent > 100) return kIOReturnBadArgument;
        return MSIECCore::setBatteryCharge(static_cast<uint8_t>(percent));
    }
    return IOService::callPlatformFunction(functionName, waitForFunction,
                                           param1, param2, param3, param4);
}
