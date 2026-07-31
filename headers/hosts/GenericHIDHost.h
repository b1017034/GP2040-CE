/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 OpenStickCommunity (gp2040-ce.info)
 */

#ifndef _GENERICHIDHOST_H_
#define _GENERICHIDHOST_H_

#include <gphost.h>

// Fallback host for any HID device that declares itself a joystick or gamepad.
// Every other host driver matches a fixed VID/PID, so anything not on those
// lists used to be dropped silently; this one reads the report descriptor and
// works out where the axes, hat and buttons actually sit in the report.
#define GENERIC_HID_MAX_BUTTONS 16

// One variable field located in an input report
struct HIDField {
    uint16_t bitOffset = 0;
    uint8_t  bitSize   = 0;
    int32_t  logicalMin = 0;
    int32_t  logicalMax = 0;
    bool     valid = false;
};

// Everything parseDescriptor() can pull out of a report descriptor
struct HIDLayout {
    // Generic Desktop axes, indexed by usage: X, Y, Z, Rx, Ry, Rz
    HIDField axis[6];
    HIDField hat;
    uint16_t buttonOffset = 0;
    uint8_t  buttonCount  = 0;
    uint8_t  reportId     = 0;     // 0 = reports are not ID-prefixed
    bool     isGamepad    = false; // top-level usage was Joystick or Gamepad
    // Licensed PS4-class pad: buttons are ordered Square, Cross, Circle,
    // Triangle rather than the A, B, X, Y a PC pad reports
    bool     sonyLayout   = false;
};

class GenericHIDHost : public GPHost {
public:
    static bool match(uint8_t dev_addr, uint8_t instance, uint16_t vendor_id, uint16_t product_id, uint8_t const* desc_report, uint16_t desc_len);
    virtual void initialize(uint8_t dev_addr, uint8_t instance, uint16_t vendor_id, uint16_t product_id, uint8_t const* desc_report, uint16_t desc_len);
    virtual void gamepad(Gamepad * gamepad);
    virtual void process(uint8_t const* report, uint16_t len);
    virtual void update() {}
    virtual void shutdown() {}
    virtual void set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len) {}
    virtual void get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len) {}

    // Walk a HID report descriptor and fill in where the gamepad fields live.
    // Returns true only for a usable gamepad: declared as one, with X and Y.
    static bool parseDescriptor(uint8_t const* desc, uint16_t len, HIDLayout& out);

private:
    HIDLayout layout;
};

#endif // _GENERICHIDHOST_H_
