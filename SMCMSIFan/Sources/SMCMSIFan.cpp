// ---------------------------------------------------------------------------
// SMCMSIFan.cpp
//
// VirtualSMC plugin — exposes fan RPM and temperatures for the
// MSI Modern 15 A10M as standard SMC keys.
//
// Published keys:
//   F0Ac / F0Mn / F0Mx / FNum  — CPU fan RPM (ISW formula)
//   TG0P                        — integrated GPU temperature (EC 0x80, sp78)
//
// Flow:
//   start()                  adds the keys, waits for VirtualSMC
//   vsmcNotificationHandler  submits the plugin, starts a 1s poller
//   refreshSensors()         reads the EC through MSIECToolboxDriver
//                            (kMSIECReadRegistersFunction) into a cache
//   readAccess()             returns the cache
// ---------------------------------------------------------------------------

#include "SMCMSIFan.h"
#include <Headers/kern_version.hpp>

OSDefineMetaClassAndStructors(SMCMSIFan, IOService)

bool     ADDPR(debugEnabled)    = false;
uint32_t ADDPR(debugPrintDelay) = 0;

_Atomic(uint16_t) SMCMSIFan::cpuRPM   = 0;
_Atomic(uint8_t)  SMCMSIFan::gpuTempC = 0;
_Atomic(uint8_t)  SMCMSIFan::fanForced = 0;

// ---------------------------------------------------------------------------
// IOService lifecycle
// ---------------------------------------------------------------------------

IOService *SMCMSIFan::probe(IOService *provider, SInt32 *score) {
    if (checkKernelArgument("-smcmsifan.off")) {
        SYSLOG("msifan", "disabled by -smcmsifan.off");
        return nullptr;
    }
    return IOService::probe(provider, score);
}

bool SMCMSIFan::start(IOService *provider) {
    if (!IOService::start(provider)) {
        SYSLOG("msifan", "failed to start the parent");
        return false;
    }

    setProperty("VersionInfo", kextVersion);

    readRegsSymbol = OSSymbol::withCString(kMSIECReadRegistersFunction);
    if (!readRegsSymbol) {
        SYSLOG("msifan", "failed to create function symbol");
        return false;
    }

    // Keys must be added in strictly ascending order (sorted key storage).
    VirtualSMCAPI::addKey(KeyF0Ac, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new SMCFanRPMValue, SMC_KEY_ATTRIBUTE_READ));
    const MSIFanDescription fanDesc;
    VirtualSMCAPI::addKey(KeyF0ID, vsmcPlugin.data,
        VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA *>(&fanDesc), sizeof(fanDesc),
                                     SmcKeyTypeFds, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));
    // Read-only: fan speed is controlled by the agent's modes and curve,
    // never by SMC clients.
    VirtualSMCAPI::addKey(KeyF0Md, vsmcPlugin.data,
        VirtualSMCAPI::valueWithUint8(0, new SMCFanModeValue, SMC_KEY_ATTRIBUTE_READ));
    VirtualSMCAPI::addKey(KeyF0Mn, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(kSMCFanMinRPM, SmcKeyTypeFpe2, nullptr, SMC_KEY_ATTRIBUTE_READ));
    VirtualSMCAPI::addKey(KeyF0Mx, vsmcPlugin.data,
        VirtualSMCAPI::valueWithFp(kSMCFanMaxRPM, SmcKeyTypeFpe2, nullptr, SMC_KEY_ATTRIBUTE_READ));
    VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data,
        VirtualSMCAPI::valueWithUint8(1));
    // TG0P returns 0 when the iGPU is idle (normal at rest on A10M)
    VirtualSMCAPI::addKey(KeyTG0P, vsmcPlugin.data,
        VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new SMCGpuTempValue, SMC_KEY_ATTRIBUTE_READ));

    vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
    if (!vsmcNotifier) {
        SYSLOG("msifan", "failed to register VirtualSMC handler");
        return false;
    }
    return true;
}

