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

static const u8 brightness_alpha_lut[17][LUT_MAX] = {
/* {brightness, alpha} */
	{0, 0xff},
	{1, 0xE0},
	{2, 0xd5},
	{3, 0xd3},
	{4, 0xd0},
	{5, 0xce},
	{6, 0xcb},
	{8, 0xc8},
	{10, 0xc4},
	{15, 0xba},
	{20, 0xb0},
	{30, 0xa0},
	{45, 0x8b},
	{70, 0x72},
	{100, 0x5a},
	{150, 0x38},
	{227, 0xe},
	{260, 0x00}
};

#endif /* SDE_EXPO_DIM_LAYER_H */
