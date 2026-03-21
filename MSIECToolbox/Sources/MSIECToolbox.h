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
#else
#define MSIEC_LOG(fmt, ...)  do {} while(0)
#define MSIEC_ERR(fmt, ...)  IOLog("MSIECToolbox [ERR]: " fmt "\n", ##__VA_ARGS__)
#endif

class MSIECToolbox {
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

    static IOReturn setBatteryCharge(uint8_t percent);  // 80 or 100
    static IOReturn getBatteryCharge(uint8_t &outPercent);

    // --- Raw EC I/O fallback ------------------------------------------------
    // Public: required by MSIECToolboxUserClient and MSIECToolboxDriver.

    static IOReturn fallbackECWrite(uint32_t offset, uint8_t value);
    static IOReturn fallbackECRead (uint32_t offset, uint8_t &outValue);

    // Returns true if the writeECField hook is installed.
    // Used by Driver::stop() to avoid freeing stateLock while an active hook
    // may still reference it.
    static bool isHookInstalled() {
        return atomic_load_explicit(&hookInstalled, memory_order_acquire);
    }

    // stateLock / ecLock: public so that MSIECToolboxDriver::start/stop can
    // initialise and release them independently of the Lilu hook.
    //   stateLock: protects speakerMuted / micMuted (short critical section)
    //   ecLock:    serialises EC bus access (port I/O sequences are non-reentrant)
    static IOLock *stateLock;
    static IOLock *ecLock;

private:
    // kextInfo lifetime must outlast the Lilu callback; static storage
    // in __DATA guarantees this. Non-const because onKextLoad() may update
    // loadIndex during the callback.
    static KernelPatcher::KextInfo kextInfo;

    static mach_vm_address_t orgWriteECField;

    // hookInstalled is written from patcherCallback() (Lilu thread) and read
    // from setMuteState() (IOKit UserClient thread) without a lock.
    // _Atomic ensures cross-thread visibility with memory_order_acquire/release.
    static _Atomic(bool) hookInstalled;

    // Protected by stateLock
    static bool speakerMuted;
    static bool micMuted;

    static void patcherCallback(void *user, KernelPatcher &patcher,
                                size_t index, mach_vm_address_t address, size_t size);

    static IOReturn hookedWriteECField(IOACPIPlatformDevice *device,
                                       uint32_t offset,
                                       const void *value,
                                       IOByteCount size);

    static bool ecWaitIBF();
    static bool ecWaitOBF();

    // MSIECToolboxDriver needs access to stateLock from start()/stop().
    // All other members remain private.
    friend class MSIECToolboxDriver;
};

#endif /* MSIECToolbox_h */
