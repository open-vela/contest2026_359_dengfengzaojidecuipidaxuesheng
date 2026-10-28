/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_BITMAP_H
#define NETSURF_OPENVELA_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/bitmap.h"

struct bitmap {
	uint8_t *pixels;
	int width;
	int height;
	size_t stride;
	bool opaque;
	uint32_t version;
};

extern struct gui_bitmap_table ov_bitmap_table;

#endif
