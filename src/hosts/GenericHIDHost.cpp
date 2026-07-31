#include "hosts/GenericHIDHost.h"

#include <string.h>

// HID usage pages / usages we care about
#define HID_UP_GENERIC_DESKTOP 0x01
#define HID_UP_BUTTON          0x09
#define HID_USAGE_JOYSTICK     0x04
#define HID_USAGE_GAMEPAD      0x05
#define HID_USAGE_X            0x30
#define HID_USAGE_RZ           0x35
#define HID_USAGE_HAT          0x39

#define MAX_LOCAL_USAGES 16

// Pull `bits` bits starting at `bitOff` out of a report, LSB first, which is
// how HID lays fields out.
static uint32_t extractBits(uint8_t const* r, uint16_t len, uint16_t bitOff, uint8_t bits) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < bits && i < 32; i++) {
        uint16_t b = bitOff + i;
        if ((uint16_t)(b >> 3) >= len) break;
        if (r[b >> 3] & (1u << (b & 7)))
            v |= (1u << i);
    }
    return v;
}

// Read a field and rescale its logical range onto the 16-bit axis range.
static uint16_t readAxis(uint8_t const* r, uint16_t len, const HIDField& f) {
    int32_t raw = (int32_t)extractBits(r, len, f.bitOffset, f.bitSize);
    // A negative logical minimum means the field is signed
    if (f.logicalMin < 0 && f.bitSize < 32) {
        uint32_t signBit = 1u << (f.bitSize - 1);
        if ((uint32_t)raw & signBit)
            raw -= (int32_t)(1u << f.bitSize);
    }
    int32_t lo = f.logicalMin, hi = f.logicalMax;
    if (hi <= lo) return GAMEPAD_JOYSTICK_MID;
    if (raw < lo) raw = lo;
    if (raw > hi) raw = hi;
    return (uint16_t)(((int64_t)(raw - lo) * 65535) / (hi - lo));
}

