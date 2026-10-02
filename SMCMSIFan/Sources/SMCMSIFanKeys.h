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
// strictly ascending order: F0Ac < F0Mn < F0Mx < FNum < TG0P
// TC0P (CPU package temperature) is not published: SMCProcessor already
// provides it from the CPU's own sensors, and two providers of one key make
// the value read by macOS ambiguous.
// ---------------------------------------------------------------------------
static constexpr SMC_KEY KeyF0Ac = SMC_MAKE_IDENTIFIER('F','0','A','c');  // current CPU fan RPM
static constexpr SMC_KEY KeyF0Mn = SMC_MAKE_IDENTIFIER('F','0','M','n');  // minimum CPU fan RPM
static constexpr SMC_KEY KeyF0Mx = SMC_MAKE_IDENTIFIER('F','0','M','x');  // maximum CPU fan RPM
static constexpr SMC_KEY KeyFNum = SMC_MAKE_IDENTIFIER('F','N','u','m');  // number of fans
static constexpr SMC_KEY KeyTG0P = SMC_MAKE_IDENTIFIER('T','G','0','P');  // GPU temperature

// RPM range for ISW formula on A10M: 1480 (val=325) to 6680 (val=1)
static constexpr uint16_t kSMCFanMinRPM = 1480;
static constexpr uint16_t kSMCFanMaxRPM = 6700;

#endif /* SMCMSIFanKeys_h */
