// ---------------------------------------------------------------------------
// MSIECToolboxUserClient.cpp
// ---------------------------------------------------------------------------

#include "MSIECToolboxUserClient.h"
#include "MSIECToolbox.h"

OSDefineMetaClassAndStructors(MSIECToolboxUserClient, IOUserClient)

// ---------------------------------------------------------------------------
// Dispatch table
// ---------------------------------------------------------------------------

IOExternalMethodDispatch MSIECToolboxUserClient::sMethods[kMSISelectorCount] = {
    // kMSISetMuteState — input: MSIMuteState struct, no output
    [kMSISetMuteState] = {
        sActionSetMuteState,
        0,                      // scalar input count
        sizeof(MSIMuteState),   // struct input size
        0,                      // scalar output count
        0                       // struct output size
    },
    // kMSIGetMuteState — no input, output: MSIMuteState struct
    [kMSIGetMuteState] = {
        sActionGetMuteState,
        0,
        0,
        0,
        sizeof(MSIMuteState)
    },
    // kMSIDumpEC — no input, output: MSIECDump (256 bytes)
    [kMSIDumpEC] = {
        sActionDumpEC,
        0,
        0,
        0,
        sizeof(MSIECDump)
    },
    // kMSISetCameraState — input: MSICameraState, no output
    [kMSISetCameraState] = {
        sActionSetCameraState,
        0,
        sizeof(MSICameraState),
        0,
        0
    },
    // kMSIGetAllState — no input, output: MSIAllState (3 EC registers)
    [kMSIGetAllState] = {
        sActionGetAllState,
        0, 0, 0, sizeof(MSIAllState)
    },
    // kMSIReadFanRPM — no input, output: MSIFanState (CPU + GPU RPM)
    [kMSIReadFanRPM] = {
        sActionReadFanRPM,
        0, 0, 0, sizeof(MSIFanState)
    },
    // kMSISetFanMode — input: MSIFanModeState, no output
    [kMSISetFanMode] = {
        sActionSetFanMode,
        0, sizeof(MSIFanModeState), 0, 0
    },
    // kMSISetCoolerBoost — input: MSICoolerBoostState, no output
    [kMSISetCoolerBoost] = {
        sActionSetCoolerBoost,
        0, sizeof(MSICoolerBoostState), 0, 0
    },
    // kMSISetShiftMode — input: MSIShiftModeState, no output
    [kMSISetShiftMode] = {
        sActionSetShiftMode,
        0, sizeof(MSIShiftModeState), 0, 0
    },
    // kMSIGetSystemState — no input, output: MSISystemState
    [kMSIGetSystemState] = {
        sActionGetSystemState,
        0, 0, 0, sizeof(MSISystemState)
    },
    // kMSISetKbBacklight — input: MSIKbBacklightState, no output
    [kMSISetKbBacklight] = {
        sActionSetKbBacklight,
        0, sizeof(MSIKbBacklightState), 0, 0
    },
    // kMSIGetKbBacklight — no input, output: MSIKbBacklightState
    [kMSIGetKbBacklight] = {
        sActionGetKbBacklight,
        0, 0, 0, sizeof(MSIKbBacklightState)
    },
    // kMSISetBatteryCharge — input: MSIBatteryChargeState, no output
    [kMSISetBatteryCharge] = {
        sActionSetBatteryCharge,
        0, sizeof(MSIBatteryChargeState), 0, 0
    },
    // kMSIGetBatteryCharge — no input, output: MSIBatteryChargeState
    [kMSIGetBatteryCharge] = {
        sActionGetBatteryCharge,
        0, 0, 0, sizeof(MSIBatteryChargeState)
    },
    // kMSISetFanCurve — input: MSIFanCurve, no output
    [kMSISetFanCurve] = {
        sActionSetFanCurve,
        0, sizeof(MSIFanCurve), 0, 0
    },
    // kMSIGetFanCurve — no input, output: MSIFanCurve
    [kMSIGetFanCurve] = {
        sActionGetFanCurve,
        0, 0, 0, sizeof(MSIFanCurve)
    },
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool MSIECToolboxUserClient::initWithTask(
    task_t owningTask, void *securityToken, UInt32 type, OSDictionary *props)
{
    if (!IOUserClient::initWithTask(owningTask, securityToken, type, props))
        return false;

    // No privilege check — LED control is accessible to any local user
    // (personal Hackintosh use case).
    MSIEC_LOG("UserClient::initWithTask OK");
    return true;
}

bool MSIECToolboxUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    MSIEC_LOG("UserClient started");
    return true;
}

