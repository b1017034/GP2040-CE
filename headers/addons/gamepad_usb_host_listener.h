#ifndef _GamepadUSBHostListener_H
#define _GamepadUSBHostListener_H

#include "usblistener.h"
#include "gamepad.h"
#include "host/usbh.h"

#include "gphost.h"

const uint32_t GAMEPAD_HOST_POLL_INTERVAL_MS = 3;

// Add other controller structs here
class GamepadUSBHostListener : public USBListener {
    public:// USB Listener Features
        virtual void setup();
        virtual void mount(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len);
        virtual void xmount(uint8_t dev_addr, uint8_t instance, uint8_t controllerType, uint8_t subtype);
        virtual void unmount(uint8_t dev_addr);
        virtual void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);
        virtual void report_sent(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {}
        virtual void set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len);
        virtual void get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len);
        void update();

        // What the last HID mount looked like, so an unsupported controller can
        // be diagnosed from the live view instead of guessed at. driver is the
        // host driver that claimed it, or "none" when nothing matched.
        struct MountInfo {
            uint16_t vid = 0;
            uint16_t pid = 0;
            uint16_t descLen = 0;
            uint8_t  instance = 0;
            bool     seen = false;
            bool     hid = false;   // false = arrived via the xinput path
            const char* driver = "none";
            // Filled in when nothing claimed it: why GenericHIDHost declined
            bool     isGamepadUsage = false;
            bool     hasXY = false;
        };
        static MountInfo lastMount;

        // True for PS4/PS5 and P5General auth dongles, which must not be
        // mistaken for the input controller
        static bool isAuthDongle(uint16_t vid, uint16_t pid, uint8_t const* desc_report, uint16_t desc_len);
        // True when a purpose-written driver covers this device
        static bool matchesSpecificHost(uint8_t dev_addr, uint8_t instance, uint16_t vid, uint16_t pid, uint8_t const* desc_report, uint16_t desc_len);
    private:
        uint8_t _controller_dev_addr;
        uint8_t _controller_instance;
        GPHost * _controller_host;
        bool _host_is_generic = false;
        uint32_t _next_update;
};

#endif
