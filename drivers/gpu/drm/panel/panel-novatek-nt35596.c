// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Amarula Solutions
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#define NT35596_CMD_LEN			2

struct nt35596_panel_desc {
	const struct drm_display_mode	*mode;
	unsigned int			lanes;
	unsigned long			flags;
	enum mipi_dsi_pixel_format	format;
	const struct nt35596_init_cmd	*panel_cmds;
	unsigned int			num_panel_cmds;
};

struct nt35596 {
	struct drm_panel		panel;
	struct mipi_dsi_device		*dsi;
	const struct nt35596_panel_desc *desc;

	struct backlight_device		*backlight;
	struct regulator		*dvdd;
	struct regulator		*avdd;
	struct regulator		*avee;
	struct gpio_desc		*reset;
};

static inline struct nt35596 *panel_to_nt35596(struct drm_panel *panel)
{
	return container_of(panel, struct nt35596, panel);
}

struct nt35596_init_cmd {
	u8 data[NT35596_CMD_LEN];
};

static const struct nt35596_init_cmd microtech_mtf050fhdi_cmds[] = {
	{ .data = { 0xFF, 0xEE } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x1F, 0x45 } },
	{ .data = { 0x24, 0x4F } },
	{ .data = { 0x38, 0xC8 } },
	{ .data = { 0x39, 0x2C } },
	{ .data = { 0x1E, 0xBB } },
	{ .data = { 0x1D, 0x0F } },
	{ .data = { 0x7E, 0xB1 } },

	/* Enter CMD1, Turn-on Tear ON */
	{ .data = { 0xFF, 0x00 } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x35, 0x01 } },
	{ .data = { 0xBA, 0x03 } },

	/* CMD2 Page0 */
	{ .data = { 0xFF, 0x01 } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x00, 0x01 } },
	{ .data = { 0x01, 0x55 } },
	{ .data = { 0x02, 0x40 } },
	{ .data = { 0x05, 0x00 } },
	{ .data = { 0x06, 0x1B } },
	{ .data = { 0x07, 0x24 } },
	{ .data = { 0x08, 0x0C } },
	{ .data = { 0x0B, 0x87 } },
	{ .data = { 0x0C, 0x87 } },
	{ .data = { 0x0E, 0xB0 } },
	{ .data = { 0x0F, 0xB3 } },
	{ .data = { 0x11, 0x10 } },
	{ .data = { 0x12, 0x10 } },
	{ .data = { 0x13, 0x05 } },
	{ .data = { 0x14, 0x4A } },
	{ .data = { 0x15, 0x18 } },
	{ .data = { 0x16, 0x18 } },
	{ .data = { 0x18, 0x00 } },
	{ .data = { 0x19, 0x77 } },
	{ .data = { 0x1A, 0x55 } },
	{ .data = { 0x1B, 0x13 } },
	{ .data = { 0x1C, 0x00 } },
	{ .data = { 0x1D, 0x00 } },
	{ .data = { 0x1E, 0x13 } },
	{ .data = { 0x1F, 0x00 } },
	{ .data = { 0x23, 0x00 } },
	{ .data = { 0x24, 0x00 } },
	{ .data = { 0x25, 0x00 } },
	{ .data = { 0x26, 0x00 } },
	{ .data = { 0x27, 0x00 } },
	{ .data = { 0x28, 0x00 } },
	{ .data = { 0x35, 0x00 } },
	{ .data = { 0x66, 0x00 } },
	{ .data = { 0x58, 0x82 } },
	{ .data = { 0x59, 0x02 } },
	{ .data = { 0x5A, 0x02 } },
	{ .data = { 0x5B, 0x02 } },
	{ .data = { 0x5C, 0x82 } },
	{ .data = { 0x5D, 0x82 } },
	{ .data = { 0x5E, 0x02 } },
	{ .data = { 0x5F, 0x02 } },
	{ .data = { 0x72, 0x31 } },

	/* CMD2 Page4 */
	{ .data = { 0xFF, 0x05 } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x00, 0x01 } },
	{ .data = { 0x01, 0x0B } },
	{ .data = { 0x02, 0x0C } },
	{ .data = { 0x03, 0x09 } },
	{ .data = { 0x04, 0x0A } },
	{ .data = { 0x05, 0x00 } },
	{ .data = { 0x06, 0x0F } },
	{ .data = { 0x07, 0x10 } },
	{ .data = { 0x08, 0x00 } },
	{ .data = { 0x09, 0x00 } },
	{ .data = { 0x0A, 0x00 } },
	{ .data = { 0x0B, 0x00 } },
	{ .data = { 0x0C, 0x00 } },
	{ .data = { 0x0D, 0x13 } },
	{ .data = { 0x0E, 0x15 } },
	{ .data = { 0x0F, 0x17 } },
	{ .data = { 0x10, 0x01 } },
	{ .data = { 0x11, 0x0B } },
	{ .data = { 0x12, 0x0C } },
	{ .data = { 0x13, 0x09 } },
	{ .data = { 0x14, 0x0A } },
	{ .data = { 0x15, 0x00 } },
	{ .data = { 0x16, 0x0F } },
	{ .data = { 0x17, 0x10 } },
	{ .data = { 0x18, 0x00 } },
	{ .data = { 0x19, 0x00 } },
	{ .data = { 0x1A, 0x00 } },
	{ .data = { 0x1B, 0x00 } },
	{ .data = { 0x1C, 0x00 } },
	{ .data = { 0x1D, 0x13 } },
	{ .data = { 0x1E, 0x15 } },
	{ .data = { 0x1F, 0x17 } },
	{ .data = { 0x20, 0x00 } },
	{ .data = { 0x21, 0x03 } },
	{ .data = { 0x22, 0x01 } },
	{ .data = { 0x23, 0x40 } },
	{ .data = { 0x24, 0x40 } },
	{ .data = { 0x25, 0xED } },
	{ .data = { 0x29, 0x58 } },
	{ .data = { 0x2A, 0x12 } },
	{ .data = { 0x2B, 0x01 } },
	{ .data = { 0x4B, 0x06 } },
	{ .data = { 0x4C, 0x11 } },
	{ .data = { 0x4D, 0x20 } },
	{ .data = { 0x4E, 0x02 } },
	{ .data = { 0x4F, 0x02 } },
	{ .data = { 0x50, 0x20 } },
	{ .data = { 0x51, 0x61 } },
	{ .data = { 0x52, 0x01 } },
	{ .data = { 0x53, 0x63 } },
	{ .data = { 0x54, 0x77 } },
	{ .data = { 0x55, 0xED } },
	{ .data = { 0x5B, 0x00 } },
	{ .data = { 0x5C, 0x00 } },
	{ .data = { 0x5D, 0x00 } },
	{ .data = { 0x5E, 0x00 } },
	{ .data = { 0x5F, 0x15 } },
	{ .data = { 0x60, 0x75 } },
	{ .data = { 0x61, 0x00 } },
	{ .data = { 0x62, 0x00 } },
	{ .data = { 0x63, 0x00 } },
	{ .data = { 0x64, 0x00 } },
	{ .data = { 0x65, 0x00 } },
	{ .data = { 0x66, 0x00 } },
	{ .data = { 0x67, 0x00 } },
	{ .data = { 0x68, 0x04 } },
	{ .data = { 0x69, 0x00 } },
	{ .data = { 0x6A, 0x00 } },
	{ .data = { 0x6C, 0x40 } },
	{ .data = { 0x75, 0x01 } },
	{ .data = { 0x76, 0x01 } },
	{ .data = { 0x7A, 0x80 } },
	{ .data = { 0x7B, 0xC5 } },
	{ .data = { 0x7C, 0xD8 } },
	{ .data = { 0x7D, 0x60 } },
	{ .data = { 0x7F, 0x10 } },
	{ .data = { 0x80, 0x81 } },
	{ .data = { 0x83, 0x05 } },
	{ .data = { 0x93, 0x08 } },
	{ .data = { 0x94, 0x10 } },
	{ .data = { 0x8A, 0x00 } },
	{ .data = { 0x9B, 0x0F } },
	{ .data = { 0xEA, 0xFF } },
	{ .data = { 0xEC, 0x00 } },

	/* CMD2 Page0 */
	{ .data = { 0xFF, 0x01 } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x75, 0x00 } },
	{ .data = { 0x76, 0x8E } },
	{ .data = { 0x77, 0x00 } },
	{ .data = { 0x78, 0x90 } },
	{ .data = { 0x79, 0x00 } },
	{ .data = { 0x7A, 0xB2 } },
	{ .data = { 0x7B, 0x00 } },
	{ .data = { 0x7C, 0xC7 } },
	{ .data = { 0x7D, 0x00 } },
	{ .data = { 0x7E, 0xD7 } },
	{ .data = { 0x7F, 0x00 } },
	{ .data = { 0x80, 0xE9 } },
	{ .data = { 0x81, 0x00 } },
	{ .data = { 0x82, 0xF9 } },
	{ .data = { 0x83, 0x01 } },
	{ .data = { 0x84, 0x01 } },
	{ .data = { 0x85, 0x01 } },
	{ .data = { 0x86, 0x0B } },
	{ .data = { 0x87, 0x01 } },
	{ .data = { 0x88, 0x3A } },
	{ .data = { 0x89, 0x01 } },
	{ .data = { 0x8A, 0x5D } },
	{ .data = { 0x8B, 0x01 } },
	{ .data = { 0x8C, 0x94 } },
	{ .data = { 0x8D, 0x01 } },
	{ .data = { 0x8E, 0xBC } },
	{ .data = { 0x8F, 0x02 } },
	{ .data = { 0x90, 0x00 } },
	{ .data = { 0x91, 0x02 } },
	{ .data = { 0x92, 0x39 } },
	{ .data = { 0x93, 0x02 } },
	{ .data = { 0x94, 0x3A } },
	{ .data = { 0x95, 0x02 } },
	{ .data = { 0x96, 0x6B } },
	{ .data = { 0x97, 0x02 } },
	{ .data = { 0x98, 0xA2 } },
	{ .data = { 0x99, 0x02 } },
	{ .data = { 0x9A, 0xC7 } },
	{ .data = { 0x9B, 0x02 } },
	{ .data = { 0x9C, 0xFB } },
	{ .data = { 0x9D, 0x03 } },
	{ .data = { 0x9E, 0x20 } },
	{ .data = { 0x9F, 0x03 } },
	{ .data = { 0xA0, 0x54 } },
	{ .data = { 0xA2, 0x03 } },
	{ .data = { 0xA3, 0x6D } },
	{ .data = { 0xA4, 0x03 } },
	{ .data = { 0xA5, 0x80 } },
	{ .data = { 0xA6, 0x03 } },
	{ .data = { 0xA7, 0x81 } },
	{ .data = { 0xA9, 0x03 } },
	{ .data = { 0xAA, 0xC7 } },
	{ .data = { 0xAB, 0x03 } },
	{ .data = { 0xAC, 0xF0 } },
	{ .data = { 0xAD, 0x03 } },
	{ .data = { 0xAE, 0xF8 } },
	{ .data = { 0xAF, 0x03 } },
	{ .data = { 0xB0, 0xFD } },
	{ .data = { 0xB1, 0x03 } },
	{ .data = { 0xB2, 0xFE } },
	{ .data = { 0xB3, 0x00 } },
	{ .data = { 0xB4, 0x8E } },
	{ .data = { 0xB5, 0x00 } },
	{ .data = { 0xB6, 0x90 } },
	{ .data = { 0xB7, 0x00 } },
	{ .data = { 0xB8, 0xB2 } },
	{ .data = { 0xB9, 0x00 } },
	{ .data = { 0xBA, 0xC7 } },
	{ .data = { 0xBB, 0x00 } },
	{ .data = { 0xBC, 0xD7 } },
	{ .data = { 0xBD, 0x00 } },
	{ .data = { 0xBE, 0xE9 } },
	{ .data = { 0xBF, 0x00 } },
	{ .data = { 0xC0, 0xF9 } },
	{ .data = { 0xC1, 0x01 } },
	{ .data = { 0xC2, 0x01 } },
	{ .data = { 0xC3, 0x01 } },
	{ .data = { 0xC4, 0x0B } },
	{ .data = { 0xC5, 0x01 } },
	{ .data = { 0xC6, 0x3A } },
	{ .data = { 0xC7, 0x01 } },
	{ .data = { 0xC8, 0x5D } },
	{ .data = { 0xC9, 0x01 } },
	{ .data = { 0xCA, 0x94 } },
	{ .data = { 0xCB, 0x01 } },
	{ .data = { 0xCC, 0xBC } },
	{ .data = { 0xCD, 0x02 } },
	{ .data = { 0xCE, 0x00 } },
	{ .data = { 0xCF, 0x02 } },
	{ .data = { 0xD0, 0x39 } },
	{ .data = { 0xD1, 0x02 } },
	{ .data = { 0xD2, 0x3A } },
	{ .data = { 0xD3, 0x02 } },
	{ .data = { 0xD4, 0x6B } },
	{ .data = { 0xD5, 0x02 } },
	{ .data = { 0xD6, 0xA2 } },
	{ .data = { 0xD7, 0x02 } },
	{ .data = { 0xD8, 0xC7 } },
	{ .data = { 0xD9, 0x02 } },
	{ .data = { 0xDA, 0xFB } },
	{ .data = { 0xDB, 0x03 } },
	{ .data = { 0xDC, 0x20 } },
	{ .data = { 0xDD, 0x03 } },
	{ .data = { 0xDE, 0x54 } },
	{ .data = { 0xDF, 0x03 } },
	{ .data = { 0xE0, 0x6D } },
	{ .data = { 0xE1, 0x03 } },
	{ .data = { 0xE2, 0x80 } },
	{ .data = { 0xE3, 0x03 } },
	{ .data = { 0xE4, 0x81 } },
	{ .data = { 0xE5, 0x03 } },
	{ .data = { 0xE6, 0xC7 } },
	{ .data = { 0xE7, 0x03 } },
	{ .data = { 0xE8, 0xF0 } },
	{ .data = { 0xE9, 0x03 } },
	{ .data = { 0xEA, 0xF8 } },
	{ .data = { 0xEB, 0x03 } },
	{ .data = { 0xEC, 0xFD } },
	{ .data = { 0xED, 0x03 } },
	{ .data = { 0xEE, 0xFE } },
	{ .data = { 0xEF, 0x00 } },
	{ .data = { 0xF0, 0x03 } },
	{ .data = { 0xF1, 0x00 } },
	{ .data = { 0xF2, 0x0B } },
	{ .data = { 0xF3, 0x00 } },
	{ .data = { 0xF4, 0x0D } },
	{ .data = { 0xF5, 0x00 } },
	{ .data = { 0xF6, 0x4A } },
	{ .data = { 0xF7, 0x00 } },
	{ .data = { 0xF8, 0x71 } },
	{ .data = { 0xF9, 0x00 } },
	{ .data = { 0xFA, 0x8C } },

	/* CMD2 Page1 */
	{ .data = { 0xFF, 0x02 } },
	{ .data = { 0xFB, 0x01 } },
	{ .data = { 0x00, 0x00 } },
	{ .data = { 0x01, 0xA1 } },
	{ .data = { 0x02, 0x00 } },
	{ .data = { 0x03, 0xB6 } },
	{ .data = { 0x04, 0x00 } },
	{ .data = { 0x05, 0xC9 } },
	{ .data = { 0x06, 0x00 } },
	{ .data = { 0x07, 0xFD } },
	{ .data = { 0x08, 0x01 } },
	{ .data = { 0x09, 0x29 } },
	{ .data = { 0x0A, 0x01 } },
	{ .data = { 0x0B, 0x6B } },
	{ .data = { 0x0C, 0x01 } },
	{ .data = { 0x0D, 0x9E } },
	{ .data = { 0x0E, 0x01 } },
	{ .data = { 0x0F, 0xEB } },
	{ .data = { 0x10, 0x02 } },
	{ .data = { 0x11, 0x25 } },
	{ .data = { 0x12, 0x02 } },
	{ .data = { 0x13, 0x27 } },
	{ .data = { 0x14, 0x02 } },
	{ .data = { 0x15, 0x5C } },
	{ .data = { 0x16, 0x02 } },
	{ .data = { 0x17, 0x95 } },
	{ .data = { 0x18, 0x02 } },
	{ .data = { 0x19, 0xBA } },
	{ .data = { 0x1A, 0x02 } },
	{ .data = { 0x1B, 0xEC } },
	{ .data = { 0x1C, 0x03 } },
	{ .data = { 0x1D, 0x0C } },
	{ .data = { 0x1E, 0x03 } },
	{ .data = { 0x1F, 0x34 } },
	{ .data = { 0x20, 0x03 } },
	{ .data = { 0x21, 0x3F } },
	{ .data = { 0x22, 0x03 } },
	{ .data = { 0x23, 0x48 } },
	{ .data = { 0x24, 0x03 } },
	{ .data = { 0x25, 0x49 } },
	{ .data = { 0x26, 0x03 } },
	{ .data = { 0x27, 0x6B } },
	{ .data = { 0x28, 0x03 } },
	{ .data = { 0x29, 0x7E } },
	{ .data = { 0x2A, 0x03 } },
	{ .data = { 0x2B, 0x8F } },
	{ .data = { 0x2D, 0x03 } },
	{ .data = { 0x2F, 0x9E } },
	{ .data = { 0x30, 0x03 } },
	{ .data = { 0x31, 0xA0 } },
	{ .data = { 0x32, 0x00 } },
	{ .data = { 0x33, 0x03 } },
	{ .data = { 0x34, 0x00 } },
	{ .data = { 0x35, 0x0B } },
	{ .data = { 0x36, 0x00 } },
	{ .data = { 0x37, 0x0D } },
	{ .data = { 0x38, 0x00 } },
	{ .data = { 0x39, 0x4A } },
	{ .data = { 0x3A, 0x00 } },
	{ .data = { 0x3B, 0x71 } },
	{ .data = { 0x3D, 0x00 } },
	{ .data = { 0x3F, 0x8C } },
	{ .data = { 0x40, 0x00 } },
	{ .data = { 0x41, 0xA1 } },
	{ .data = { 0x42, 0x00 } },
	{ .data = { 0x43, 0xB6 } },
	{ .data = { 0x44, 0x00 } },
	{ .data = { 0x45, 0xC9 } },
	{ .data = { 0x46, 0x00 } },
	{ .data = { 0x47, 0xFD } },
	{ .data = { 0x48, 0x01 } },
	{ .data = { 0x49, 0x29 } },
	{ .data = { 0x4A, 0x01 } },
	{ .data = { 0x4B, 0x6B } },
	{ .data = { 0x4C, 0x01 } },
	{ .data = { 0x4D, 0x9E } },
	{ .data = { 0x4E, 0x01 } },
	{ .data = { 0x4F, 0xEB } },
	{ .data = { 0x50, 0x02 } },
	{ .data = { 0x51, 0x25 } },
	{ .data = { 0x52, 0x02 } },
	{ .data = { 0x53, 0x27 } },
	{ .data = { 0x54, 0x02 } },
	{ .data = { 0x55, 0x5C } },
	{ .data = { 0x56, 0x02 } },
	{ .data = { 0x58, 0x95 } },
	{ .data = { 0x59, 0x02 } },
	{ .data = { 0x5A, 0xBA } },
	{ .data = { 0x5B, 0x02 } },
	{ .data = { 0x5C, 0xEC } },
	{ .data = { 0x5D, 0x03 } },
	{ .data = { 0x5E, 0x0C } },
	{ .data = { 0x5F, 0x03 } },
	{ .data = { 0x60, 0x34 } },
	{ .data = { 0x61, 0x03 } },
	{ .data = { 0x62, 0x3F } },
	{ .data = { 0x63, 0x03 } },
	{ .data = { 0x64, 0x48 } },
	{ .data = { 0x65, 0x03 } },
	{ .data = { 0x66, 0x49 } },
	{ .data = { 0x67, 0x03 } },
	{ .data = { 0x68, 0x6B } },
	{ .data = { 0x69, 0x03 } },
	{ .data = { 0x6A, 0x7E } },
	{ .data = { 0x6B, 0x03 } },
	{ .data = { 0x6C, 0x8F } },
	{ .data = { 0x6D, 0x03 } },
	{ .data = { 0x6E, 0x9E } },
	{ .data = { 0x6F, 0x03 } },
	{ .data = { 0x70, 0xA0 } },
	{ .data = { 0x71, 0x00 } },
	{ .data = { 0x72, 0xFB } },
	{ .data = { 0x73, 0x00 } },
	{ .data = { 0x74, 0xFD } },
	{ .data = { 0x75, 0x01 } },
	{ .data = { 0x76, 0x05 } },
	{ .data = { 0x77, 0x01 } },
	{ .data = { 0x78, 0x0D } },
	{ .data = { 0x79, 0x01 } },
	{ .data = { 0x7A, 0x17 } },
	{ .data = { 0x7B, 0x01 } },
	{ .data = { 0x7C, 0x1F } },
	{ .data = { 0x7D, 0x01 } },
	{ .data = { 0x7E, 0x28 } },
	{ .data = { 0x7F, 0x01 } },
	{ .data = { 0x80, 0x32 } },
	{ .data = { 0x81, 0x01 } },
	{ .data = { 0x82, 0x38 } },
	{ .data = { 0x83, 0x01 } },
	{ .data = { 0x84, 0x53 } },
	{ .data = { 0x85, 0x01 } },
	{ .data = { 0x86, 0x72 } },
	{ .data = { 0x87, 0x01 } },
	{ .data = { 0x88, 0x9B } },
	{ .data = { 0x89, 0x01 } },
	{ .data = { 0x8A, 0xC3 } },
	{ .data = { 0x8B, 0x02 } },
	{ .data = { 0x8C, 0x01 } },
	{ .data = { 0x8D, 0x02 } },
	{ .data = { 0x8E, 0x36 } },
	{ .data = { 0x8F, 0x02 } },
	{ .data = { 0x90, 0x37 } },
	{ .data = { 0x91, 0x02 } },
	{ .data = { 0x92, 0x69 } },
	{ .data = { 0x93, 0x02 } },
	{ .data = { 0x94, 0xA1 } },
	{ .data = { 0x95, 0x02 } },
	{ .data = { 0x96, 0xC8 } },
	{ .data = { 0x97, 0x02 } },
	{ .data = { 0x98, 0xFF } },
	{ .data = { 0x99, 0x03 } },
	{ .data = { 0x9A, 0x26 } },
	{ .data = { 0x9B, 0x03 } },
	{ .data = { 0x9C, 0x69 } },
	{ .data = { 0x9D, 0x03 } },
	{ .data = { 0x9E, 0x88 } },
	{ .data = { 0x9F, 0x03 } },
	{ .data = { 0xA0, 0xF8 } },
	{ .data = { 0xA2, 0x03 } },
	{ .data = { 0xA3, 0xF9 } },
	{ .data = { 0xA4, 0x03 } },
	{ .data = { 0xA5, 0xFE } },
	{ .data = { 0xA6, 0x03 } },
	{ .data = { 0xA7, 0xFE } },
	{ .data = { 0xA9, 0x03 } },
	{ .data = { 0xAA, 0xFE } },
	{ .data = { 0xAB, 0x03 } },
	{ .data = { 0xAC, 0xFE } },
	{ .data = { 0xAD, 0x03 } },
	{ .data = { 0xAE, 0xFE } },
	{ .data = { 0xAF, 0x00 } },
	{ .data = { 0xB0, 0xFB } },
	{ .data = { 0xB1, 0x00 } },
	{ .data = { 0xB2, 0xFD } },
	{ .data = { 0xB3, 0x01 } },
	{ .data = { 0xB4, 0x05 } },
	{ .data = { 0xB5, 0x01 } },
	{ .data = { 0xB6, 0x0D } },
	{ .data = { 0xB7, 0x01 } },
	{ .data = { 0xB8, 0x17 } },
	{ .data = { 0xB9, 0x01 } },
	{ .data = { 0xBA, 0x1F } },
	{ .data = { 0xBB, 0x01 } },
	{ .data = { 0xBC, 0x28 } },
	{ .data = { 0xBD, 0x01 } },
	{ .data = { 0xBE, 0x32 } },
	{ .data = { 0xBF, 0x01 } },
	{ .data = { 0xC0, 0x38 } },
	{ .data = { 0xC1, 0x01 } },
	{ .data = { 0xC2, 0x53 } },
	{ .data = { 0xC3, 0x01 } },
	{ .data = { 0xC4, 0x72 } },
	{ .data = { 0xC5, 0x01 } },
	{ .data = { 0xC6, 0x9B } },
	{ .data = { 0xC7, 0x01 } },
	{ .data = { 0xC8, 0xC3 } },
	{ .data = { 0xC9, 0x02 } },
	{ .data = { 0xCA, 0x01 } },
	{ .data = { 0xCB, 0x02 } },
	{ .data = { 0xCC, 0x36 } },
	{ .data = { 0xCD, 0x02 } },
	{ .data = { 0xCE, 0x37 } },
	{ .data = { 0xCF, 0x02 } },
	{ .data = { 0xD0, 0x69 } },
	{ .data = { 0xD1, 0x02 } },
	{ .data = { 0xD2, 0xA1 } },
	{ .data = { 0xD3, 0x02 } },
	{ .data = { 0xD4, 0xC8 } },
	{ .data = { 0xD5, 0x02 } },
	{ .data = { 0xD6, 0xFF } },
	{ .data = { 0xD7, 0x03 } },
	{ .data = { 0xD8, 0x26 } },
	{ .data = { 0xD9, 0x03 } },
	{ .data = { 0xDA, 0x69 } },
	{ .data = { 0xDB, 0x03 } },
	{ .data = { 0xDC, 0x88 } },
	{ .data = { 0xDD, 0x03 } },
	{ .data = { 0xDE, 0xF8 } },
	{ .data = { 0xDF, 0x03 } },
	{ .data = { 0xE0, 0xF9 } },
	{ .data = { 0xE1, 0x03 } },
	{ .data = { 0xE2, 0xFE } },
	{ .data = { 0xE3, 0x03 } },
	{ .data = { 0xE4, 0xFE } },
	{ .data = { 0xE5, 0x03 } },
	{ .data = { 0xE6, 0xFE } },
	{ .data = { 0xE7, 0x03 } },
	{ .data = { 0xE8, 0xFE } },
	{ .data = { 0xE9, 0x03 } },
	{ .data = { 0xEA, 0xFE } },

	/* CMD1, VBP/VFP settings */
	{ .data = { 0xFF, 0x00 } },
	{ .data = { 0xD3, 0x14 } },
	{ .data = { 0xD4, 0x14 } },

	/* Exit CMD1, Turn-off Tear ON */
	{ .data = { 0xFF, 0x00 } },
	{ .data = { 0x35, 0x00 } },
};

