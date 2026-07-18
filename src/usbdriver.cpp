/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2024 Open Stick Community (gp2040-ce.info)
 */

#ifndef _USBDRIVER_CPP_
#define _USBDRIVER_CPP_

#include <string.h>

#include "tusb.h"
#include "drivermanager.h"
#include "storagemanager.h"

static bool usb_mounted;
static bool usb_suspended;

bool get_usb_mounted(void) {
	return usb_mounted;
}

bool get_usb_suspended(void) {
	return usb_suspended;
}

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count) {
	*driver_count = 1;
	return DriverManager::getInstance().getDriver()->get_class_driver();
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
	return DriverManager::getInstance().getDriver()->get_report(report_id, report_type, buffer, reqlen);
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
	DriverManager::getInstance().getDriver()->set_report(report_id, report_type, buffer, bufsize);
}

// Invoked when device is mounted
void tud_mount_cb(void)
{
	usb_mounted = true;
	usb_suspended = false;
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
	usb_mounted = false;
	usb_suspended = false;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
	(void)remote_wakeup_en;
	usb_suspended = true;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
	usb_suspended = false;
}

// Vendor Controlled XFER occured
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
	return DriverManager::getInstance().getDriver()->vendor_control_xfer_cb(rhport, stage, request);
}


// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
	return DriverManager::getInstance().getDriver()->get_descriptor_string_cb(index, langid);
}

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb() {
	return DriverManager::getInstance().getDriver()->get_descriptor_device_cb();
}

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
	return DriverManager::getInstance().getDriver()->get_hid_descriptor_report_cb(itf);
}

// Rewrite the interrupt IN endpoint bInterval on interface 0 (the gamepad data
// interface in every driver) to match the configured USB poll rate. Descriptors
// are const flash arrays, so patch a RAM copy.
static uint8_t const *applyUsbPollRate(uint8_t const *desc) {
	static uint8_t patchedDesc[256];

	if (desc == nullptr || DriverManager::getInstance().isConfigMode())
		return desc;

	uint32_t pollRateHz = Storage::getInstance().getGamepadOptions().usbPollRate;
	if (pollRateHz == 0)
		return desc;

	uint16_t totalLength = (uint16_t)(desc[2] | (desc[3] << 8));
	if (totalLength < TUD_CONFIG_DESC_LEN || totalLength > sizeof(patchedDesc))
		return desc;

	// 1000Hz -> 1ms, 500Hz -> 2ms, 250Hz -> 4ms, 125Hz -> 8ms
	uint32_t interval = (pollRateHz >= 1000) ? 1 : (1000 / pollRateHz);
	if (interval > 16)
		interval = 16;

	memcpy(patchedDesc, desc, totalLength);

	uint16_t pos = 0;
	int32_t itf = -1;
	while (pos + 2 <= totalLength) {
		uint8_t len = patchedDesc[pos];
		uint8_t type = patchedDesc[pos + 1];
		if (len < 2 || pos + len > totalLength)
			break;
		if (type == TUSB_DESC_INTERFACE && len >= 3) {
			itf = patchedDesc[pos + 2];
		} else if (type == TUSB_DESC_ENDPOINT && len >= 7 && itf == 0) {
			uint8_t epAddr = patchedDesc[pos + 2];
			uint8_t xferType = patchedDesc[pos + 3] & 0x03;
			if ((epAddr & TUSB_DIR_IN_MASK) && xferType == TUSB_XFER_INTERRUPT)
				patchedDesc[pos + 6] = (uint8_t)interval;
		}
		pos += len;
	}
	return patchedDesc;
}

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
	return applyUsbPollRate(DriverManager::getInstance().getDriver()->get_descriptor_configuration_cb(index));
}

uint8_t const* tud_descriptor_device_qualifier_cb() {
	return DriverManager::getInstance().getDriver()->get_descriptor_device_qualifier_cb();
}

#endif
