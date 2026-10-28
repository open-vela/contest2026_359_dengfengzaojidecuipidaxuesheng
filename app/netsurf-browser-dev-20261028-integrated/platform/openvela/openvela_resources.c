/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "openvela_resources.h"
#include "netsurf/browser.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "nsw_resources.h"

static bool options_ready;
static const char *filetype(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot) return "text/plain";
	static const struct { const char *extension, *mime; } types[] = {
		{".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
		{".svg", "image/svg+xml"}, {".png", "image/png"},
		{".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
		{".webp", "image/webp"}, {".js", "text/javascript"},
	};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (!strcasecmp(dot, types[i].extension)) return types[i].mime;
	return "text/plain";
}
static nserror resource(const char *path, const uint8_t **data, size_t *size)
{
#define RESOURCE(name, symbol) if (!strcmp(path, name)) { *data = symbol; *size = symbol##_size; return NSERROR_OK; }
	RESOURCE("default.css", nsw_default_css)
	RESOURCE("internal.css", nsw_internal_css)
	RESOURCE("quirks.css", nsw_quirks_css)
	RESOURCE("adblock.css", nsw_adblock_css)
#undef RESOURCE
	if (!strcmp(path, "user.css")) { *data = nsw_empty_css; *size = 0; return NSERROR_OK; }
	return NSERROR_NOT_FOUND;
}
static nserror release(const uint8_t *data) { (void)data; return NSERROR_OK; }
struct gui_fetch_table *ov_resource_fetch_table(void)
{
	static struct gui_fetch_table table = {
		.filetype = filetype, .get_resource_data = resource,
		.release_resource_data = release,
	};
	return &table;
}
static nserror defaults(struct nsoption_s *options)
{
	(void)options;
#ifdef OV_ENABLE_WEB_JS
	nsoption_set_bool(enable_javascript, true);
#else
	nsoption_set_bool(enable_javascript, false);
#endif
	nsoption_set_int(memory_cache_size, 2 * 1024 * 1024);
	nsoption_set_uint(disc_cache_size, 0);
	nsoption_set_int(max_fetchers, 2);
	nsoption_set_int(max_fetchers_per_host, 2);
	return NSERROR_OK;
}
nserror ov_core_options_init(void)
{
	if (options_ready) return NSERROR_INIT_FAILED;
	nserror error = nsoption_init(defaults, &nsoptions, &nsoptions_default);
	if (error) return error;
	options_ready = true;
	error = messages_add_from_inline(nsw_messages, nsw_messages_size);
	if (error) { ov_core_options_fini(); return error; }
	browser_set_dpi(96);
	return NSERROR_OK;
}
void ov_core_options_fini(void)
{
	if (options_ready) nsoption_finalise(nsoptions, nsoptions_default);
	options_ready = false;
}
