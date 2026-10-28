/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OV_BROWSER_CACHE_H
#define OV_BROWSER_CACHE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*ov_browser_cache_sync_fn)(void *, uintptr_t, size_t, bool);
bool ov_browser_cache_prepare(uintptr_t start, size_t bytes, size_t line,
	ov_browser_cache_sync_fn sync, void *ctx);
#endif
