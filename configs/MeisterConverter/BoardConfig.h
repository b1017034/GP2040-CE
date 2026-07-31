#ifndef MEISTERCONVERTER_BOARD_CONFIG_H_
#define MEISTERCONVERTER_BOARD_CONFIG_H_

#include "enums.pb.h"
#include "class/hid/hid.h"

// MeisterConverter (github.com/AlpenmeisterCustoms/MeisterConverter) as a
// DualSense tuning pass-through: DualSense + PS5 auth dongle plug into the
// board's USB-A inputs, the board outputs to the console from USB1.
//
// Wiring taken from the V1.1 KiCad source (the published repo ships gerbers
// and a BOM only; the schematic is recoverable from git history):
//   U3  RP2040
//   U4  CH334R 4-port hub, upstream pair D+2/D-2
//   R10 27R  D+2 <-> U3 pad36 = GPIO24   <- PIO-USB host D+
//   R11 27R  D-2 <-> U3 pad37 = GPIO25   <- PIO-USB host D-
//   R3/R4 27R  D+/D- = native USB device -> USB1 (to console)
//   USB2/3/4  three USB-A receptacles behind the hub
//   SW19 -> GPIO1 (only user button), SW20 -> RUN, SW18 -> BOOTSEL
//   F1   500mA polyfuse feeding the shared VBUS of all three input ports
// U3 SWCLK/SWDIO (pads 24/25) are unconnected: no SWD, debug over USB only.
#define BOARD_CONFIG_LABEL "MeisterConverter"

// SW19 is the only user button on the board (SW20 = RUN, SW18 = BOOTSEL).
// Spend it on S2, matching the vendor's own config: S2 is what boots into web
// config (hold while plugging in), which is the one thing that must not depend
// on a host controller being enumerated.
#define GPIO_PIN_01 GpioAction::BUTTON_PRESS_S2
// Everything else is unconnected. GP2040-CE pulls unmapped inputs up, so the
// floating GPIO0-23 nets read as released.

// PIO-USB host feeding the CH334R hub, so the DualSense and the auth dongle
// enumerate together underneath it (CFG_TUH_HUB=1, CFG_TUH_DEVICE_MAX=4 covers
// hub + 2 devices).
#define USB_PERIPHERAL_ENABLED 1
#define USB_PERIPHERAL_PIN_DPLUS 24
#define USB_PERIPHERAL_PIN_ORDER 0
// Keep the GPIO mapper off the host pair; PIO-USB owns these two.
#define GPIO_PIN_24 GpioAction::ASSIGNED_TO_ADDON
#define GPIO_PIN_25 GpioAction::ASSIGNED_TO_ADDON

// Read the DualSense through the USB host add-on...
#define GAMEPAD_USB_HOST_ENABLED 1
// ...then tune those sticks. With no SPI block the HMLAnalog add-on takes its
// sticks from gamepad state instead of an ADS8332, so the FireBird RC filter,
// curves and deadzones apply to the DualSense. No ADC, triggers, stick clicks
// or lightbar on this board.
#define HML_ANALOG_ENABLED 1
#define HML_ANALOG_CH_LX -1
#define HML_ANALOG_CH_LY -1
#define HML_ANALOG_CH_RX -1
#define HML_ANALOG_CH_RY -1
#define HML_ANALOG_CH_PS -1
#define HML_ANALOG_CH_L3 -1
#define HML_ANALOG_CH_R3 -1
#define HML_ANALOG_L3_PIN -1
#define HML_ANALOG_R3_PIN -1
#define HML_ANALOG_LT_ADC_PIN -1
#define HML_ANALOG_RT_ADC_PIN -1
#define HML_LIGHTBAR_ENABLED false

// Boot into XInput, NOT P5General: P5GeneralDriver::process() returns false
// until the dongle authenticates, so a board defaulting to P5General looks dead
// on a PC (and on a PS5 with the dongle unplugged). XInput needs no auth, so
// the board is always usable out of the box.
//
// There are no buttons here for boot-time mode selection, so mode switching is
// the runtime combo on the host controller: hold Share+Options+face button for
// REBOOT_HOTKEY_HOLD_TIME_MS - x = XInput, o = P5General, [] = PS4, ^ = Switch
// (assignments live in hmlAnalogOptions.comboModeB1..B4). Share+Options+R1 gets
// to web config the same way.
#define DEFAULT_INPUT_MODE INPUT_MODE_XINPUT

#define DEFAULT_DEBOUNCE_DELAY 5
#define DEFAULT_USB_POLL_RATE 1000

// Keyboard Mapping Configuration (required defaults)
#define KEY_DPAD_UP     HID_KEY_ARROW_UP
#define KEY_DPAD_DOWN   HID_KEY_ARROW_DOWN
#define KEY_DPAD_RIGHT  HID_KEY_ARROW_RIGHT
#define KEY_DPAD_LEFT   HID_KEY_ARROW_LEFT
#define KEY_BUTTON_B1   HID_KEY_SHIFT_LEFT
#define KEY_BUTTON_B2   HID_KEY_Z
#define KEY_BUTTON_R2   HID_KEY_X
#define KEY_BUTTON_L2   HID_KEY_V
#define KEY_BUTTON_B3   HID_KEY_CONTROL_LEFT
#define KEY_BUTTON_B4   HID_KEY_ALT_LEFT
#define KEY_BUTTON_R1   HID_KEY_SPACE
#define KEY_BUTTON_L1   HID_KEY_C
#define KEY_BUTTON_S1   HID_KEY_5
#define KEY_BUTTON_S2   HID_KEY_1
#define KEY_BUTTON_L3   HID_KEY_EQUAL
#define KEY_BUTTON_R3   HID_KEY_MINUS
#define KEY_BUTTON_A1   HID_KEY_9
#define KEY_BUTTON_A2   HID_KEY_F2
#define KEY_BUTTON_FN   HID_KEY_F1

#endif
