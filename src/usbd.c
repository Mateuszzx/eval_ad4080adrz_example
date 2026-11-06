/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB CDC ACM on the SDP-K1 ULPI port. The controller is configured
 * for high speed. On this board the link enumerates at full speed.
 * The hub on the USB-C connector keeps DAPLink on its other downstream
 * port, so the console UART stays up while this device enumerates.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(ad4080_usb, LOG_LEVEL_ERR);

/* pid.codes vendor ID, product chosen for this eval firmware. */
#define AD4080_USB_VID 0x1209
#define AD4080_USB_PID 0x4080

USBD_DEVICE_DEFINE(ad4080_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), AD4080_USB_VID,
		   AD4080_USB_PID);

USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "AD4080 eval");
USBD_DESC_PRODUCT_DEFINE(usb_product, "AD4080 capture");
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS");

/* Bus-powered, 500 mA. The analog front end still needs the 12 V adapter. */
USBD_CONFIGURATION_DEFINE(fs_config, 0, 250, &fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(hs_config, 0, 250, &hs_cfg_desc);

static const char *const blocklist[] = {NULL};

static int add_config(enum usbd_speed speed, struct usbd_config_node *config)
{
	int err;

	err = usbd_add_configuration(&ad4080_usbd, speed, config);
	if (err) {
		LOG_ERR("USB configuration failed (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&ad4080_usbd, speed, 1, blocklist);
	if (err) {
		LOG_ERR("USB class register failed (%d)", err);
		return err;
	}

	/* CDC ACM has an interface association descriptor. */
	return usbd_device_set_code_triple(&ad4080_usbd, speed, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

const struct device *usb_serial_start(void)
{
	const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
	int err;

	if (!device_is_ready(cdc)) {
		LOG_ERR("USB CDC device is not ready");
		return NULL;
	}

	err = usbd_add_descriptor(&ad4080_usbd, &usb_lang);
	if (err) {
		return NULL;
	}
	err = usbd_add_descriptor(&ad4080_usbd, &usb_mfr);
	if (err) {
		return NULL;
	}
	err = usbd_add_descriptor(&ad4080_usbd, &usb_product);
	if (err) {
		return NULL;
	}

	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&ad4080_usbd) == USBD_SPEED_HS) {
		err = add_config(USBD_SPEED_HS, &hs_config);
		if (err) {
			return NULL;
		}
	}

	err = add_config(USBD_SPEED_FS, &fs_config);
	if (err) {
		return NULL;
	}

	usbd_self_powered(&ad4080_usbd, false);

	/*
	 * The USB3343 locks its PLL from the 26 MHz clock. Release reset
	 * and let that settle before the OTG core starts high-speed chirp.
	 */
	{
		const struct gpio_dt_spec reset =
			GPIO_DT_SPEC_GET(DT_NODELABEL(otghs_ulpi_phy), reset_gpios);

		if (gpio_is_ready_dt(&reset)) {
			gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
			k_msleep(5);
			gpio_pin_configure_dt(&reset, GPIO_OUTPUT_INACTIVE);
			k_msleep(10);
		}
	}

	err = usbd_init(&ad4080_usbd);
	if (err) {
		LOG_ERR("USB init failed (%d)", err);
		return NULL;
	}

	if (!usbd_can_detect_vbus(&ad4080_usbd)) {
		err = usbd_enable(&ad4080_usbd);
		if (err) {
			LOG_ERR("USB enable failed (%d)", err);
			return NULL;
		}
	}

	return cdc;
}
