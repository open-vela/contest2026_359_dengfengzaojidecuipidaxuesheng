/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_READING_H
#define NETSURF_OPENVELA_READING_H
#include <stdbool.h>

/* Optional portable-core presentation extension, implemented by patch 0060.
 * NetSurf owner thread only. Fetching, intrinsic sizes and layout are retained.
 * This interface is internal to the browser module, not the firmware loader. */
void nsw_content_set_images_visible(bool visible);
struct hlcache_handle;
/* Reversible article presentation on the original DOM; no script evaluation. */
bool ov_reading_apply(struct hlcache_handle *content, bool enabled,
		      bool *available);

#endif
