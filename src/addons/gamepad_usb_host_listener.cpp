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

// A device whose job is signing auth challenges, not sending inputs: the PS4/
// PS5 dongle (Sony vendor page 0xFFF0 with report 0xF3, the same test
// PS4AuthUSBListener uses) or the P5General dongle.
bool GamepadUSBHostListener::isAuthDongle(uint16_t vid, uint16_t pid, uint8_t const* desc_report, uint16_t desc_len) {
    if ( vid == 0x2B81 && pid == 0x0101 ) return true;   // P5General
    if ( desc_report == nullptr || desc_len == 0 ) return false;
    tuh_hid_report_info_t report_info[4];
    uint8_t report_count = tuh_hid_parse_report_descriptor(report_info, 4, desc_report, desc_len);
    for ( uint8_t i = 0; i < report_count; i++ ) {
        if ( report_info[i].usage_page == 0xFFF0 && report_info[i].report_id == 0xF3 )
            return true;
    }
    return false;
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
        lastMount.driver = "auth-dongle";
        return;
    }

    // Only host one controller at a time
    if ( _controller_host != nullptr ) {
        lastMount.driver = "busy";
        return;
    }

    _controller_host = nullptr;
     if ( DualshockPS4Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DualshockPS4Host();
        lastMount.driver = "ds4";
    }else if ( DualsensePS5Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DualsensePS5Host();
        lastMount.driver = "dualsense";
    } else if ( GoogleStadiaHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new GoogleStadiaHost();
        lastMount.driver = "stadia";
    } else if ( DrivingForceHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new DrivingForceHost();
        lastMount.driver = "drivingforce";
    } else if ( Ultrastik360Host::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new Ultrastik360Host();
        lastMount.driver = "ultrastik360";
    } else if ( SwitchProHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        _controller_host = new SwitchProHost();
        lastMount.driver = "switchpro";
    } else if ( GenericHIDHost::match(dev_addr, instance, vid, pid, desc_report, desc_len) ) {
        // Last resort: anything that declares itself a joystick or gamepad and
        // reports X/Y. The driver reads the report descriptor for the field
        // layout, so no VID/PID entry is needed.
        _controller_host = new GenericHIDHost();
        lastMount.driver = "generic";
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

    // Only host one controller at a time
    if ( _controller_host != nullptr ) {
        lastMount.driver = "busy";
        return;
    }

    // Match is a static function, setup and init happens after we've established something
    _controller_host = nullptr;
    if ( Xbox360Host::match(dev_addr, instance, vid, pid, controllerType) ) {
        _controller_host = new Xbox360Host();
        lastMount.driver = "xbox360";
    }
    /* else if ( XboxOneHost::match(vid, pid && controllerType == 2 ) {
        _controller_host = new XboxOneHost();
    }*/

    if ( _controller_host != nullptr ) {
        _controller_dev_addr = dev_addr;
        _controller_instance = instance;
        _controller_host->initialize(dev_addr, instance, vid, pid, nullptr, 0);
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
}

void GamepadUSBHostListener::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    if ( _controller_host != nullptr && 
        _controller_dev_addr == dev_addr &&
        _controller_instance == instance ) {
        _controller_host->process(report, len);
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
