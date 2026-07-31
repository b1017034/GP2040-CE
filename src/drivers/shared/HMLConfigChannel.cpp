#include "drivers/shared/HMLConfigChannel.h"

#include <string.h>
#include <stddef.h>

#include "tusb.h"
#include "storagemanager.h"
#include "eventmanager.h"
#include "events/GPStorageSaveEvent.h"
#include "events/GPRestartEvent.h"
#include "addons/hml_analog.h"
#include "addons/gamepad_usb_host_listener.h"
#include "peripheralmanager.h"
#include "config.pb.h"

namespace {

enum ParamKind : uint8_t { PK_U32, PK_I32, PK_BOOL };

struct ParamDef {
    uint8_t id;         // proto field number
    ParamKind kind;
    uint16_t offset;    // offsetof(struct, field); repeated: offset of element 0
    uint16_t auxOffset; // scalar: offsetof(has_flag); repeated: offsetof(field_count)
    uint8_t maxCount;   // 0 = scalar, else repeated capacity
};

#define P_SCALAR(st, id, field, kind) \
    { id, kind, (uint16_t)offsetof(st, field), (uint16_t)offsetof(st, has_##field), 0 }
#define P_ARRAY(st, id, field, kind, maxc) \
    { id, kind, (uint16_t)offsetof(st, field), (uint16_t)offsetof(st, field##_count), maxc }

static const ParamDef stickParams[] = {
    P_SCALAR(HMLStickOptions,  1, calMinX, PK_U32),
    P_SCALAR(HMLStickOptions,  2, calCenterX, PK_U32),
    P_SCALAR(HMLStickOptions,  3, calMaxX, PK_U32),
    P_SCALAR(HMLStickOptions,  4, calMinY, PK_U32),
    P_SCALAR(HMLStickOptions,  5, calCenterY, PK_U32),
    P_SCALAR(HMLStickOptions,  6, calMaxY, PK_U32),
    P_SCALAR(HMLStickOptions,  7, innerDeadzone, PK_U32),
    P_SCALAR(HMLStickOptions,  8, outerDeadzone, PK_U32),
    P_SCALAR(HMLStickOptions,  9, curvePreset, PK_U32),
    P_SCALAR(HMLStickOptions, 10, curvePointCount, PK_U32),
    P_ARRAY (HMLStickOptions, 11, curveX, PK_U32, 10),
    P_ARRAY (HMLStickOptions, 12, curveY, PK_U32, 10),
    P_SCALAR(HMLStickOptions, 13, rcFilterStrength, PK_I32),
    P_SCALAR(HMLStickOptions, 14, rcSnapback, PK_U32),
    P_SCALAR(HMLStickOptions, 15, rcAdvanced, PK_BOOL),
    P_SCALAR(HMLStickOptions, 16, rcCurvePointCount, PK_U32),
    P_ARRAY (HMLStickOptions, 17, rcCurveX, PK_U32, 7),
    P_ARRAY (HMLStickOptions, 18, rcCurveY, PK_I32, 7),
    P_SCALAR(HMLStickOptions, 19, rcVelocityMult100, PK_U32),
    P_SCALAR(HMLStickOptions, 20, invertX, PK_BOOL),
    P_SCALAR(HMLStickOptions, 21, invertY, PK_BOOL),
    P_SCALAR(HMLStickOptions, 22, diagScaleX, PK_U32),
    P_SCALAR(HMLStickOptions, 23, diagScaleY, PK_U32),
    P_SCALAR(HMLStickOptions, 24, noiseSuppress, PK_BOOL),
    P_SCALAR(HMLStickOptions, 25, noiseX, PK_U32),
    P_SCALAR(HMLStickOptions, 26, noiseY, PK_U32),
    P_SCALAR(HMLStickOptions, 27, scaleUp, PK_U32),
    P_SCALAR(HMLStickOptions, 28, scaleDown, PK_U32),
    P_SCALAR(HMLStickOptions, 29, scaleLeft, PK_U32),
    P_SCALAR(HMLStickOptions, 30, scaleRight, PK_U32),
    P_SCALAR(HMLStickOptions, 31, innerX, PK_U32),
    P_SCALAR(HMLStickOptions, 32, innerY, PK_U32),
    P_SCALAR(HMLStickOptions, 33, outerX, PK_U32),
    P_SCALAR(HMLStickOptions, 34, outerY, PK_U32),
    P_SCALAR(HMLStickOptions, 35, antiX, PK_U32),
    P_SCALAR(HMLStickOptions, 36, antiY, PK_U32),
    P_SCALAR(HMLStickOptions, 37, resolution, PK_U32),
    P_SCALAR(HMLStickOptions, 38, delayMs, PK_U32),
    P_SCALAR(HMLStickOptions, 39, angleOffset100, PK_I32),
    P_SCALAR(HMLStickOptions, 40, angleSnap, PK_U32),
    P_SCALAR(HMLStickOptions, 41, angleMapCount, PK_U32),
    P_ARRAY (HMLStickOptions, 42, angleMapIn100, PK_I32, 16),
    P_ARRAY (HMLStickOptions, 43, angleMapOut100, PK_I32, 16),
    P_ARRAY (HMLStickOptions, 44, angleMapDz100, PK_U32, 16),
    P_SCALAR(HMLStickOptions, 45, angleMapEnable, PK_U32),
};

static const ParamDef globalParams[] = {
    P_SCALAR(HMLAnalogOptions,  1, enabled, PK_BOOL),
    P_SCALAR(HMLAnalogOptions,  2, chLX, PK_I32),
    P_SCALAR(HMLAnalogOptions,  3, chLY, PK_I32),
    P_SCALAR(HMLAnalogOptions,  4, chRX, PK_I32),
    P_SCALAR(HMLAnalogOptions,  5, chRY, PK_I32),
    P_SCALAR(HMLAnalogOptions,  6, chPsButton, PK_I32),
    P_SCALAR(HMLAnalogOptions,  7, psThreshold, PK_U32),
    P_SCALAR(HMLAnalogOptions,  8, psActiveHigh, PK_BOOL),
    P_SCALAR(HMLAnalogOptions, 16, l3Threshold, PK_U32),
    P_SCALAR(HMLAnalogOptions, 17, l3ActiveHigh, PK_BOOL),
    P_SCALAR(HMLAnalogOptions, 19, r3Threshold, PK_U32),
    P_SCALAR(HMLAnalogOptions, 20, r3ActiveHigh, PK_BOOL),
    P_SCALAR(HMLAnalogOptions, 21, lightbarEnabled, PK_BOOL),
    P_SCALAR(HMLAnalogOptions, 23, lightbarBrightness, PK_U32),
    P_SCALAR(HMLAnalogOptions, 24, lightbarColor, PK_U32),
    P_SCALAR(HMLAnalogOptions, 25, comboModeB1, PK_I32),
    P_SCALAR(HMLAnalogOptions, 26, comboModeB2, PK_I32),
    P_SCALAR(HMLAnalogOptions, 27, comboModeB3, PK_I32),
    P_SCALAR(HMLAnalogOptions, 28, comboModeB4, PK_I32),
};

// target 0/1/2 -> (base pointer, table, table size); returns false if invalid
static bool resolveTarget(uint8_t target, uint8_t *&base, const ParamDef *&table, size_t &tableLen) {
    HMLAnalogOptions &opts = Storage::getInstance().getAddonOptions().hmlAnalogOptions;
    switch (target) {
    case 0:
        opts.has_leftStick = true;
        base = (uint8_t *)&opts.leftStick;
        table = stickParams; tableLen = sizeof(stickParams) / sizeof(stickParams[0]);
        return true;
    case 1:
        opts.has_rightStick = true;
        base = (uint8_t *)&opts.rightStick;
        table = stickParams; tableLen = sizeof(stickParams) / sizeof(stickParams[0]);
        return true;
    case 2:
        base = (uint8_t *)&opts;
        table = globalParams; tableLen = sizeof(globalParams) / sizeof(globalParams[0]);
        return true;
    default:
        return false;
    }
}

static const ParamDef *findParam(const ParamDef *table, size_t tableLen, uint8_t id) {
    for (size_t i = 0; i < tableLen; i++)
        if (table[i].id == id)
            return &table[i];
    return nullptr;
}

static bool readParam(const uint8_t *base, const ParamDef &def, uint8_t index, int32_t &out) {
    if (def.maxCount == 0) {
        if (index != 0) return false;
    } else if (index >= def.maxCount) {
        return false;
    }
    const uint8_t *p = base + def.offset;
    switch (def.kind) {
    case PK_U32: out = (int32_t)((const uint32_t *)p)[index]; return true;
    case PK_I32: out = ((const int32_t *)p)[index]; return true;
    case PK_BOOL: out = ((const bool *)p)[index] ? 1 : 0; return true;
    }
    return false;
}

static bool writeParam(uint8_t *base, const ParamDef &def, uint8_t index, int32_t value) {
    if (def.maxCount == 0) {
        if (index != 0) return false;
    } else if (index >= def.maxCount) {
        return false;
    }
    uint8_t *p = base + def.offset;
    switch (def.kind) {
    case PK_U32: ((uint32_t *)p)[index] = (uint32_t)value; break;
    case PK_I32: ((int32_t *)p)[index] = value; break;
    case PK_BOOL: ((bool *)p)[index] = (value != 0); break;
    }
    if (def.maxCount == 0) {
        *(bool *)(base + def.auxOffset) = true; // has_ flag
    } else {
        // grow the nanopb element count to cover the written index
        pb_size_t *count = (pb_size_t *)(base + def.auxOffset);
        if (*count <= index) *count = index + 1;
    }
    return true;
}

// Pending GET selection, set by SELECT and consumed (repeatedly) by handleGet
struct Selection {
    uint8_t page = 0x00; // INFO
    uint8_t target = 0;
    uint8_t paramId = 0;
    uint8_t index = 0;
};
static Selection selection;

static void put32(uint8_t *p, int32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static int32_t get32(const uint8_t *p) {
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

constexpr uint8_t RESP_LEN = 63;
constexpr uint8_t DUMP_PER_PAGE = 9; // 9 x (id, idx, i32) = 54 bytes + header

} // namespace

namespace HMLConfigChannel {

const uint8_t hml_config_report_descriptor[] = { HML_CONFIG_REPORT_DESC_BLOCK };
const uint16_t hml_config_report_descriptor_len = sizeof(hml_config_report_descriptor);

static uint8_t patchedDesc[512];
static bool itfEnabled = false;
static uint8_t hidInstance = 0;

void disableInterface() { itfEnabled = false; }
bool isInterfaceEnabled() { return itfEnabled; }
uint8_t getHidInstance() { return hidInstance; }

const uint8_t *patchConfigDescriptor(const uint8_t *desc) {
    itfEnabled = false;
    if (desc == nullptr)
        return desc;

    constexpr uint16_t ADD_LEN = 9 + 9 + 7; // interface + HID + endpoint
    uint16_t totalLength = (uint16_t)(desc[2] | (desc[3] << 8));
    if (totalLength < 9 || totalLength + ADD_LEN > (uint16_t)sizeof(patchedDesc))
        return desc;

    // Scan for IN endpoints in use and existing HID-class interfaces: the new
    // interface needs a free interrupt-IN endpoint, and its tinyusb HID
    // instance index equals the number of HID interfaces claimed before it.
    uint16_t pos = 0;
    uint16_t inEpMask = 0;
    uint8_t hidCount = 0;
    while (pos + 2 <= totalLength) {
        uint8_t len = desc[pos];
        uint8_t type = desc[pos + 1];
        if (len < 2 || pos + len > totalLength)
            return desc; // malformed, leave untouched
        if (type == TUSB_DESC_INTERFACE && len >= 9) {
            if (desc[pos + 3] == 0 && desc[pos + 5] == TUSB_CLASS_HID)
                hidCount++;
        } else if (type == TUSB_DESC_ENDPOINT && len >= 7) {
            uint8_t epAddr = desc[pos + 2];
            if (epAddr & TUSB_DIR_IN_MASK)
                inEpMask |= (1u << (epAddr & 0x0F));
        }
        pos += len;
    }
    int epNum = -1;
    for (int i = 1; i < 16; i++) {
        if (!(inEpMask & (1u << i))) { epNum = i; break; }
    }
    if (epNum < 0)
        return desc;

    memcpy(patchedDesc, desc, totalLength);
    uint8_t itfNum = patchedDesc[4]; // bNumInterfaces == next free number
    uint8_t *p = patchedDesc + totalLength;
    // Interface descriptor: HID class, no subclass/protocol
    p[0] = 9; p[1] = TUSB_DESC_INTERFACE; p[2] = itfNum; p[3] = 0;
    p[4] = 1; p[5] = TUSB_CLASS_HID; p[6] = 0; p[7] = 0; p[8] = 0;
    // HID descriptor
    p[9] = 9; p[10] = 0x21; p[11] = 0x11; p[12] = 0x01; p[13] = 0; p[14] = 1;
    p[15] = 0x22;
    p[16] = (uint8_t)(hml_config_report_descriptor_len & 0xFF);
    p[17] = (uint8_t)(hml_config_report_descriptor_len >> 8);
    // Interrupt IN endpoint (required by the HID class; never actually used —
    // all traffic runs over EP0 feature reports)
    p[18] = 7; p[19] = TUSB_DESC_ENDPOINT; p[20] = (uint8_t)(0x80 | epNum);
    p[21] = TUSB_XFER_INTERRUPT; p[22] = 8; p[23] = 0; p[24] = 10;

    totalLength += ADD_LEN;
    patchedDesc[2] = (uint8_t)(totalLength & 0xFF);
    patchedDesc[3] = (uint8_t)(totalLength >> 8);
    patchedDesc[4] = itfNum + 1;

    hidInstance = hidCount;
    itfEnabled = true;
    return patchedDesc;
}

void handleSet(const uint8_t *buffer, uint16_t bufsize) {
    if (bufsize < 1) return;

    switch (buffer[0]) {
    case 0x01: { // SET_PARAM
        if (bufsize < 8) return;
        uint8_t *base; const ParamDef *table; size_t tableLen;
        if (!resolveTarget(buffer[1], base, table, tableLen)) return;
        const ParamDef *def = findParam(table, tableLen, buffer[2]);
        if (def == nullptr) return;
        writeParam(base, *def, buffer[3], get32(buffer + 4));
        break;
    }
    case 0x02: // SAVE (deferred to the main loop via the storage-save event)
        EventManager::getInstance().triggerEvent(new GPStorageSaveEvent(true));
        break;
    case 0x03: // SELECT
        if (bufsize < 2) return;
        selection.page = buffer[1];
        selection.target = (bufsize > 2) ? buffer[2] : 0;
        selection.paramId = (bufsize > 3) ? buffer[3] : 0;
        selection.index = (bufsize > 4) ? buffer[4] : 0;
        break;
    case 0x04: { // REBOOT (deferred to the main loop via the restart event)
        if (bufsize < 2) return;
        System::BootMode mode = System::BootMode::DEFAULT;
        if (buffer[1] == 1) mode = System::BootMode::USB;       // BOOTSEL / UF2
        else if (buffer[1] == 2) mode = System::BootMode::WEBCONFIG;
        EventManager::getInstance().triggerEvent(new GPRestartEvent(mode));
        break;
    }
    case 0x05: { // SET_INPUT_MODE: persist and reboot into the new driver
        if (bufsize < 2) return;
        if (buffer[1] > INPUT_MODE_P5GENERAL) return;
        GamepadOptions &gamepadOptions = Storage::getInstance().getGamepadOptions();
        gamepadOptions.inputMode = (InputMode)buffer[1];
        EventManager::getInstance().triggerEvent(new GPStorageSaveEvent(true, true));
        break;
    }
    case 0x06: // CAL_START: begin a gate sweep for one stick
        if (bufsize < 2) return;
        HMLAnalogAddon::calStart(buffer[1]);
        break;
    case 0x07: // CAL_SAVE: store the sweep against the mounted controller
        // Key the profile on the controller in use, matching the lookup in
        // HMLAnalogAddon::gateFor. Saving against a rejected mount would store
        // the sweep where nothing ever looks for it.
        HMLAnalogAddon::calSave(GamepadUSBHostListener::active.vid,
                                GamepadUSBHostListener::active.pid);
        break;
    case 0x08: // CAL_ABORT
        HMLAnalogAddon::calAbort();
        break;
    default:
        break;
    }
}

uint16_t handleGet(uint8_t *buffer, uint16_t reqlen) {
    if (reqlen < RESP_LEN) return 0;
    memset(buffer, 0, RESP_LEN);
    buffer[0] = 0xFF; // invalid marker unless a branch below succeeds

    switch (selection.page) {
    case 0x00: { // INFO
        buffer[0] = 0x00;
        buffer[1] = 'H'; buffer[2] = 'M'; buffer[3] = 'L'; buffer[4] = 'C';
        buffer[5] = PROTOCOL_VERSION;
        buffer[6] = (uint8_t)(sizeof(stickParams) / sizeof(stickParams[0]));
        buffer[7] = (uint8_t)(sizeof(globalParams) / sizeof(globalParams[0]));
        buffer[8] = DUMP_PER_PAGE;
        buffer[9] = (uint8_t)Storage::getInstance().getGamepadOptions().inputMode;
        // Board kind: without it the tool cannot tell a converter (whose sticks
        // arrive from a controller on the USB host) from the pad (whose sticks
        // are the on-board ADC), and the two need different screens.
        buffer[10] = PeripheralManager::getInstance().isSPIEnabled(0)
                         ? HML_BOARD_PAD : HML_BOARD_CONVERTER;
        break;
    }
    case 0x01: { // RAW: live ADC channels + processed stick output
        buffer[0] = 0x01;
        buffer[1] = HMLAnalogAddon::rawValid ? 1 : 0;
        for (int i = 0; i < ADS8332_CHANNEL_COUNT && i < 8; i++)
            put16(buffer + 2 + i * 2, HMLAnalogAddon::rawChannels[i]);
        put16(buffer + 18, HMLAnalogAddon::rawAdc[0]);
        put16(buffer + 20, HMLAnalogAddon::rawAdc[1]);
        Gamepad *gamepad = Storage::getInstance().GetProcessedGamepad();
        if (gamepad != nullptr) {
            put16(buffer + 22, gamepad->state.lx);
            put16(buffer + 24, gamepad->state.ly);
            put16(buffer + 26, gamepad->state.rx);
            put16(buffer + 28, gamepad->state.ry);
            buffer[30] = gamepad->state.lt;
            buffer[31] = gamepad->state.rt;
        }
        // ADC link diagnostics (warm-reboot debugging)
        if (HMLAnalogAddon::instance != nullptr && HMLAnalogAddon::instance->getAds() != nullptr) {
            ADS8332 *ads = HMLAnalogAddon::instance->getAds();
            buffer[32] = 1; // diag present
            buffer[33] = ads->linkOk ? 1 : 0;
            buffer[34] = ads->scanCompleted ? 1 : 0;
            buffer[35] = (uint8_t)ads->probedCs;
            buffer[36] = ads->probedMode;
            buffer[37] = ads->hwCs ? 1 : 0;
            buffer[38] = ads->stitch ? 1 : 0;
        }
        break;
    }
    case 0x04: { // HOST: controller on the USB host + gate-calibration state
        buffer[0] = 0x04;
        // Report the controller in use, falling back to the last attempt so a
        // rejected device still explains itself
        const auto &a = GamepadUSBHostListener::active;
        const auto &m = a.seen ? a : GamepadUSBHostListener::lastMount;
        // Layout is packed to fit RESP_LEN: the sweep alone is 32 bytes, so the
        // driver travels as a code rather than a name and the last radius must
        // land at 54. Keep the tail in sight when adding fields here.
        buffer[1] = m.seen ? 1 : 0;
        buffer[2] = m.hid ? 1 : 0;
        put16(buffer + 3, m.vid);
        put16(buffer + 5, m.pid);
        buffer[7] = m.driver;
        buffer[8] = HMLAnalogAddon::calActive() ? 1 : 0;
        buffer[9] = HMLAnalogAddon::calStick;
        buffer[10] = HMLAnalogAddon::calCoverage();
        put16(buffer + 11, HMLAnalogAddon::calCenter[0]);
        put16(buffer + 13, HMLAnalogAddon::calCenter[1]);
        // Untuned sticks straight off the controller, for the gate plot
        Gamepad *live = Storage::getInstance().GetGamepad();
        if (live != nullptr) {
            put16(buffer + 15, live->state.lx);
            put16(buffer + 17, live->state.ly);
            put16(buffer + 19, live->state.rx);
            put16(buffer + 21, live->state.ry);
        }
        // Sweep so far, 16 directions from up, clockwise, in 1/1000 full scale
        static_assert(23 + HML_GATE_BINS * 2 <= 63, "HOST response overruns the report");
        for (int i = 0; i < HML_GATE_BINS; i++)
            put16(buffer + 23 + i * 2, HMLAnalogAddon::calRadius[i]);
        break;
    }
    case 0x02: { // PARAM: single value
        uint8_t *base; const ParamDef *table; size_t tableLen;
        if (!resolveTarget(selection.target, base, table, tableLen)) break;
        const ParamDef *def = findParam(table, tableLen, selection.paramId);
        int32_t value;
        if (def == nullptr || !readParam(base, *def, selection.index, value)) break;
        buffer[0] = 0x02;
        buffer[1] = selection.target;
        buffer[2] = selection.paramId;
        buffer[3] = selection.index;
        put32(buffer + 4, value);
        break;
    }
    case 0x03: { // DUMP: page of (id, idx, value) tuples over the expanded table
        uint8_t *base; const ParamDef *table; size_t tableLen;
        if (!resolveTarget(selection.target, base, table, tableLen)) break;
        buffer[0] = 0x03;
        buffer[1] = selection.target;
        buffer[2] = selection.paramId; // page number (reuses the SELECT byte)
        uint32_t skip = (uint32_t)selection.paramId * DUMP_PER_PAGE;
        uint8_t count = 0;
        uint8_t *out = buffer + 4;
        for (size_t i = 0; i < tableLen && count < DUMP_PER_PAGE; i++) {
            uint8_t slots = (table[i].maxCount == 0) ? 1 : table[i].maxCount;
            for (uint8_t idx = 0; idx < slots && count < DUMP_PER_PAGE; idx++) {
                if (skip > 0) { skip--; continue; }
                int32_t value;
                if (!readParam(base, table[i], idx, value)) continue;
                out[0] = table[i].id;
                out[1] = idx;
                put32(out + 2, value);
                out += 6;
                count++;
            }
        }
        buffer[3] = count;
        break;
    }
    default:
        break;
    }
    return RESP_LEN;
}

} // namespace HMLConfigChannel