bool SMCMSIFan::vsmcNotificationHandler(void *sensors, void *refCon,
                                        IOService *vsmc, IONotifier *notifier) {
    if (!sensors || !vsmc) {
        SYSLOG("msifan", "got null vsmc notification");
        return false;
    }

    auto self = static_cast<SMCMSIFan *>(sensors);
    auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true,
                                          sensors, &self->vsmcPlugin, nullptr, nullptr);
    if (ret == kIOReturnUnsupported) {
        DBGLOG("msifan", "plugin submission to non vsmc");
        return false;
    }
    if (ret != kIOReturnSuccess) {
        SYSLOG("msifan", "plugin submission failure %X", ret);
        return false;
    }
    DBGLOG("msifan", "submitted plugin");

    // Dedicated workloop: a refresh may wait on the EC bus / ACPI global lock,
    // which must not stall the shared platform workloop that IOResources
    // clients get from getWorkLoop().
    self->workloop = IOWorkLoop::workLoop();
    self->poller = IOTimerEventSource::timerEventSource(self,
        [](OSObject *object, IOTimerEventSource *) {
            auto fan = OSDynamicCast(SMCMSIFan, object);
            if (fan) fan->refreshSensors();
        });

    if (!self->workloop || !self->poller) {
        SYSLOG("msifan", "failed to create poller or workloop");
        return false;
    }
    if (self->workloop->addEventSource(self->poller) != kIOReturnSuccess) {
        SYSLOG("msifan", "failed to add timer event source to workloop");
        OSSafeReleaseNULL(self->poller);
        return false;
    }

    // First sample right away so keys are not stuck at 0 for a second.
    self->poller->setTimeoutMS(1);
    return true;
}

void SMCMSIFan::stop(IOService *provider) {
    // kern_stop refuses unloading and VirtualSMC keeps our key values, so
    // stop() only has to halt polling.
    if (poller) {
        poller->cancelTimeout();
        if (workloop) workloop->removeEventSource(poller);
        OSSafeReleaseNULL(poller);
    }
    OSSafeReleaseNULL(workloop);
    if (vsmcNotifier) {
        vsmcNotifier->remove();
        vsmcNotifier = nullptr;
    }
    OSSafeReleaseNULL(ecService);
    OSSafeReleaseNULL(readRegsSymbol);
    IOService::stop(provider);
}

// ---------------------------------------------------------------------------
// refreshSensors — runs on the workloop every PollIntervalMS
//
// The EC is never touched directly: MSIECToolboxDriver owns the bus lock
// (and the ACPI global lock), and serves batched reads through
// callPlatformFunction. Reading the 4 registers in one batch also keeps the
// RPM hi/lo bytes consistent.
// ---------------------------------------------------------------------------

void SMCMSIFan::refreshSensors() {
    if (!ecService) {
        // MSIECToolbox may start after us: look it up again on each tick.
        auto match = IOService::serviceMatching("MSIECToolboxDriver");
        if (match) {
            ecService = IOService::copyMatchingService(match);
            match->release();
        }
        if (!ecService)
            DBGLOG("msifan", "MSIECToolboxDriver not available yet");
    }

    if (ecService) {
        static const uint8_t offsets[4] = {
            kMSI_EC_GPU_TEMP_ADDR, kMSI_EC_FAN_CPU_HI, kMSI_EC_FAN_CPU_LO,
            kMSI_EC_COOLER_BOOST_ADDR,
        };
        uint8_t v[4] = {};
        IOReturn r = ecService->callPlatformFunction(readRegsSymbol, false,
                         const_cast<uint8_t *>(offsets), v,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(4)), nullptr);
        if (r == kIOReturnSuccess) {
            atomic_store_explicit(&gpuTempC, v[0], memory_order_relaxed);
            atomic_store_explicit(&cpuRPM, msiECToRPM(v[1], v[2]), memory_order_relaxed);
            atomic_store_explicit(&fanForced,
                                  (v[3] & kMSI_EC_COOLER_BOOST_MASK) ? 1 : 0, memory_order_relaxed);
        } else {
            // Keep the previous sample: one EC timeout should not make the
            // fan look stopped.
            DBGLOG("msifan", "EC read failed 0x%08X", r);
        }
    }

    poller->setTimeoutMS(PollIntervalMS);
}

// ---------------------------------------------------------------------------
// SMC value readAccess() implementations — cache only
// ---------------------------------------------------------------------------

SMC_RESULT SMCFanRPMValue::readAccess() {
    uint16_t rpm = atomic_load_explicit(&SMCMSIFan::cpuRPM, memory_order_relaxed);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, rpm);
    return SmcSuccess;
}

SMC_RESULT SMCFanModeValue::readAccess() {
    *reinterpret_cast<uint8_t *>(data) = atomic_load_explicit(&SMCMSIFan::fanForced, memory_order_relaxed);
    return SmcSuccess;
}

SMC_RESULT SMCGpuTempValue::readAccess() {
    uint8_t t = atomic_load_explicit(&SMCMSIFan::gpuTempC, memory_order_relaxed);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntSp(SmcKeyTypeSp78, t);
    return SmcSuccess;
}

// ---------------------------------------------------------------------------
// kmod entry points (MODULE_START / MODULE_STOP)
// ---------------------------------------------------------------------------

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    lilu_get_boot_args("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-smcmsifan.dbg") ||
                          checkKernelArgument("-vsmcdbg") ||
                          checkKernelArgument("-liludbgall");
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // VirtualSMC keeps pointers to our key values: never unload.
    return KERN_FAILURE;
}
