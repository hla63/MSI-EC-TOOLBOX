#ifndef MSIECToolboxShared_h
#define MSIECToolboxShared_h

#include <stdint.h>

// ---------------------------------------------------------------------------
// EC port I/O constants (ACPI specification, hardware-level)
// These values are universal across all ACPI-compliant embedded controllers.
// ---------------------------------------------------------------------------
static constexpr uint16_t kECCommandPort = 0x66;  // EC command / status port
static constexpr uint16_t kECDataPort    = 0x62;  // EC data port
static constexpr uint8_t  kECOpRead      = 0x80;  // RD_EC opcode
static constexpr uint8_t  kECOpWrite     = 0x81;  // WR_EC opcode

// ---------------------------------------------------------------------------
// EC register offsets — fan / thermal / performance
// Source: msi-ec Linux driver, config CONF_G1_5 (MSI Modern 15 A10M, 1551EMS1)
// ---------------------------------------------------------------------------

// Fan mode (EC 0xF4, write-only)
static constexpr uint32_t kMSI_EC_FAN_MODE_ADDR    = 0xF4;
static constexpr uint8_t  kMSI_EC_FAN_AUTO         = 0x0D;  // EC-controlled curve
static constexpr uint8_t  kMSI_EC_FAN_SILENT       = 0x1D;  // minimal fan speed
static constexpr uint8_t  kMSI_EC_FAN_ADVANCED     = 0x8D;  // custom curve active

// Cooler Boost (EC 0x98, bit 7)
// Forces all fans to 100%. Use read-modify-write to preserve bits 0-6.
static constexpr uint32_t kMSI_EC_COOLER_BOOST_ADDR = 0x98;
static constexpr uint8_t  kMSI_EC_COOLER_BOOST_MASK = 0x80;  // bit 7

// ---------------------------------------------------------------------------
// CPU fan curve breakpoints (confirmed by EC dump on A10M 1551EMS1)
// Active only when fan mode is kMSI_EC_FAN_ADVANCED.
// ---------------------------------------------------------------------------

// Temperature thresholds (6 points, direct °C value)
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP0 = 0x6A;  // default: 50°C
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP1 = 0x6B;  // default: 58°C
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP2 = 0x6C;  // default: 65°C
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP3 = 0x6D;  // default: 70°C
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP4 = 0x6E;  // default: 90°C
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP5 = 0x6F;  // default: 95°C

// Fan speed targets (6 points, direct % value, range 0-100)
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD0  = 0x72;  // default:  0%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD1  = 0x73;  // default: 58%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD2  = 0x74;  // default: 65%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD3  = 0x75;  // default: 72%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD4  = 0x76;  // default: 80%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD5  = 0x77;  // default: 85%
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD6  = 0x78;  // fixed: 100% (read-only)

// Firmware default values — reference for curve reset
static constexpr uint8_t kMSI_FAN_DEFAULT_TEMPS[6] = { 0x32, 0x3A, 0x41, 0x46, 0x5A, 0x5F };
static constexpr uint8_t kMSI_FAN_DEFAULT_SPDS[6]  = { 0x00, 0x3A, 0x41, 0x48, 0x50, 0x55 };

// Base addresses and point count — used for loop-based writes
static constexpr uint32_t kMSI_EC_FAN_CPU_TEMP_BASE = 0x6A;  // 6 contiguous registers
static constexpr uint32_t kMSI_EC_FAN_CPU_SPD_BASE  = 0x72;  // 6 registers + 0x78 fixed
static constexpr uint8_t  kMSI_EC_FAN_CURVE_POINTS  = 6;     // editable breakpoints

// ---------------------------------------------------------------------------
// GPU fan curve breakpoints (present in EC on A10M but effect unconfirmed)
// The A10M has a single physical fan (iGPU only). EC register 0x89 returned
// 0x2D (45%) in dumps while 0x71 (CPU fan %) was 0 — likely a firmware
// residual or mirror value. Do not write these registers without validation.
// ---------------------------------------------------------------------------

static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP0 = 0x82;  // default: 50°C
static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP1 = 0x83;  // default: 60°C
static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP2 = 0x84;  // default: 70°C
static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP3 = 0x85;  // default: 82°C
static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP4 = 0x86;  // default: 90°C
static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP5 = 0x87;  // default: 93°C

static constexpr uint32_t kMSI_EC_FAN_GPU_SPD0  = 0x8A;  // default: 45%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD1  = 0x8B;  // default: 50%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD2  = 0x8C;  // default: 65%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD3  = 0x8D;  // default: 72%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD4  = 0x8E;  // default: 80%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD5  = 0x8F;  // default: 85%
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD6  = 0x90;  // fixed: 100%

static constexpr uint8_t kMSI_FAN_DEFAULT_GPU_TEMPS[6] = { 0x32, 0x3C, 0x46, 0x52, 0x5A, 0x5D };
static constexpr uint8_t kMSI_FAN_DEFAULT_GPU_SPDS[6]  = { 0x2D, 0x32, 0x41, 0x48, 0x50, 0x55 };

