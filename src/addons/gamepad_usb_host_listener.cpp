#include "addons/gamepad_usb_host_listener.h"
#include "storagemanager.h"
#include "class/hid/hid.h"
#include "class/hid/hid_host.h"

#include "hosts/DualsensePS5Host.h"
#include "hosts/DualshockPS4Host.h"
#include "hosts/GoogleStadiaHost.h"
#include "hosts/DrivingForceHost.h"
#include "hosts/Ultrastik360Host.h"
#include "hosts/SwitchProHost.h"
#include "hosts/Xbox360Host.h"
#include "hosts/GenericHIDHost.h"

/*
#include "hosts/XboxOneHost.h"
*/

GamepadUSBHostListener::MountInfo GamepadUSBHostListener::lastMount;
GamepadUSBHostListener::MountInfo GamepadUSBHostListener::active;
uint32_t GamepadUSBHostListener::sampleTick = 0;

const char* GamepadUSBHostListener::driverName(uint8_t code) {
    switch (code) {
        case DRV_BUSY:         return "busy";
        case DRV_AUTH_DONGLE:  return "auth-dongle";
        case DRV_DS4:          return "ds4";
        case DRV_DUALSENSE:    return "dualsense";
        case DRV_SWITCHPRO:    return "switchpro";
        case DRV_XBOX360:      return "xbox360";
        case DRV_STADIA:       return "stadia";
        case DRV_DRIVINGFORCE: return "drivingforce";
        case DRV_ULTRASTIK360: return "ultrastik360";
        case DRV_GENERIC:      return "generic";
        default:               return "none";
    }
}

// A device that only signs auth challenges and never sends inputs.
//
// Deliberately NOT the Sony vendor-page test PS4AuthUSBListener uses: every
// licensed PS4 controller carries that interface - that is what makes it usable
// as a dongle - so keying on it would reject real DualShock 4s and third-party
// PS4 pads as input devices. Only the P5General dongle, which is a dongle and
// nothing else, is identified here. Dongle-vs-controller contention for the
// single controller slot is handled by preferring specific drivers instead.
bool GamepadUSBHostListener::isAuthDongle(uint16_t vid, uint16_t pid, uint8_t const* desc_report, uint16_t desc_len) {
    return ( vid == 0x2B81 && pid == 0x0101 );   // P5General
}

// Does a driver written for this exact device exist, as opposed to the generic
// descriptor-driven fallback?
bool GamepadUSBHostListener::matchesSpecificHost(uint8_t dev_addr, uint8_t instance, uint16_t vid, uint16_t pid, uint8_t const* desc_report, uint16_t desc_len) {
    return DualshockPS4Host::match(dev_addr, instance, vid, pid, desc_report, desc_len)
        || DualsensePS5Host::match(dev_addr, instance, vid, pid, desc_report, desc_len)
        || GoogleStadiaHost::match(dev_addr, instance, vid, pid, desc_report, desc_len)
        || DrivingForceHost::match(dev_addr, instance, vid, pid, desc_report, desc_len)
        || Ultrastik360Host::match(dev_addr, instance, vid, pid, desc_report, desc_len)
        || SwitchProHost::match(dev_addr, instance, vid, pid, desc_report, desc_len);
}

void GamepadUSBHostListener::setup() {
    _controller_host = nullptr;
    _controller_dev_addr = 0;
    _controller_instance = 0;
    _next_update = 0;
}

void GamepadUSBHostListener::update() {
    if ( _controller_host == nullptr ) return;

    // Controller host updates (rumble, LEDs, etc. outside of gamepad input reports)
    if (getMillis() > _next_update) {
        _controller_host->update();
        _next_update = getMillis() + GAMEPAD_HOST_POLL_INTERVAL_MS;
    }
    
    // Controller host driver changes gamepad
    Gamepad *gamepad = Storage::getInstance().GetGamepad();
    _controller_host->gamepad(gamepad);    
}

