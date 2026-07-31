/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2021 Jason Skuby (mytechtoybox.com)
 */

#pragma once

#include <stdint.h>
#include <pico/unique_id.h>

#define XINPUT_ENDPOINT_SIZE 20

// Buttons 1 (8 bits)
// TODO: Consider using an enum class here.
#define XBOX_MASK_UP    (1U << 0)
#define XBOX_MASK_DOWN  (1U << 1)
#define XBOX_MASK_LEFT  (1U << 2)
#define XBOX_MASK_RIGHT (1U << 3)
#define XBOX_MASK_START (1U << 4)
#define XBOX_MASK_BACK  (1U << 5)
#define XBOX_MASK_LS    (1U << 6)
#define XBOX_MASK_RS    (1U << 7)

// Buttons 2 (8 bits)
// TODO: Consider using an enum class here.
#define XBOX_MASK_LB    (1U << 0)
#define XBOX_MASK_RB    (1U << 1)
#define XBOX_MASK_HOME  (1U << 2)
//#define UNUSED        (1U << 3)
#define XBOX_MASK_A     (1U << 4)
#define XBOX_MASK_B     (1U << 5)
#define XBOX_MASK_X     (1U << 6)
#define XBOX_MASK_Y     (1U << 7)

typedef struct __attribute((packed, aligned(1)))
{
    uint8_t report_id;
    uint8_t report_size;
    uint8_t buttons1;
    uint8_t buttons2;
    uint8_t lt;
    uint8_t rt;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
    uint8_t _reserved[6];
} XInputReport;

