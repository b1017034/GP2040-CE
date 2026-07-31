#include "addons/gamepad_usb_host.h"
#include "addons/gamepad_usb_host_listener.h"
#include "storagemanager.h"
#include "drivermanager.h"
#include "usbhostmanager.h"
#include "peripheralmanager.h"
#include "class/hid/hid_host.h"

GamepadUSBHostAddon* GamepadUSBHostAddon::instance = nullptr;

bool GamepadUSBHostAddon::available() {
    const GamepadUSBHostOptions& gamepadUSBHostOptions = Storage::getInstance().getAddonOptions().gamepadUSBHostOptions;
    return gamepadUSBHostOptions.enabled && PeripheralManager::getInstance().isUSBEnabled(0);
}

void GamepadUSBHostAddon::setup() {
    instance = this;
    listener = new GamepadUSBHostListener();
    listener->setup();
}

void GamepadUSBHostAddon::preprocess() {
    ((GamepadUSBHostListener*)listener)->update();
}

// Web-config runs USBHostManager::process() but skips Core0 add-ons, so the
// controller enumerates yet its sticks never reach gamepad state. Drive just
// this add-on from there so the live view can show the raw controller values -
// nothing else runs, so what lands in gamepad state is pre-tuning.
void GamepadUSBHostAddon::hostTick() {
    if (instance != nullptr && instance->listener != nullptr)
        instance->preprocess();
}