void GamepadUSBHostListener::mount(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    // Record every HID mount, including ones we end up rejecting - a controller
    // that never appears here was never handed to us by the USB stack at all,
    // which is a different problem from one we declined to claim.
    lastMount = MountInfo();
    lastMount.seen = true;
    lastMount.hid = true;
    lastMount.vid = vid;
    lastMount.pid = pid;
    lastMount.descLen = desc_len;
    lastMount.instance = instance;

    // Never claim an authentication dongle as the input controller. Licensed
    // PS4/PS5 dongles and sticks also present a gamepad collection, so with a
    // single controller slot the dongle would take it and lock the real
    // controller out. They are identified the same way PS4AuthUSBListener does
    // it - the Sony vendor page - plus the P5General dongle by VID/PID.
    if ( isAuthDongle(vid, pid, desc_report, desc_len) ) {
        lastMount.driver = DRV_AUTH_DONGLE;
        return;
    }

    // Only host one controller at a time. The exception is a slot held by the
    // generic driver: a licensed dongle or stick answers the generic match too,
    // so if it mounted first it would lock out a controller we have a real
    // driver for. Let a specific driver take the slot from a generic one.
    if ( _controller_host != nullptr ) {
        if ( !_host_is_generic ||
             !matchesSpecificHost(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
            lastMount.driver = DRV_BUSY;
            return;
        }
        delete _controller_host;
        _controller_host = nullptr;
    }

    _controller_host = nullptr;
    _host_is_generic = false;
     if ( DualshockPS4Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DualshockPS4Host();
        lastMount.driver = DRV_DS4;
    }else if ( DualsensePS5Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DualsensePS5Host();
        lastMount.driver = DRV_DUALSENSE;
    } else if ( GoogleStadiaHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new GoogleStadiaHost();
        lastMount.driver = DRV_STADIA;
    } else if ( DrivingForceHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DrivingForceHost();
        lastMount.driver = DRV_DRIVINGFORCE;
    } else if ( Ultrastik360Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new Ultrastik360Host();
        lastMount.driver = DRV_ULTRASTIK360;
    } else if ( SwitchProHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new SwitchProHost();
        lastMount.driver = DRV_SWITCHPRO;
    } else if ( GenericHIDHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        // Last resort: anything that declares itself a joystick or gamepad and
        // reports X/Y. The driver reads the report descriptor for the field
        // layout, so no VID/PID entry is needed.
        _controller_host = new GenericHIDHost();
        _host_is_generic = true;
        lastMount.driver = DRV_GENERIC;
    }

    // Nothing claimed it: record how far the generic parser got, which
    // separates "not declared a gamepad" from "no X/Y" from "no descriptor"
    if ( _controller_host == nullptr ) {
        HIDLayout probe;
        GenericHIDHost::parseDescriptor(desc_report, desc_len, probe);
        lastMount.isGamepadUsage = probe.isGamepad;
        lastMount.hasXY = probe.axis[0].valid && probe.axis[1].valid;
    }

    if ( _controller_host != nullptr ) {
        _controller_dev_addr = dev_addr;
        _controller_instance = instance;
        _controller_host->initialize(dev_addr, instance, vid, pid, desc_report, desc_len);
        active = lastMount;
    } else { 
        // send message to the main screen that we have an unknown controller
        
        // if auto-detect is enabled, let's try to determine controller type by sending some special packets
        // Order: ps4
        // Order: ps5
        // Order: switch pro
        // Order: generic HID
    }
}

void GamepadUSBHostListener::xmount(uint8_t dev_addr, uint8_t instance, uint8_t controllerType, uint8_t subtype) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    lastMount = MountInfo();
    lastMount.seen = true;
    lastMount.hid = false;   // XInput devices are vendor class, never HID
    lastMount.vid = vid;
    lastMount.pid = pid;
    lastMount.instance = instance;

    // As in mount(): an XInput pad has a real driver, so it may take the slot
    // off the generic fallback
    if ( _controller_host != nullptr ) {
        if ( !_host_is_generic || !Xbox360Host::match(dev_addr, instance, vid, pid, controllerType) ) {
            lastMount.driver = DRV_BUSY;
            return;
        }
        delete _controller_host;
        _controller_host = nullptr;
    }

    // Match is a static function, setup and init happens after we've established something
    _controller_host = nullptr;
    _host_is_generic = false;
    if ( Xbox360Host::match(dev_addr, instance, vid, pid, controllerType) ) {
        _controller_host = new Xbox360Host();
        lastMount.driver = DRV_XBOX360;
    }
    /* else if ( XboxOneHost::match(vid, pid && controllerType == 2 ) {
        _controller_host = new XboxOneHost();
    }*/

    if ( _controller_host != nullptr ) {
        _controller_dev_addr = dev_addr;
        _controller_instance = instance;
        _controller_host->initialize(dev_addr, instance, vid, pid, nullptr, 0);
        active = lastMount;
    } else {
        // send message to the main screen that we have an unknown controller

        // if auto-magic
        // check for xbox 360 markers
        // check for xbox one markers
    }
}

void GamepadUSBHostListener::unmount(uint8_t dev_addr) {
    if ( _controller_host == nullptr ) return;
    
    _controller_host->shutdown();
    delete _controller_host;
    _controller_host = nullptr;
    _host_is_generic = false;
    active = MountInfo();
}

void GamepadUSBHostListener::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    if ( _controller_host != nullptr &&
        _controller_dev_addr == dev_addr &&
        _controller_instance == instance ) {
        _controller_host->process(report, len);
        sampleTick++;
    }
}

void GamepadUSBHostListener::set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len) {
    if ( _controller_host != nullptr &&
        _controller_dev_addr == dev_addr &&
        _controller_instance == instance ) {
        _controller_host->set_report_complete(dev_addr, instance, report_id, report_type, len);
    } 
}

void GamepadUSBHostListener::get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint8_t report_type, uint16_t len) {
    if ( _controller_host != nullptr &&
        _controller_dev_addr == dev_addr &&
        _controller_instance == instance ) {
        _controller_host->get_report_complete(dev_addr, instance, report_id, report_type, len);
    } 
}