// XInput protocol command packets
static const uint8_t XBOX360_WIRED_RUMBLE[] = {0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t XBOX360_WIRED_LED[] = {0x01, 0x03, 0x00};

typedef enum {
    XINPUT_SUBTYPE_NONE = 0x00,
    XINPUT_SUBTYPE_GAMEPAD = 0x01,
    XINPUT_SUBTYPE_WHEEL = 0x02,
    XINPUT_SUBTYPE_ARCADE_STICK = 0x03,
    XINPUT_SUBTYPE_HOTAS = 0x04,
    XINPUT_SUBTYPE_DANCE_PAD = 0x05,
    XINPUT_SUBTYPE_GUITAR = 0x06,
    XINPUT_SUBTYPE_GUITAR_ALT = 0x07,
    XINPUT_SUBTYPE_DRUMS = 0x08,
    XINPUT_SUBTYPE_GUITAR_BASS = 0x0B,
    XINPUT_SUBTYPE_ARCADE_PAD = 0x13,
} XInputSubtype;

static const uint8_t xinput_string_language[]    = { 0x09, 0x04 };
static const uint8_t xinput_string_serial[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
// USB product string, which is the name the BirdStrike config tool and the OS
// show. Per-board so each build identifies itself; the HML pad keeps the plain
// name it has always reported.
#ifndef XINPUT_PRODUCT_NAME
#define XINPUT_PRODUCT_NAME "BirdStrike"
#endif

static const uint8_t xinput_string_manfacturer[] = "WSK";
static const uint8_t xinput_string_product[]     = XINPUT_PRODUCT_NAME;
static uint8_t xinput_string_version[]     = "08FEC93"; // Serial that is overridden by Pico ID
static const uint8_t xinput_string_xsm3[]        = "Xbox Security Method 3, Version 1.00, \xa9 2005 Microsoft Corporation. All rights reserved.";

static const uint8_t *xinput_string_descriptors[] __attribute__((unused)) =
{
    xinput_string_serial,
    xinput_string_manfacturer,
    xinput_string_product,
    xinput_string_version,
    xinput_string_xsm3
};

static const uint8_t * xinput_get_string_descriptor(int index) {
    if ( index == 3 ) {
        // Generate a serial number of hex bytes from the pico's unique ID
        pico_unique_board_id_t unique_id;
        pico_get_unique_board_id(&unique_id);
        for(int i = 0; i < 3; i++) {
            sprintf((char*)(&xinput_string_version[i*2+1]), "%02X", (uint8_t)unique_id.id[i+5]);
        }
    }

    return xinput_string_descriptors[index];
}

// Third-party-XInput identity (Flydigi/8BitDo style) instead of a byte-exact
// Xbox 360 pad clone: composite device class + a PID Windows has no device-
// level INF for, so usbccgp splits the interfaces and binds xusb22.sys to
// interface 0 via the MS OS "XUSB10" compatible ID. This is what lets the
// HML config HID interface surface on Windows next to a working XInput
// function. VID stays 0x045E so Linux xpad still matches (it accepts any
// 0x045E device carrying an FF/5D/01 interface). Real Xbox 360 console
// support is dropped in exchange (no security interface, unknown PID).
static const uint8_t xinput_device_descriptor[] =
{
    0x12,       // bLength
    0x01,       // bDescriptorType (Device)
    0x00, 0x02, // bcdUSB 2.00
    0x00,          // bDeviceClass (composite, per-interface)
    0x00,          // bDeviceSubClass
    0x00,          // bDeviceProtocol
    0x40,          // bMaxPacketSize0 64
    0x5E, 0x04, // idVendor 0x045E
    0x58, 0x48, // idProduct 0x4858
    0x14, 0x01, // bcdDevice 2.14
    0x01,       // iManufacturer (String Index)
    0x02,       // iProduct (String Index)
    0x03,       // iSerialNumber (String Index)
    0x01,       // bNumConfigurations 1
};

// MS OS 1.0 Extended Compat ID: function 0 -> XUSB10 (binds xusb22.sys),
// function 1 (the appended HML config HID interface) -> none (binds by class)
static const uint8_t xinput_ms_compat_id_descriptor[] =
{
    0x40, 0x00, 0x00, 0x00, // dwLength 64
    0x00, 0x01,             // bcdVersion 1.00
    0x04, 0x00,             // wIndex (Extended Compat ID)
    0x02,                   // bCount 2
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved
    // Function 0: XInput control interface
    0x00,                   // bFirstInterfaceNumber
    0x01,                   // reserved
    'X', 'U', 'S', 'B', '1', '0', 0x00, 0x00, // compatibleID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // subCompatibleID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // reserved
    // Function 1: HML config HID interface (no compat ID)
    0x01,                   // bFirstInterfaceNumber
    0x01,                   // reserved
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// XInput control interface only (audio/plugin/security interfaces are Xbox
// 360 console machinery — dropped with the third-party identity). The HML
// config HID interface is appended at runtime in usbdriver.cpp.
static const uint8_t xinput_configuration_descriptor[] =
{
    0x09,        // bLength
    0x02,        // bDescriptorType (Configuration)
    0x31, 0x00,  // wTotalLength 49
    0x01,        // bNumInterfaces 1
    0x01,        // bConfigurationValue
    0x00,        // iConfiguration (String Index)
    0xA0,        // bmAttributes (remote wakeup)
    0xFA,        // bMaxPower 500mA

    // Control Interface (0x5D 0xFF)
    0x09,        // bLength
    0x04,        // bDescriptorType (Interface)
    0x00,        // bInterfaceNumber 0
    0x00,        // bAlternateSetting
    0x02,        // bNumEndpoints 2
    0xFF,        // bInterfaceClass
    0x5D,        // bInterfaceSubClass
    0x01,        // bInterfaceProtocol
    0x00,        // iInterface (String Index)

    // Gamepad Descriptor
    0x11,        // bLength
    0x21,        // bDescriptorType (HID)
    0x00, 0x01,  // bcdHID 1.10
    0x01,        // SUB_TYPE
    0x25,        // reserved2
    0x81,        // DEVICE_EPADDR_IN
    0x14,        // bMaxDataSizeIn
    0x00, 0x00, 0x00, 0x00, 0x13, // reserved3
    0x02,        // DEVICE_EPADDR_OUT is this right?
    0x08,        // bMaxDataSizeOut
    0x00, 0x00,  // reserved4

    // Report IN Endpoint 1.1
    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x81,        // bEndpointAddress (IN/D2H)
    0x03,        // bmAttributes (Interrupt)
    0x20, 0x00,  // wMaxPacketSize 32
    0x01,        // bInterval 1 (unit depends on device speed)

    // Report OUT Endpoint 1.2
    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x02,        // bEndpointAddress (OUT/H2D)
    0x03,        // bmAttributes (Interrupt)
    0x20, 0x00,  // wMaxPacketSize 32
    0x08,        // bInterval 8 (unit depends on device speed)
};

typedef enum
{
    XSM360_AUTH_NONE                  = 0x00,    // None
    XSM360_GET_SERIAL                 = 0x81,    // Xbox 360 Get Controller Serial
    XSM360_INIT_AUTH                  = 0x82,    // Xbox 360 Initialize Authentication
    XSM360_RESPOND_CHALLENGE          = 0x83,    // Xbox 360 Respond with Challenge
    XSM360_AUTH_KEEPALIVE             = 0x84,    // Xbox 360 Keep Authentication Alive
    XSM360_REQUEST_STATE              = 0x86,    // Xbox 360 Request Authentication State
    XSM360_VERIFY_AUTH                = 0x87,    // Xbox 360 Verify Authentication
} XSM360AuthRequest;