static constexpr uint32_t kMSI_EC_FAN_GPU_TEMP_BASE = 0x82;
static constexpr uint32_t kMSI_EC_FAN_GPU_SPD_BASE  = 0x8A;

// ---------------------------------------------------------------------------
// Battery charge stop threshold (EC 0xEF, confirmed by dump on 1551EMS1)
//
// Known encodings:
//   0x64 (100) = full charge (firmware default)
//   0x50  (80) = 80% stop
//   0xBC (188) = 60% stop ("Super Battery" mode in MSI Center)
//
// WARNING: the encoding is non-linear. Confirm each new value with an EC
// dump before exposing it in the UI.
// ---------------------------------------------------------------------------
static constexpr uint32_t kMSI_EC_BATTERY_CHARGE_ADDR  = 0xEF;
static constexpr uint8_t  kMSI_EC_BATTERY_CHARGE_100   = 0x64;
static constexpr uint8_t  kMSI_EC_BATTERY_CHARGE_80    = 0x50;
static constexpr uint8_t  kMSI_EC_BATTERY_CHARGE_60    = 0xBC;  // confirmed by dump

// ---------------------------------------------------------------------------
// Keyboard backlight (EC 0xF3, confirmed by msi-ec Linux driver CONF_G1_5)
//
// Simple write — no read-modify-write needed.
// Survives sleep but is reset to off by firmware at boot.
// ---------------------------------------------------------------------------
static constexpr uint32_t kMSI_EC_KB_BACKLIGHT_ADDR  = 0xF3;
static constexpr uint8_t  kMSI_EC_KB_BACKLIGHT_OFF   = 0x80;
static constexpr uint8_t  kMSI_EC_KB_BACKLIGHT_LOW   = 0x81;
static constexpr uint8_t  kMSI_EC_KB_BACKLIGHT_MED   = 0x82;
static constexpr uint8_t  kMSI_EC_KB_BACKLIGHT_HIGH  = 0x83;
static constexpr uint8_t  kMSI_EC_KB_BACKLIGHT_MAX   = 0x83;

// ---------------------------------------------------------------------------
// Shift mode / performance profile (EC 0xF2)
// Confirmed across 4 EC captures (balanced/performance/silent/super_battery).
// ---------------------------------------------------------------------------
static constexpr uint32_t kMSI_EC_SHIFT_MODE_ADDR   = 0xF2;
static constexpr uint8_t  kMSI_EC_SHIFT_ECO         = 0xC2;  // minimum power
static constexpr uint8_t  kMSI_EC_SHIFT_COMFORT     = 0xC1;  // balanced (default)
static constexpr uint8_t  kMSI_EC_SHIFT_TURBO       = 0xC0;  // maximum performance

// Real-time temperature (read-only)
static constexpr uint32_t kMSI_EC_CPU_TEMP_ADDR     = 0x68;  // direct °C
static constexpr uint32_t kMSI_EC_GPU_TEMP_ADDR     = 0x80;  // direct °C

// Real-time fan speed % (read-only, range 0-150)
static constexpr uint32_t kMSI_EC_CPU_FAN_PCT_ADDR  = 0x71;
// NOTE: on A10M (iGPU only), 0x89 returned 0x2D (45%) in dumps while 0x71
// was 0 (fan stopped). Likely a firmware residual — treat as unreliable.
static constexpr uint32_t kMSI_EC_GPU_FAN_PCT_ADDR  = 0x89;

// Fan mode logical values (independent of raw EC bytes)
enum MSIFanModeValue : uint8_t {
    kMSIFanModeAuto     = 0,
    kMSIFanModeSilent   = 1,
    kMSIFanModeAdvanced = 2,
};

// Shift mode logical values
enum MSIShiftModeValue : uint8_t {
    kMSIShiftEco     = 0,
    kMSIShiftComfort = 1,
    kMSIShiftTurbo   = 2,
};

// ---------------------------------------------------------------------------
// LED / camera EC register offsets
// ---------------------------------------------------------------------------
static constexpr uint32_t kMSI_EC_OFFSET_MIC     = 0x2B;
static constexpr uint32_t kMSI_EC_OFFSET_SPEAKER = 0x2C;
static constexpr uint32_t kMSI_EC_OFFSET_CAMERA  = 0x2E;
static constexpr uint32_t kMSI_EC_FAN_GPU_HI     = 0xCA;
static constexpr uint32_t kMSI_EC_FAN_GPU_LO     = 0xCB;
static constexpr uint32_t kMSI_EC_FAN_CPU_HI     = 0xCC;
static constexpr uint32_t kMSI_EC_FAN_CPU_LO     = 0xCD;
static constexpr uint8_t  kMSI_EC_BASE_MIC       = 0x80;
static constexpr uint8_t  kMSI_EC_BASE_SPEAKER   = 0xE0;
static constexpr uint8_t  kMSI_EC_CAM_ACTIVE     = 0x4B;
static constexpr uint8_t  kMSI_EC_CAM_OFF        = 0x49;
static constexpr uint8_t  kMSI_EC_BIT_LED        = 0x04;  // LED mute indicator bit

