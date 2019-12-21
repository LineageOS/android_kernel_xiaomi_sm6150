/*
 * A new exposure driver based on SDE dim layer for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019, Devries <therkduan@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef SDE_EXPO_DIM_LAYER_H
#define SDE_EXPO_DIM_LAYER_H

#define BL_DC_THRESHOLD 241

enum {
	BRIGHTNESS = 0,
	ALPHA = 1,
	LUT_MAX,
};

static const u8 brightness_alpha_lut[20][LUT_MAX] = {
/* {brightness, alpha} */
	{0, 0xFF},
	{1, 0xEB},
	{2, 0xE7},
	{3, 0xE0},
	{4, 0xD5},
	{5, 0xD3},
	{6, 0xD0},
	{8, 0xCE},
	{10, 0xCB},
	{15, 0xC8},
	{20, 0xC4},
	{30, 0xBA},
	{45, 0xB0},
	{70, 0xA0},
	{90, 0x8B},
	{120, 0x72},
	{150, 0x5A},
	{180, 0x38},
	{210, 0x0E},
	{240, 0x00}
};

u32 expo_calc_backlight(u32 bl_lvl);

#endif /* SDE_EXPO_DIM_LAYER_H */
