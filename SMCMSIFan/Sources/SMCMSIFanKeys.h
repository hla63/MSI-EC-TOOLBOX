// ---------------------------------------------------------------------------
// SMCMSIFanKeys.h
// ---------------------------------------------------------------------------

#ifndef SMCMSIFanKeys_h
#define SMCMSIFanKeys_h

#include <Headers/kern_util.hpp>
#include <VirtualSMCSDK/kern_keyvalue.hpp>
#include <VirtualSMCSDK/kern_smcinfo.hpp>
#include <VirtualSMCSDK/AppleSmc.h>

// EC register map and the kMSIECReadRegistersFunction interface.
// Header only: SMCMSIFan links no MSIECToolbox symbol.
#include "MSIECToolboxShared.h"

// ---------------------------------------------------------------------------
// SMC keys — VirtualSMC key storage must be sorted, so keys are added in
// strictly ascending order: F0xx < F1xx < FNum < TG0P (within a fan:
// Ac < ID < Md < Mn < Mx). The A10M has two fans: F0 = CPU (EC 0xCC-0xCD),
// F1 = GPU (EC 0xCA-0xCB), confirmed by MSI Creator Center and HWiNFO.
// TC0P (CPU package temperature) is not published: SMCProcessor already
// provides it from the CPU's own sensors, and two providers of one key make
// the value read by macOS ambiguous.
// ---------------------------------------------------------------------------
static constexpr SMC_KEY KeyF0Ac = SMC_MAKE_IDENTIFIER('F','0','A','c');  // current CPU fan RPM
static constexpr SMC_KEY KeyF0ID = SMC_MAKE_IDENTIFIER('F','0','I','D');  // fan description ({fds)
static constexpr SMC_KEY KeyF0Md = SMC_MAKE_IDENTIFIER('F','0','M','d');  // fan mode: 0 auto, 1 forced
static constexpr SMC_KEY KeyF0Mn = SMC_MAKE_IDENTIFIER('F','0','M','n');  // minimum CPU fan RPM
static constexpr SMC_KEY KeyF0Mx = SMC_MAKE_IDENTIFIER('F','0','M','x');  // maximum CPU fan RPM
static constexpr SMC_KEY KeyF1Ac = SMC_MAKE_IDENTIFIER('F','1','A','c');  // current GPU fan RPM
static constexpr SMC_KEY KeyF1ID = SMC_MAKE_IDENTIFIER('F','1','I','D');  // fan description ({fds)
static constexpr SMC_KEY KeyF1Md = SMC_MAKE_IDENTIFIER('F','1','M','d');  // fan mode: 0 auto, 1 forced
static constexpr SMC_KEY KeyF1Mn = SMC_MAKE_IDENTIFIER('F','1','M','n');  // minimum GPU fan RPM
static constexpr SMC_KEY KeyF1Mx = SMC_MAKE_IDENTIFIER('F','1','M','x');  // maximum GPU fan RPM
static constexpr SMC_KEY KeyFNum = SMC_MAKE_IDENTIFIER('F','N','u','m');  // number of fans (2)
static constexpr SMC_KEY KeyTG0P = SMC_MAKE_IDENTIFIER('T','G','0','P');  // GPU temperature

// F0ID payload ({fds type, 16 bytes), same layout as Apple's and as
// VirtualSMC's SMCDellSensors (FanTypeDescStruct), which is not in the SDK.
struct MSIFanDescription {
    uint8_t type        {0};   // 0 = PWM fan with tachometer
    uint8_t zone        {1};
    uint8_t location    {12};  // LEFT_MID_REAR (nominal: positions are not reported)
    uint8_t reserved    {0};
    char    function[12] {};   // name shown by iStat / HWMonitor
};
static_assert(sizeof(MSIFanDescription) == 16, "{fds is 16 bytes");

// RPM range for ISW formula on A10M: 1480 (val=325) to 6680 (val=1)
static constexpr uint16_t kSMCFanMinRPM = 1480;
static constexpr uint16_t kSMCFanMaxRPM = 6700;

#endif /* SMCMSIFanKeys_h */