static int nt35596_power_on(struct nt35596 *nt35596)
{
	int ret;

	ret = regulator_enable(nt35596->dvdd);
	if (ret)
		return ret;

	/* T_power_ramp_up for VDDI */
	msleep(2);

	ret = regulator_enable(nt35596->avdd);
	if (ret)
		return ret;

	/* T_power_ramp_up for AVDD/AVEE */
	msleep(5);

	ret = regulator_enable(nt35596->avee);
	if (ret)
		return ret;

	msleep(10);

	gpiod_set_value(nt35596->reset, 0);

	msleep(120);

	gpiod_set_value(nt35596->reset, 1);

	return 0;
}

static int nt35596_power_off(struct nt35596 *nt35596)
{
	gpiod_set_value(nt35596->reset, 0);

	msleep(10);

	regulator_disable(nt35596->avee);

	/* T_power_ramp_down for AVEE/AVDD */
	msleep(5);

	regulator_disable(nt35596->avdd);

	/* T_power_ramp_down for VDDI */
	msleep(2);

	regulator_disable(nt35596->dvdd);

	return 0;
}

static int nt35596_prepare(struct drm_panel *panel)
{
	struct nt35596 *nt35596 = panel_to_nt35596(panel);
	struct mipi_dsi_device *dsi = nt35596->dsi;
	const struct nt35596_panel_desc *desc = nt35596->desc;
	int ret, i;

	ret = nt35596_power_on(nt35596);
	if (ret)
		return ret;

	msleep(120);

	for (i = 0; i < desc->num_panel_cmds; i++) {
		const struct nt35596_init_cmd *cmd = &desc->panel_cmds[i];

		ret = mipi_dsi_dcs_write_buffer(dsi, cmd->data,
						NT35596_CMD_LEN);
		if (ret < 0) {
			DRM_DEV_ERROR(panel->dev,
				      "failed to write cmd %d: %d\n", i, ret);
			goto power_off;
		}
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			      "failed to exit from sleep mode: %d\n", ret);
		goto power_off;
	}

	return 0;

