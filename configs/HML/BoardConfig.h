#ifndef HML_BOARD_CONFIG_H_
#define HML_BOARD_CONFIG_H_

#include "enums.pb.h"
#include "class/hid/hid.h"

#define BOARD_CONFIG_LABEL "HML"

// SPI0: ADS8332 external ADC (sticks) lives here on stock hardware
#define SPI0_ENABLED 1
#define SPI0_PIN_RX  0
#define SPI0_PIN_CS  1
#define SPI0_PIN_SCK 2
#define SPI0_PIN_TX  3

#define DEFAULT_DEBOUNCE_DELAY 5

// Real pin mappings extracted from stock GNS firmware (v0.7.12-297 fork)
#define GPIO_PIN_08 GpioAction::BUTTON_PRESS_DOWN
#define GPIO_PIN_09 GpioAction::BUTTON_PRESS_B2
#define GPIO_PIN_12 GpioAction::BUTTON_PRESS_A2
#define GPIO_PIN_13 GpioAction::BUTTON_PRESS_B3
#define GPIO_PIN_14 GpioAction::BUTTON_PRESS_B4
#define GPIO_PIN_15 GpioAction::BUTTON_PRESS_B1
#define GPIO_PIN_16 GpioAction::BUTTON_PRESS_LEFT
#define GPIO_PIN_17 GpioAction::BUTTON_PRESS_RIGHT
#define GPIO_PIN_18 GpioAction::BUTTON_PRESS_S1
#define GPIO_PIN_19 GpioAction::BUTTON_PRESS_S2
#define GPIO_PIN_21 GpioAction::BUTTON_PRESS_R1
#define GPIO_PIN_22 GpioAction::BUTTON_PRESS_L1
#define GPIO_PIN_23 GpioAction::BUTTON_PRESS_UP
// Pins 4-7, 10, 11, 20, 24, 25, 26, 27 intentionally left unmapped:
// getHeldPins probes them with pull-downs to find the FPC PS button line.

// ---- HML Analog addon: ADS8332 sticks + analog PS button + ADC triggers ----
#define HML_ANALOG_ENABLED 1
// ADS8332 channel assignment (adjustable live in webconfig)
#define HML_ANALOG_CH_LX 0
#define HML_ANALOG_CH_LY 1
#define HML_ANALOG_CH_RX 2
#define HML_ANALOG_CH_RY 3
#define HML_ANALOG_CH_PS 4
#define HML_ANALOG_PS_THRESHOLD 32768
// Analog triggers on RP2040 ADC (from stock firmware analysis)
#define HML_ANALOG_LT_ADC_PIN 26
#define HML_ANALOG_RT_ADC_PIN 28

// Trigger calibration: raw ADC counts at rest / fully pressed (pressed pulls low)
#define HML_TRIGGER_LT_IDLE 4060
#define HML_TRIGGER_LT_PRESSED 20
#define HML_TRIGGER_RT_IDLE 2057
#define HML_TRIGGER_RT_PRESSED 20
// Digital L2/R2 fire above this fraction of travel (percent)
#define HML_TRIGGER_DIGITAL_PERCENT 40

// Boot combos: hold button while plugging in to select input mode
// B2 (right face) = XInput, B4 (top face) = PS4, R2 = P5 General (PS5)
#define DEFAULT_INPUT_MODE_R2 INPUT_MODE_P5GENERAL

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