// ---------------------------------------------------------------------------
// UserClient selectors — must match the dispatch table in UserClient.cpp
// ---------------------------------------------------------------------------
enum MSIECToolboxSelector : uint32_t {
    kMSISetMuteState     = 0,
    kMSIGetMuteState     = 1,   // Reserved for CLI — superseded by kMSIGetAllState (4)
    kMSIDumpEC           = 2,
    kMSISetCameraState   = 3,
    kMSIGetAllState      = 4,
    kMSIReadFanRPM       = 5,
    kMSISetFanMode       = 6,
    kMSISetCoolerBoost   = 7,
    kMSISetShiftMode     = 8,
    kMSIGetSystemState   = 9,
    kMSISetKbBacklight   = 10,
    kMSIGetKbBacklight   = 11,
    kMSISetBatteryCharge = 12,
    kMSIGetBatteryCharge = 13,
    kMSISetFanCurve      = 14,
    kMSIGetFanCurve      = 15,
    kMSISelectorCount
};

// ---------------------------------------------------------------------------
// Structs exchanged via IOConnectCallStructMethod
// All structs are packed to avoid alignment padding between kernel and user space.
// ---------------------------------------------------------------------------

struct MSIMuteState {
    uint8_t speakerMuted;
    uint8_t micMuted;
    uint8_t reserved[2];
} __attribute__((packed));

struct MSICameraState {
    uint8_t cameraOff;
    uint8_t reserved[3];
} __attribute__((packed));

struct MSIAllState {
    uint8_t micMuted;
    uint8_t speakerMuted;
    uint8_t cameraOff;
    uint8_t reserved;
} __attribute__((packed));

struct MSIFanState {
    uint16_t gpuRPM;  // 0 = fan stopped
    uint16_t cpuRPM;
} __attribute__((packed));

struct MSIECDump {
    uint8_t data[256];
} __attribute__((packed));

struct MSIFanModeState {
    uint8_t mode;       // MSIFanModeValue
    uint8_t reserved[3];
} __attribute__((packed));

struct MSICoolerBoostState {
    uint8_t enabled;    // 0=OFF, 1=ON
    uint8_t reserved[3];
} __attribute__((packed));

struct MSIShiftModeState {
    uint8_t mode;       // MSIShiftModeValue
    uint8_t reserved[3];
} __attribute__((packed));

struct MSIKbBacklightState {
    uint8_t level;      // 0=off 1=low 2=medium 3=high
    uint8_t reserved[3];
} __attribute__((packed));

// Selector 9 — aggregated system state (read-only)
struct MSISystemState {
    uint8_t cpuTempC;     // °C, 0 on EC error
    uint8_t gpuTempC;     // °C, 0 on EC error (iGPU only on A10M)
    uint8_t cpuFanPct;    // %, range 0-150
    uint8_t gpuFanPct;    // %, always 0 on A10M (no dedicated GPU fan)
    uint8_t fanMode;      // MSIFanModeValue
    uint8_t shiftMode;    // MSIShiftModeValue
    uint8_t coolerBoost;  // 0 or 1
    uint8_t reserved;
} __attribute__((packed));

struct MSIBatteryChargeState {
    uint8_t percent;    // 80 or 100
    uint8_t reserved[3];
} __attribute__((packed));

// Selector 14/15 — CPU fan curve (advanced mode only)
// 6 editable breakpoints; point 7 (100% at ~95°C) is fixed in hardware.
// Constraints validated by EC dump on A10M 1551EMS1:
//   - temp[i] < temp[i+1]   (strictly increasing)
//   - temp[i] in [20, 95] °C
//   - speed[i] <= speed[i+1] (non-decreasing)
//   - speed[i] in [0, 100] %  (0 = fan off, valid for point 0)
struct MSIFanCurve {
    uint8_t temps[6];   // °C, indices 0-5
    uint8_t speeds[6];  // %, indices 0-5
} __attribute__((packed));

// ---------------------------------------------------------------------------
// ISW RPM formula
// val = big-endian 16-bit register (EC 0xCC-0xCD for CPU fan)
// RPM = ((325 - val) * 16) + 1480  when val != 0, else 0 (fan stopped)
// ---------------------------------------------------------------------------
static inline uint16_t msiECToRPM(uint8_t hi, uint8_t lo) {
    uint16_t val = ((uint16_t)hi << 8) | lo;
    if (val == 0) return 0;
    if (val > 325) val = 325;
    return (uint16_t)(((325 - val) * 16) + 1480);
}

#endif /* MSIECToolboxShared_h */