bool GenericHIDHost::parseDescriptor(uint8_t const* desc, uint16_t len, HIDLayout& out) {
    out = HIDLayout();
    if (desc == nullptr || len == 0) return false;

    uint16_t usagePage = 0;
    int32_t  logMin = 0, logMax = 0;
    uint32_t reportSize = 0, reportCount = 0;
    uint8_t  curId = 0;
    uint16_t bitPos = 0;
    uint16_t usages[MAX_LOCAL_USAGES];
    uint8_t  usageCount = 0;
    uint32_t usageMin = 0;
    bool     haveUsageMin = false;
    bool     idLocked = false;   // locked onto the report carrying the axes
    int32_t  topUsage = -1;      // first usage seen on Generic Desktop

    uint16_t i = 0;
    while (i < len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) { // long item: skip it, nothing we need is encoded this way
            if (i >= len) break;
            uint8_t dataSize = desc[i];
            i += 2 + dataSize;
            continue;
        }
        uint8_t bSize = b & 0x03;
        if (bSize == 3) bSize = 4;
        uint8_t bType = (b >> 2) & 0x03;
        uint8_t bTag  = (b >> 4) & 0x0F;
        if (i + bSize > len) break;

        uint32_t uval = 0;
        for (uint8_t k = 0; k < bSize; k++)
            uval |= (uint32_t)desc[i + k] << (8 * k);
        // Signed reading, for logical min/max
        int32_t sval = (int32_t)uval;
        if (bSize > 0 && bSize < 4) {
            uint32_t signBit = 1u << (bSize * 8 - 1);
            if (uval & signBit)
                sval = (int32_t)uval - (int32_t)(1u << (bSize * 8));
        }
        i += bSize;

        if (bType == 1) {            // Global
            switch (bTag) {
                case 0x0: usagePage = (uint16_t)uval; break;
                case 0x1: logMin = sval; break;
                case 0x2: logMax = sval; break;
                case 0x7: reportSize = uval; break;
                case 0x8:            // Report ID: each report starts over at bit 0
                    curId = (uint8_t)uval;
                    bitPos = 0;
                    break;
                case 0x9: reportCount = uval; break;
                default: break;
            }
        } else if (bType == 2) {     // Local
            switch (bTag) {
                case 0x0:
                    if (usageCount < MAX_LOCAL_USAGES) usages[usageCount++] = (uint16_t)uval;
                    if (topUsage < 0 && usagePage == HID_UP_GENERIC_DESKTOP) topUsage = (int32_t)uval;
                    break;
                case 0x1: usageMin = uval; haveUsageMin = true; break;
                default: break;
            }
        } else if (bType == 0) {     // Main
            if (bTag == 0x8) {       // Input
                bool isConst = (uval & 0x01) != 0;
                bool isVar   = (uval & 0x02) != 0;
                if (!isConst && isVar && reportSize > 0) {
                    for (uint32_t n = 0; n < reportCount; n++) {
                        uint16_t usage;
                        if (usageCount > 0)
                            usage = usages[(n < usageCount) ? n : (usageCount - 1)];
                        else if (haveUsageMin)
                            usage = (uint16_t)(usageMin + n);
                        else
                            continue;
                        uint16_t off = (uint16_t)(bitPos + n * reportSize);

                        if (usagePage == HID_UP_GENERIC_DESKTOP) {
                            // Lock onto whichever report carries the axes; a
                            // device may also send unrelated ID'd reports
                            if (idLocked && curId != out.reportId) continue;
                            HIDField f;
                            f.bitOffset = off;
                            f.bitSize = (uint8_t)reportSize;
                            f.logicalMin = logMin;
                            f.logicalMax = logMax;
                            f.valid = true;
                            if (usage >= HID_USAGE_X && usage <= HID_USAGE_RZ) {
                                uint8_t idx = (uint8_t)(usage - HID_USAGE_X);
                                if (!out.axis[idx].valid) {
                                    out.axis[idx] = f;
                                    if (!idLocked) { out.reportId = curId; idLocked = true; }
                                }
                            } else if (usage == HID_USAGE_HAT && !out.hat.valid) {
                                out.hat = f;
                                if (!idLocked) { out.reportId = curId; idLocked = true; }
                            }
                        } else if (usagePage == HID_UP_BUTTON) {
                            if (idLocked && curId != out.reportId) continue;
                            if (out.buttonCount == 0) out.buttonOffset = off;
                            if (out.buttonCount < GENERIC_HID_MAX_BUTTONS) out.buttonCount++;
                        }
                    }
                }
                bitPos = (uint16_t)(bitPos + reportSize * reportCount);
            } else if (bTag == 0x9 || bTag == 0xB) { // Output / Feature: not in the input stream
                // nothing to do
            }
            // Locals are cleared by every main item
            usageCount = 0;
            haveUsageMin = false;
        }
    }

    out.isGamepad = (topUsage == HID_USAGE_JOYSTICK || topUsage == HID_USAGE_GAMEPAD);
    // Require a declared gamepad with both primary axes: keeps mice and
    // keyboards (which also report X/Y on this page) out of here.
    return out.isGamepad && out.axis[0].valid && out.axis[1].valid;
}

bool GenericHIDHost::match(uint8_t dev_addr, uint8_t instance, uint16_t vendor_id, uint16_t product_id, uint8_t const* desc_report, uint16_t desc_len) {
    HIDLayout probe;
    return parseDescriptor(desc_report, desc_len, probe);
}

void GenericHIDHost::initialize(uint8_t dev_addr, uint8_t instance, uint16_t vendor_id, uint16_t product_id, uint8_t const* desc_report, uint16_t desc_len) {
    _dev_addr = dev_addr;
    _instance = instance;
    _vendor_id = vendor_id;
    _product_id = product_id;

    parseDescriptor(desc_report, desc_len, layout);

    _controller_host_state.buttons = 0;
    _controller_host_state.dpad = 0;
    _controller_host_state.lx = GAMEPAD_JOYSTICK_MID;
    _controller_host_state.ly = GAMEPAD_JOYSTICK_MID;
    _controller_host_state.rx = GAMEPAD_JOYSTICK_MID;
    _controller_host_state.ry = GAMEPAD_JOYSTICK_MID;
    _controller_host_state.lt = 0;
    _controller_host_state.rt = 0;
}

