/* SPDX-License-Identifier: GPL-2.0-only */
/* Included only by the namespaced webpage backend (QuickJS-ng ABI). */
#ifndef NETSURF_OPENVELA_WEB_ALLOC_H
#define NETSURF_OPENVELA_WEB_ALLOC_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <quickjs/quickjs.h>

union ov_web_block { max_align_t align; size_t size; };
static void *ov_web_malloc(void *opaque, size_t size)
{
	(void)opaque;
	if (size > SIZE_MAX - sizeof(union ov_web_block)) return NULL;
	union ov_web_block *b = malloc(sizeof(*b) + size);
	if (!b) return NULL;
	b->size = size; return b + 1;
}
static void ov_web_free(void *opaque, void *ptr)
{ (void)opaque; if (ptr) free((union ov_web_block *)ptr - 1); }
static size_t ov_web_usable(const void *ptr)
{ return ptr ? ((const union ov_web_block *)ptr - 1)->size : 0; }
static void *ov_web_calloc(void *opaque, size_t count, size_t size)
{
	if (count && size > SIZE_MAX / count) return NULL;
	void *ptr = ov_web_malloc(opaque, count * size);
	if (ptr) memset(ptr, 0, count * size);
	return ptr;
}
static void *ov_web_realloc(void *opaque, void *ptr, size_t size)
{
	if (!ptr) return ov_web_malloc(opaque, size);
	if (!size) { ov_web_free(opaque, ptr); return NULL; }
	if (size > SIZE_MAX - sizeof(union ov_web_block)) return NULL;
	union ov_web_block *b = realloc((union ov_web_block *)ptr - 1, sizeof(*b) + size);
	if (!b) return NULL;
	b->size = size; return b + 1;
}
static const JSMallocFunctions ov_web_alloc_functions = {
	ov_web_calloc, ov_web_malloc, ov_web_free, ov_web_realloc, ov_web_usable
};
#endif
