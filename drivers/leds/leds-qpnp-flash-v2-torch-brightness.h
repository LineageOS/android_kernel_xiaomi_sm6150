/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 *
 * QPNP Flash LED v2 - Realtime Torch Brightness Control
 *
 * Provides sysfs interface for realtime torch brightness adjustment.
 * Brightness can be changed while the torch is on without toggling.
 */

#ifndef __LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS_H
#define __LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS_H

#ifdef CONFIG_LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS

/* Default torch brightness in mA */
#define TORCH_BRIGHTNESS_DEFAULT	225

/*
 * Sysfs show function for custom_brightness attribute
 */
ssize_t torch_brightness_show(struct device *dev,
			      struct device_attribute *attr, char *buf);

/*
 * Sysfs store function for custom_brightness attribute
 * Applies brightness change in realtime to hardware registers.
 */
ssize_t torch_brightness_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t size);

#endif /* CONFIG_LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS */

#endif /* __LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS_H */
