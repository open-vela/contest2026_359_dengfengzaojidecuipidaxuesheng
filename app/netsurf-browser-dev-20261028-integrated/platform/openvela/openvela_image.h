/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_IMAGE_H
#define NETSURF_OPENVELA_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include "utils/errors.h"
struct bitmap;
struct bitmap *ov_image_decode(const uint8_t *data, size_t size);
nserror ov_image_register(void);
#endif
