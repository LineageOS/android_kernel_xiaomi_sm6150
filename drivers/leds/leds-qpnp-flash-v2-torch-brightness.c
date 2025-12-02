// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 *
 * QPNP Flash LED v2 - Realtime Torch Brightness Control
 *
 * This module provides realtime brightness control for torch LEDs.
 * When custom_brightness is written via sysfs, the new value is
 * immediately applied to the hardware registers, allowing brightness
 * changes without toggling the LED off and on.
 *
 * Usage:
 *   echo <value_mA> > /sys/class/leds/led:torch_X/custom_brightness
 *
 * The brightness resets to default when the torch is turned off.
 */

#define pr_fmt(fmt)	"torch-brightness: %s: " fmt, __func__

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include "leds-qpnp-flash-v2-torch-brightness.h"

/*
 * Note: This file is compiled as part of leds-qpnp-flash-v2 module.
 * The actual implementation uses structures and functions from the
 * main driver file. See the #ifdef CONFIG_LEDS_QPNP_FLASH_V2_TORCH_BRIGHTNESS
 * sections in leds-qpnp-flash-v2.c for the full implementation.
 */
