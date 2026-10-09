#ifndef MSIECToolbox_h
#define MSIECToolbox_h

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOLocks.h>
#include <stdatomic.h>

#include "MSIECToolboxShared.h"

// Logging macros — full logging in DEBUG builds, errors only in RELEASE
#ifdef DEBUG
#define MSIEC_LOG(fmt, ...)  IOLog("MSIECToolbox: " fmt "\n", ##__VA_ARGS__)
#define MSIEC_ERR(fmt, ...)  IOLog("MSIECToolbox [ERR]: " fmt "\n", ##__VA_ARGS__)
#define MSIEC_INFO(fmt, ...) IOLog("MSIECToolbox: " fmt "\n", ##__VA_ARGS__)
#else
#define MSIEC_LOG(fmt, ...)  do {} while(0)
#define MSIEC_ERR(fmt, ...)  IOLog("MSIECToolbox [ERR]: " fmt "\n", ##__VA_ARGS__)
#define MSIEC_INFO(fmt, ...) IOLog("MSIECToolbox: " fmt "\n", ##__VA_ARGS__)  // rare, always on
#endif

// EC logic shared by the Lilu hook, the UserClient and SMCMSIFan.
// Not named MSIECToolbox: Lilu's plugin_start.hpp already declares
// `class PRODUCT_NAME : IOService`, i.e. `class MSIECToolbox`.
class MSIECCore {
public:
    static void pluginStart();

    // --- UserClient interface -----------------------------------------------

    static IOReturn setMuteState(bool speakerMuted, bool micMuted);
    static IOReturn getMuteState(bool &outSpeaker, bool &outMic);
    static IOReturn setCameraState(bool cameraOff);
    static IOReturn dumpEC(uint8_t outData[256]);
    static IOReturn readFanRPM(uint16_t &outCpuRPM, uint16_t &outGpuRPM);

    // --- Fan / performance controls -----------------------------------------

    static IOReturn setFanMode(MSIFanModeValue mode);
    static IOReturn setCoolerBoost(bool enable);
    static IOReturn setShiftMode(MSIShiftModeValue mode);
    static IOReturn getSystemState(MSISystemState &out);
    static IOReturn setFanCurve(const MSIFanCurve &curve);
    static IOReturn getFanCurve(MSIFanCurve &out);

    // --- Keyboard backlight -------------------------------------------------

    static IOReturn setKbBacklight(uint8_t level);   // level: 0=off 1=low 2=med 3=high
    static IOReturn getKbBacklight(uint8_t &outLevel);

    // --- Battery charge limit -----------------------------------------------

    static IOReturn setBatteryCharge(uint8_t percent);  // 10-100, 100 = no limit
    static IOReturn getBatteryCharge(uint8_t &outPercent);

    // --- Fn / Win key swap (EC 0xBF bit 4) -----------------------------------

    // Read-modify-write under one BusGuard; outWasSwapped gets the previous state.
    static IOReturn setFnWinSwap(bool swapped, bool *outWasSwapped = nullptr);

    // --- Touchpad (relayed to the touchpad drivers, not the EC) -------------

    static IOReturn setTouchpad(uint8_t request, bool &outEnabled);  // request: kMSITouchpad*

    // --- EC bus access ------------------------------------------------------
    // Every EC access in both kexts goes through these, so a multi-byte
    // sequence is never interleaved with another one (see BusGuard).

    static IOReturn ecWrite(uint32_t offset, uint8_t value);
    static IOReturn ecRead (uint32_t offset, uint8_t &outValue);

    // Reads `count` registers in a single bus transaction.
    // Entry point for SMCMSIFan (via MSIECToolboxDriver::callPlatformFunction).
    static IOReturn readRegisters(const uint8_t *offsets, uint8_t *outValues, uint32_t count);

    // EC ACPI device (PNP0C09), set by MSIECToolboxDriver::start().
    // Used to take the ACPI global lock around raw port I/O.
    static void setECDevice(IOACPIPlatformDevice *device);

    // ecLock: serialises EC bus access (port I/O sequences are non-reentrant).
    // Initialised by pluginStart() or MSIECToolboxDriver::start(), whichever
    // runs first, and never freed (see MSIECToolboxDriver::stop()).
    static IOLock *ecLock;

    static bool allocLocks();

private:
    // Exclusive ownership of the EC bus for the lifetime of the object:
    // ecLock first (serialises this kext and SMCMSIFan), then the ACPI
    // global lock, which AML takes around EC fields declared with the
    // `Lock` rule. Port I/O is only allowed while status() is success.
    class BusGuard {
    public:
        BusGuard();
        ~BusGuard();
        IOReturn status() const { return st; }
    private:
        IOReturn st {kIOReturnNotReady};
        IOLock  *heldLock {nullptr};  // ecLock as seen by the constructor
        bool     haveGlobalLock {false};
        UInt32   globalLockToken {0};
        IOACPIPlatformDevice *device {nullptr};
    };

    // Raw RD_EC / WR_EC sequences — caller must hold a BusGuard.
    static IOReturn ecReadLocked (uint8_t offset, uint8_t &outValue);
    static IOReturn ecWriteLocked(uint8_t offset, uint8_t value);
    static bool ecWaitIBF();
    static bool ecWaitOBF();

    // kextInfo lifetime must outlast the Lilu callback; static storage
    // in __DATA guarantees this. Non-const because onKextLoad() may update
    // loadIndex during the callback.
    static const char *kextPaths[];
    static KernelPatcher::KextInfo kextInfo;

    static mach_vm_address_t orgWriteECField;

    // hookInstalled is written from patcherCallback() (Lilu thread) and read
    // from other threads without a lock.
    static _Atomic(bool) hookInstalled;

    static _Atomic(IOACPIPlatformDevice *) ecDevice;

    // Cleared on the first non-timeout failure of acquireGlobalLock()
    // (firmware without a global lock): raw I/O then only relies on ecLock.
    static _Atomic(bool) globalLockUsable;

    // Read by hookedWriteECField in ACPI context, where taking a mutex is
    // not allowed: atomics only.
    static _Atomic(bool) speakerMuted;
    static _Atomic(bool) micMuted;

    // DEBUG builds log the first calls of the hook so its offset/size
    // semantics can be checked on real hardware.
    static _Atomic(uint32_t) hookCallsLogged;

    static void patcherCallback(void *user, KernelPatcher &patcher,
                                size_t index, mach_vm_address_t address, size_t size);

    static IOReturn hookedWriteECField(IOACPIPlatformDevice *device,
                                       uint32_t offset,
                                       const void *value,
                                       IOByteCount size);

    static IOReturn setLEDBit(BusGuard &bus, uint8_t offset, bool on);
};

#endif /* MSIECToolbox_h */