void GenericHIDHost::process(uint8_t const* report, uint16_t len) {
    if (!layout.axis[0].valid || report == nullptr || len == 0) return;

    // Skip the leading report ID byte when the device uses them
    if (layout.reportId != 0) {
        if (report[0] != layout.reportId) return;
        report++;
        len--;
    }
    if (len == 0) return;

    _controller_host_state.lx = readAxis(report, len, layout.axis[0]);
    _controller_host_state.ly = readAxis(report, len, layout.axis[1]);

    // Right stick is Z/Rz on most pads and Rx/Ry on the rest. When Z/Rz carry
    // the stick, a remaining Rx/Ry pair is almost always the analog triggers.
    const HIDField& rxF = layout.axis[2].valid ? layout.axis[2] : layout.axis[3];
    const HIDField& ryF = layout.axis[5].valid ? layout.axis[5] : layout.axis[4];
    if (rxF.valid) _controller_host_state.rx = readAxis(report, len, rxF);
    if (ryF.valid) _controller_host_state.ry = readAxis(report, len, ryF);
    if (layout.axis[2].valid && layout.axis[5].valid) {
        if (layout.axis[3].valid) _controller_host_state.lt = (uint8_t)(readAxis(report, len, layout.axis[3]) >> 8);
        if (layout.axis[4].valid) _controller_host_state.rt = (uint8_t)(readAxis(report, len, layout.axis[4]) >> 8);
    }

    // Buttons, in the order PC gamepads conventionally report them
    static const uint16_t BUTTON_MAP[GENERIC_HID_MAX_BUTTONS] = {
        GAMEPAD_MASK_B1, GAMEPAD_MASK_B2, GAMEPAD_MASK_B3, GAMEPAD_MASK_B4,
        GAMEPAD_MASK_L1, GAMEPAD_MASK_R1, GAMEPAD_MASK_L2, GAMEPAD_MASK_R2,
        GAMEPAD_MASK_S1, GAMEPAD_MASK_S2, GAMEPAD_MASK_L3, GAMEPAD_MASK_R3,
        GAMEPAD_MASK_A1, GAMEPAD_MASK_A2, 0, 0,
    };
    _controller_host_state.buttons = 0;
    for (uint8_t n = 0; n < layout.buttonCount; n++) {
        if (extractBits(report, len, (uint16_t)(layout.buttonOffset + n), 1))
            _controller_host_state.buttons |= BUTTON_MAP[n];
    }

    // Hat switch: 8 clockwise directions from up, anything else is neutral
    _controller_host_state.dpad = 0;
    if (layout.hat.valid) {
        int32_t h = (int32_t)extractBits(report, len, layout.hat.bitOffset, layout.hat.bitSize);
        h -= layout.hat.logicalMin;
        switch (h) {
            case 0: _controller_host_state.dpad = GAMEPAD_MASK_UP; break;
            case 1: _controller_host_state.dpad = GAMEPAD_MASK_UP | GAMEPAD_MASK_RIGHT; break;
            case 2: _controller_host_state.dpad = GAMEPAD_MASK_RIGHT; break;
            case 3: _controller_host_state.dpad = GAMEPAD_MASK_RIGHT | GAMEPAD_MASK_DOWN; break;
            case 4: _controller_host_state.dpad = GAMEPAD_MASK_DOWN; break;
            case 5: _controller_host_state.dpad = GAMEPAD_MASK_DOWN | GAMEPAD_MASK_LEFT; break;
            case 6: _controller_host_state.dpad = GAMEPAD_MASK_LEFT; break;
            case 7: _controller_host_state.dpad = GAMEPAD_MASK_LEFT | GAMEPAD_MASK_UP; break;
            default: break;
        }
    }
}

void GenericHIDHost::gamepad(Gamepad * gamepad) {
    gamepad->hasAnalogTriggers   = (layout.axis[3].valid || layout.axis[4].valid);
    gamepad->hasLeftAnalogStick  = true;
    gamepad->hasRightAnalogStick = (layout.axis[2].valid || layout.axis[3].valid);
    gamepad->state.dpad    |= _controller_host_state.dpad;
    gamepad->state.buttons |= _controller_host_state.buttons;
    gamepad->state.lx       = _controller_host_state.lx;
    gamepad->state.ly       = _controller_host_state.ly;
    gamepad->state.rx       = _controller_host_state.rx;
    gamepad->state.ry       = _controller_host_state.ry;
    gamepad->state.lt       = _controller_host_state.lt;
    gamepad->state.rt       = _controller_host_state.rt;
}
