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

// SW19 is the only user button on the board (SW20 = RUN, SW18 = BOOTSEL), so
// spend it on the hotkey modifier: FN plus a DualSense button reaches every
// hotkey action without needing a free hand on the converter.
#define GPIO_PIN_01 GpioAction::BUTTON_PRESS_FN
// Everything else is unconnected. GP2040-CE pulls unmapped inputs up, so the
// floating GPIO0-23 nets read as released.

// PIO-USB host feeding the CH334R hub, so the DualSense and the auth dongle
// enumerate together underneath it (CFG_TUH_HUB=1, CFG_TUH_DEVICE_MAX=4 covers
// hub + 2 devices).
#define USB_PERIPHERAL_ENABLED 1
#define USB_PERIPHERAL_PIN_DPLUS 24
#define USB_PERIPHERAL_PIN_ORDER 0

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

// The point of the board is PS5, so boot straight into P5General; the dongle on
// one of the hub ports signs every report.
#define DEFAULT_INPUT_MODE INPUT_MODE_P5GENERAL

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