power_off:
	if (nt35596_power_off(nt35596))
		DRM_DEV_ERROR(panel->dev, "failed to power off\n");
	return ret;
}

static int nt35596_enable(struct drm_panel *panel)
{
	struct nt35596 *nt35596 = panel_to_nt35596(panel);

	msleep(120);

	mipi_dsi_dcs_set_display_on(nt35596->dsi);
	backlight_enable(nt35596->backlight);

	return 0;
}

static int nt35596_disable(struct drm_panel *panel)
{
	struct nt35596 *nt35596 = panel_to_nt35596(panel);

	backlight_disable(nt35596->backlight);
	return mipi_dsi_dcs_set_display_off(nt35596->dsi);
}

static int nt35596_unprepare(struct drm_panel *panel)
{
	struct nt35596 *nt35596 = panel_to_nt35596(panel);

	mipi_dsi_dcs_enter_sleep_mode(nt35596->dsi);

	msleep(120);

	nt35596_power_off(nt35596);

	return 0;
}

static int nt35596_get_modes(struct drm_panel *panel)
{
	struct nt35596 *nt35596 = panel_to_nt35596(panel);
	const struct drm_display_mode *desc_mode = nt35596->desc->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, desc_mode);
	if (!mode) {
		DRM_DEV_ERROR(&nt35596->dsi->dev,
			      "failed to add mode %ux%ux@%u\n",
			      desc_mode->hdisplay,
			      desc_mode->vdisplay,
			      desc_mode->vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = desc_mode->width_mm;
	panel->connector->display_info.height_mm = desc_mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs nt35596_funcs = {
	.disable	= nt35596_disable,
	.unprepare	= nt35596_unprepare,
	.prepare	= nt35596_prepare,
	.enable		= nt35596_enable,
	.get_modes	= nt35596_get_modes,
};

static const struct drm_display_mode microtech_mtf050fhdi_mode = {
	.clock		= 147000,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 408,
	.hsync_end	= 1080 + 408 + 4,
	.htotal		= 1080 + 408 + 4 + 38,

	.vdisplay	= 1920,
	.vsync_start	= 1920 + 9,
	.vsync_end	= 1920 + 9 + 12,
	.vtotal		= 1920 + 9 + 12 + 9,
	.vrefresh	= 50,

	.width_mm	= 64,
	.height_mm	= 118,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct nt35596_panel_desc microtech_mtf050fhdi_desc = {
	.mode		= &microtech_mtf050fhdi_mode,
	.lanes		= 4,
	.flags		= MIPI_DSI_MODE_VIDEO_BURST,
	.format		= MIPI_DSI_FMT_RGB888,
	.panel_cmds	= microtech_mtf050fhdi_cmds,
	.num_panel_cmds	= ARRAY_SIZE(microtech_mtf050fhdi_cmds),
};

static int nt35596_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct nt35596_panel_desc *desc;
	struct nt35596 *nt35596;
	int ret;

	nt35596 = devm_kzalloc(&dsi->dev, sizeof(*nt35596), GFP_KERNEL);
	if (!nt35596)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	drm_panel_init(&nt35596->panel);
	nt35596->panel.dev = &dsi->dev;
	nt35596->panel.funcs = &nt35596_funcs;

	nt35596->dvdd = devm_regulator_get(&dsi->dev, "dvdd");
	if (IS_ERR(nt35596->dvdd)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get dvdd regulator\n");
		return PTR_ERR(nt35596->dvdd);
	}

	nt35596->avdd = devm_regulator_get(&dsi->dev, "avdd");
	if (IS_ERR(nt35596->avdd)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get avdd regulator\n");
		return PTR_ERR(nt35596->avdd);
	}

	nt35596->avee = devm_regulator_get(&dsi->dev, "avee");
	if (IS_ERR(nt35596->avee)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get avee regulator\n");
		return PTR_ERR(nt35596->avee);
	}

	nt35596->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(nt35596->reset)) {
		DRM_DEV_ERROR(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(nt35596->reset);
	}

	nt35596->backlight = devm_of_find_backlight(&dsi->dev);
	if (IS_ERR(nt35596->backlight))
		return PTR_ERR(nt35596->backlight);

	ret = drm_panel_add(&nt35596->panel);
	if (ret < 0)
		return ret;

	mipi_dsi_set_drvdata(dsi, nt35596);
	nt35596->dsi = dsi;
	nt35596->desc = desc;

	return mipi_dsi_attach(dsi);
}

static int nt35596_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct nt35596 *nt35596 = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&nt35596->panel);

	return 0;
}

static const struct of_device_id nt35596_of_match[] = {
	{
		.compatible = "microtech,mtf050fhdi-03",
		.data = &microtech_mtf050fhdi_desc,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt35596_of_match);

static struct mipi_dsi_driver nt35596_driver = {
	.probe = nt35596_dsi_probe,
	.remove = nt35596_dsi_remove,
	.driver = {
		.name = "nt35596-nt35596",
		.of_match_table = nt35596_of_match,
	},
};
module_mipi_dsi_driver(nt35596_driver);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Novatek NT35596 MIPI-DSI LCD panel");
MODULE_LICENSE("GPL");
