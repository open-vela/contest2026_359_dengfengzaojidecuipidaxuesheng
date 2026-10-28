/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_cache.h"
bool ov_browser_cache_prepare(uintptr_t start, size_t bytes, size_t line,
	ov_browser_cache_sync_fn sync, void *ctx)
{
	/* The allocation owns whole cache lines, so invalidation cannot discard
	 * a neighbouring allocation's dirty bytes. The module linker enforces it. */
	if (!sync || !start || !bytes || !line || line > 4096 || (line & (line-1)) ||
		(start & (line-1)) || (bytes & (line-1)) || bytes > UINTPTR_MAX-start)
		return false;
	for (unsigned phase = 0; phase < 2; phase++) {
		for (size_t offset = 0; offset < bytes;) {
			size_t chunk = bytes-offset < 4096 ? bytes-offset : 4096;
			if (!sync(ctx, start+offset, chunk, phase != 0)) return false;
			offset += chunk;
		}
	}
	return true;
}
