// Vendor-defined HID feature-report channel for real-time HML analog tuning.
//
// A browser (WebHID) talks to the running gamepad without entering web-config
// mode, FireBird-web-tool style: settings are written straight into the RAM
// copy of HMLAnalogOptions (the addon re-reads it every frame) and are only
// persisted to flash on an explicit SAVE command.
//
// Wire protocol (all values little-endian int32 unless noted):
//   Feature report 0x60 (host -> device, 63 bytes):
//     [0]=command
//       0x01 SET_PARAM: [1]=target [2]=paramId [3]=index [4..7]=value
//       0x02 SAVE:      persist current RAM config to flash
//       0x04 REBOOT:    [1]=0 normal, 1 BOOTSEL (UF2), 2 web-config
//       0x05 SET_INPUT_MODE: [1]=InputMode enum value; saves and reboots
//       0x03 SELECT:    choose what the next 0x61 GET returns
//               [1]=0x00 INFO
//               [1]=0x01 RAW    (live raw ADC + processed stick output)
//               [1]=0x02 PARAM: [2]=target [3]=paramId [4]=index
//               [1]=0x03 DUMP:  [2]=target [3]=page (9 params per page)
//   Feature report 0x61 (device -> host, 63 bytes): response, [0] echoes the
//   selected page type (0xFF if the selection was invalid).
//
// target: 0 = left stick, 1 = right stick, 2 = HMLAnalogOptions globals.
// paramId is the proto field number in HMLStickOptions / HMLAnalogOptions;
// index addresses elements of repeated fields (0 for scalars).

#ifndef _HMLConfigChannel_H_
#define _HMLConfigChannel_H_

#include <stdint.h>
#include "enums.pb.h"

namespace HMLConfigChannel {

// InputMode values that can be selected from the config tool / mode combos
// (gaps in the enum and INPUT_MODE_CONFIG are rejected)
inline bool isSelectableInputMode(int32_t mode) {
    return (mode >= INPUT_MODE_XINPUT && mode <= INPUT_MODE_SINPUT) ||
           mode == INPUT_MODE_SWITCH2_PRO;
}

constexpr uint8_t REPORT_SET = 0x60;
constexpr uint8_t REPORT_GET = 0x61;
// v2: REBOOT / SET_INPUT_MODE commands, INFO carries current input mode
// v3: comboModeB1-B4 global params (Share+Options+face-button mode switch)
// v4: RAW page [40..47] = loop avg / loop max (last 1s) / ADC acquire time /
//     sample age at stick processing, all u16 microseconds
constexpr uint8_t PROTOCOL_VERSION = 4;

// Handle a SET_REPORT (feature) with report id REPORT_SET
void handleSet(const uint8_t *buffer, uint16_t bufsize);

// Handle a GET_REPORT (feature) with report id REPORT_GET.
// Fills buffer (up to reqlen) and returns the number of bytes written.
uint16_t handleGet(uint8_t *buffer, uint16_t reqlen);

// Standalone report descriptor for the dedicated config HID interface
// (= HML_CONFIG_REPORT_DESC_BLOCK as an array)
extern const uint8_t hml_config_report_descriptor[];
extern const uint16_t hml_config_report_descriptor_len;

// Append a dedicated config HID interface (interface + HID + interrupt-IN
// endpoint descriptors) to a configuration descriptor. Returns a patched RAM
// copy and records which tinyusb HID instance the new interface will get, or
// the original pointer unchanged if patching is not possible. Call once per
// GET_CONFIGURATION_DESCRIPTOR; the returned buffer stays valid until the
// next call.
const uint8_t *patchConfigDescriptor(const uint8_t *desc);

// Mark the dedicated interface as absent (modes that must not grow extra
// interfaces, e.g. PS5, keep their descriptor untouched)
void disableInterface();

// True if the last enumerated configuration includes the dedicated interface
bool isInterfaceEnabled();
// tinyusb HID instance index of the dedicated interface (valid when enabled)
uint8_t getHidInstance();

// Report-descriptor snippet declaring the two feature reports as a top-level
// vendor collection; append after the last collection of a driver's report
// descriptor (and grow the descriptor lengths accordingly).
#define HML_CONFIG_REPORT_DESC_BLOCK \
    0x06, 0x60, 0xFF,  /* Usage Page (Vendor Defined 0xFF60) */ \
    0x09, 0x01,        /* Usage (0x01) */ \
    0xA1, 0x01,        /* Collection (Application) */ \
    0x85, 0x60,        /*   Report ID (0x60) */ \
    0x09, 0x02,        /*   Usage (0x02) */ \
    0x15, 0x00,        /*   Logical Minimum (0) */ \
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */ \
    0x75, 0x08,        /*   Report Size (8) */ \
    0x95, 0x3F,        /*   Report Count (63) */ \
    0xB1, 0x02,        /*   Feature (Data,Var,Abs) */ \
    0x85, 0x61,        /*   Report ID (0x61) */ \
    0x09, 0x03,        /*   Usage (0x03) */ \
    0x95, 0x3F,        /*   Report Count (63) */ \
    0xB1, 0x02,        /*   Feature (Data,Var,Abs) */ \
    0xC0               /* End Collection */

} // namespace HMLConfigChannel

#endif
