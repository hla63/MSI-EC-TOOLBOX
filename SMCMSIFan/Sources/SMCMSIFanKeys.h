// ---------------------------------------------------------------------------
// SMCMSIFanKeys.h
// ---------------------------------------------------------------------------

#ifndef SMCMSIFanKeys_h
#define SMCMSIFanKeys_h

#include <Headers/kern_util.hpp>
#include <VirtualSMCSDK/kern_keyvalue.hpp>
#include <VirtualSMCSDK/kern_smcinfo.hpp>
#include <VirtualSMCSDK/AppleSmc.h>

// ---------------------------------------------------------------------------
// EC port I/O constants
// Duplicated from MSIECToolboxShared.h — SMCMSIFan is a standalone kext
// with no access to MSIECToolbox symbols at runtime.
// ---------------------------------------------------------------------------
static constexpr uint16_t kECCommandPort = 0x66;
static constexpr uint16_t kECDataPort    = 0x62;
static constexpr uint8_t  kECOpRead      = 0x80;  // RD_EC
static constexpr uint8_t  kECOpWrite     = 0x81;  // WR_EC

// ---------------------------------------------------------------------------
// SmcKeyFromStr — not exported by this SDK, defined locally
// ---------------------------------------------------------------------------
static constexpr SMC_KEY SmcKeyFromStr(const char *key) {
    return ((uint32_t)(uint8_t)key[0] << 24)
         | ((uint32_t)(uint8_t)key[1] << 16)
         | ((uint32_t)(uint8_t)key[2] <<  8)
         |  (uint32_t)(uint8_t)key[3];
}

// ---------------------------------------------------------------------------
// SMC keys — must be registered in strictly ascending uint32_t order.
// Current order: F0Ac < F0Mn < F0Mx < FNum < TC0P < TG0P
// ---------------------------------------------------------------------------
static constexpr SMC_KEY KeyF0Ac = SmcKeyFromStr("F0Ac");  // current CPU fan RPM
static constexpr SMC_KEY KeyF0Mn = SmcKeyFromStr("F0Mn");  // minimum CPU fan RPM
static constexpr SMC_KEY KeyF0Mx = SmcKeyFromStr("F0Mx");  // maximum CPU fan RPM
static constexpr SMC_KEY KeyFNum = SmcKeyFromStr("FNum");  // number of fans
static constexpr SMC_KEY KeyTC0P = SmcKeyFromStr("TC0P");  // CPU package temperature
static constexpr SMC_KEY KeyTG0P = SmcKeyFromStr("TG0P");  // GPU temperature

// RPM range for ISW formula on A10M: 1480 (val=325) to 6700 (val=0)
static constexpr uint16_t kSMCFanMinRPM = 1480;
static constexpr uint16_t kSMCFanMaxRPM = 6700;

// Encodes an RPM value to fpe2 big-endian format (stored = RPM * 4)
static inline uint16_t encodeFpe2(uint16_t rpm) {
    return __builtin_bswap16(static_cast<uint16_t>(rpm << 2));
}

// Encodes a temperature to sp78 big-endian format (stored = T * 256)
// sp78 is the standard Apple format for all TCxx / TGxx SMC keys.
static inline uint16_t encodeSp78(uint8_t tempC) {
    return __builtin_bswap16(static_cast<uint16_t>(tempC) << 8);
}

// SmcKeyTypeSp78 is defined in AppleSmc.h (VirtualSMC SDK) — do not redefine.

#endif /* SMCMSIFanKeys_h */