void MSIECToolboxUserClient::stop(IOService *provider) {
    IOUserClient::stop(provider);
}

IOReturn MSIECToolboxUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// externalMethod — dispatch vers sMethods
//
// The dispatch/target/reference parameters passed by IOKit are ignored
// explicitly — we pass the correct pointers directly to super::externalMethod().
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch * /*dispatch*/,
    OSObject * /*target*/, void * /*reference*/)
{
    if (selector >= kMSISelectorCount) return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector, args,
                                        &sMethods[selector], this, nullptr);
}

// ---------------------------------------------------------------------------
// Static action stubs
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetMuteState(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIMuteState))
        return kIOReturnBadArgument;

    const auto *state = static_cast<const MSIMuteState *>(args->structureInput);
    return MSIECToolbox::setMuteState(state->speakerMuted != 0,
                                    state->micMuted     != 0);
}

IOReturn MSIECToolboxUserClient::sActionGetMuteState(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIMuteState))
        return kIOReturnBadArgument;

    auto *state = static_cast<MSIMuteState *>(args->structureOutput);
    bool spk, mic;
    IOReturn ret = MSIECToolbox::getMuteState(spk, mic);
    if (ret == kIOReturnSuccess) {
        state->speakerMuted = spk ? 1 : 0;
        state->micMuted     = mic ? 1 : 0;
        state->reserved[0]  = 0;
        state->reserved[1]  = 0;
    }
    return ret;
}

IOReturn MSIECToolboxUserClient::sActionDumpEC(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIECDump))
        return kIOReturnBadArgument;

    auto *dump = static_cast<MSIECDump *>(args->structureOutput);
    return MSIECToolbox::dumpEC(dump->data);
}

IOReturn MSIECToolboxUserClient::sActionSetCameraState(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSICameraState))
        return kIOReturnBadArgument;

    const auto *state = static_cast<const MSICameraState *>(args->structureInput);
    return MSIECToolbox::setCameraState(state->cameraOff != 0);
}

IOReturn MSIECToolboxUserClient::sActionGetAllState(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIAllState))
        return kIOReturnBadArgument;

    auto *out = static_cast<MSIAllState *>(args->structureOutput);
    uint8_t micVal = 0, spkVal = 0, camVal = 0;

    // Check each return value — an EC timeout must return an error rather
    // than zeros that would be misinterpreted as "active" state.
    if (MSIECToolbox::fallbackECRead(kMSI_EC_OFFSET_MIC,     micVal) != kIOReturnSuccess)
        return kIOReturnTimeout;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_OFFSET_SPEAKER, spkVal) != kIOReturnSuccess)
        return kIOReturnTimeout;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_OFFSET_CAMERA,  camVal) != kIOReturnSuccess)
        return kIOReturnTimeout;

    out->micMuted     = (micVal & kMSI_EC_BIT_LED) ? 1 : 0;
    out->speakerMuted = (spkVal & kMSI_EC_BIT_LED) ? 1 : 0;
    out->cameraOff    = (camVal == kMSI_EC_CAM_OFF)  ? 1 : 0;
    out->reserved     = 0;
    return kIOReturnSuccess;
}

IOReturn MSIECToolboxUserClient::sActionReadFanRPM(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIFanState))
        return kIOReturnBadArgument;

    auto *out = static_cast<MSIFanState *>(args->structureOutput);

    // Check each return value — zeros from a timeout would be interpreted as
    // valid RPM values by the msiECToRPM formula.
    uint8_t gpuHi = 0, gpuLo = 0, cpuHi = 0, cpuLo = 0;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_FAN_GPU_HI, gpuHi) != kIOReturnSuccess) return kIOReturnTimeout;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_FAN_GPU_LO, gpuLo) != kIOReturnSuccess) return kIOReturnTimeout;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_FAN_CPU_HI, cpuHi) != kIOReturnSuccess) return kIOReturnTimeout;
    if (MSIECToolbox::fallbackECRead(kMSI_EC_FAN_CPU_LO, cpuLo) != kIOReturnSuccess) return kIOReturnTimeout;

    out->gpuRPM = msiECToRPM(gpuHi, gpuLo);
    out->cpuRPM = msiECToRPM(cpuHi, cpuLo);

    MSIEC_LOG("readFanRPM: GPU=%u CPU=%u (raw GPU=0x%02X%02X CPU=0x%02X%02X)",
               out->gpuRPM, out->cpuRPM, gpuHi, gpuLo, cpuHi, cpuLo);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// sActionSetFanMode — sélecteur 6
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetFanMode(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIFanModeState))
        return kIOReturnBadArgument;
    const auto *s = static_cast<const MSIFanModeState *>(args->structureInput);
    return MSIECToolbox::setFanMode(static_cast<MSIFanModeValue>(s->mode));
}

// ---------------------------------------------------------------------------
// sActionSetCoolerBoost — sélecteur 7
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetCoolerBoost(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSICoolerBoostState))
        return kIOReturnBadArgument;
    const auto *s = static_cast<const MSICoolerBoostState *>(args->structureInput);
    return MSIECToolbox::setCoolerBoost(s->enabled != 0);
}

// ---------------------------------------------------------------------------
// sActionSetShiftMode — sélecteur 8
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetShiftMode(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIShiftModeState))
        return kIOReturnBadArgument;
    const auto *s = static_cast<const MSIShiftModeState *>(args->structureInput);
    return MSIECToolbox::setShiftMode(static_cast<MSIShiftModeValue>(s->mode));
}

// ---------------------------------------------------------------------------
// sActionGetSystemState — sélecteur 9
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionGetSystemState(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSISystemState))
        return kIOReturnBadArgument;
    auto *out = static_cast<MSISystemState *>(args->structureOutput);
    return MSIECToolbox::getSystemState(*out);
}

// ---------------------------------------------------------------------------
// sActionSetBatteryCharge — sélecteur 12
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetBatteryCharge(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIBatteryChargeState))
        return kIOReturnBadArgument;
    const auto *s = static_cast<const MSIBatteryChargeState *>(args->structureInput);
    return MSIECToolbox::setBatteryCharge(s->percent);
}

// ---------------------------------------------------------------------------
// sActionGetBatteryCharge — sélecteur 13
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionGetBatteryCharge(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIBatteryChargeState))
        return kIOReturnBadArgument;
    auto *out = static_cast<MSIBatteryChargeState *>(args->structureOutput);
    out->reserved[0] = out->reserved[1] = out->reserved[2] = 0;
    return MSIECToolbox::getBatteryCharge(out->percent);
}

// ---------------------------------------------------------------------------
// sActionSetFanCurve — sélecteur 14
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetFanCurve(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIFanCurve))
        return kIOReturnBadArgument;
    const auto *c = static_cast<const MSIFanCurve *>(args->structureInput);
    return MSIECToolbox::setFanCurve(*c);
}

// ---------------------------------------------------------------------------
// sActionGetFanCurve — sélecteur 15
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionGetFanCurve(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIFanCurve))
        return kIOReturnBadArgument;
    auto *out = static_cast<MSIFanCurve *>(args->structureOutput);
    return MSIECToolbox::getFanCurve(*out);
}

// ---------------------------------------------------------------------------
// sActionSetKbBacklight — sélecteur 10
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionSetKbBacklight(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureInput || args->structureInputSize < sizeof(MSIKbBacklightState))
        return kIOReturnBadArgument;
    const auto *s = static_cast<const MSIKbBacklightState *>(args->structureInput);
    return MSIECToolbox::setKbBacklight(s->level);
}

// ---------------------------------------------------------------------------
// sActionGetKbBacklight — sélecteur 11
// ---------------------------------------------------------------------------

IOReturn MSIECToolboxUserClient::sActionGetKbBacklight(
    OSObject * /*target*/, void *, IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MSIKbBacklightState))
        return kIOReturnBadArgument;
    auto *out = static_cast<MSIKbBacklightState *>(args->structureOutput);
    out->reserved[0] = out->reserved[1] = out->reserved[2] = 0;
    return MSIECToolbox::getKbBacklight(out->level);
}
