/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_native_host.h"
#include "browser_async.h"
#include "browser_host_abi.h"
#include "browser_cache.h"
#include <esp_cache.h>
#include <nuttx/lib/elf.h>
#include <nuttx/arch.h>
#include <nuttx/module.h>
#include <nuttx/symtab.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

extern const struct symtab_s ov_browser_exports[];
extern const int ov_browser_nexports;
static struct native_page {
 struct ov_browser_async load;
 JSContext *context;
 JSValue system;
 lv_obj_t *root;
 lv_timer_t *timer;
} *page;
static bool sync_cache(void *ctx, uintptr_t addr, size_t bytes, bool instruction)
{
	(void)ctx;
	int flags = instruction ? ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST :
		ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA;
	return esp_cache_msync((void *)addr, bytes, flags) == ESP_OK;
}
static void *open_module(const char *path)
{
	const struct symtab_s *previous;
	int previous_count;
	libelf_getsymtab(&previous, &previous_count);
	libelf_setsymtab(ov_browser_exports, ov_browser_nexports);
	fprintf(stderr,"[browser-loader] loading %s\n",path);
	void *handle = insmod(path, "netsurf-browser");
	int error=errno;
	libelf_setsymtab(previous, previous_count);
	if (!handle) fprintf(stderr,"[browser-loader] insmod failed errno=%d\n",error);
	if (handle) {
#if !defined(CONFIG_FS_PROCFS) || defined(CONFIG_FS_PROCFS_EXCLUDE_MODULE)
#error "Native browser requires module textsize metadata"
#endif
		struct module_s *module = handle;
		fprintf(stderr,"[browser-loader] text=%p bytes=%lu\n",module->textalloc,(unsigned long)module->textsize);
#ifdef CONFIG_OV_BROWSER_TEXT_POOL
		if (!up_textheap_heapmember(module->textalloc)) {
			(void)rmmod(handle);
			fprintf(stderr,"[browser-loader] executable pool mismatch\n");
			return NULL;
		}
#endif
		size_t line = esp_cache_get_line_size_by_addr(module->textalloc);
		if (!ov_browser_cache_prepare((uintptr_t)module->textalloc, module->textsize,
			line, sync_cache, NULL)) {
			(void)rmmod(handle);
			fprintf(stderr,"[browser-loader] cache preparation failed\n");
			return NULL;
		}
	}
	if (handle) fprintf(stderr,"[browser-loader] ready\n");
	return handle;
}
static ov_browser_module_query_fn query_module(void *handle)
{
	return (ov_browser_module_query_fn)modsym(handle, "ov_browser_module_query");
}
static void close_module(void *handle) { (void)rmmod(handle); }
static const struct ov_browser_loader loader = {open_module, query_module, close_module};
static void publish_error(const char *message)
{
 JS_SetPropertyStr(page->context, page->system, "browserLoading", JS_FALSE);
 JS_SetPropertyStr(page->context, page->system, "browserError",
  JS_NewString(page->context, message));
}
static void poll_load(lv_timer_t *timer)
{
 (void)timer;
 enum ov_browser_load_state state = ov_browser_async_state(&page->load);
 if (state == OV_BROWSER_LOADING) return;
 lv_timer_delete(page->timer);
 page->timer = NULL;
 /* Only the LVGL owner touches context, system, root or LVGL objects. */
 if (state != OV_BROWSER_READY) { publish_error(page->load.host.error); return; }
 struct ov_browser_host *host = &page->load.host;
 if (!host->api->attach(page->context, page->system, page->root, "com.netsurf.browser")) {
  publish_error("浏览器初始化失败，请检查字体、证书和可用内存"); return;
 }
 host->attached = true;
 JS_SetPropertyStr(page->context, page->system, "browserLoading", JS_FALSE);
 fprintf(stderr, "[browser-loader] UI attached\n");
}
bool ov_browser_native_install(JSContext *context, JSValue system, lv_obj_t *root,
 const char *package, const char *entry)
{
 if (!package || strcmp(package, "com.netsurf.browser")) return true;
 if (!page) {
  struct native_page *created = calloc(1, sizeof(*created));
  if (created && ov_browser_async_init(&created->load)) { free(created); created = NULL; }
  page = created;
 }
 if (!page) {
  JS_SetPropertyStr(context, system, "browserError", JS_NewString(context, "浏览器加载器内存不足"));
  return false;
 }
 ov_browser_native_stop();
 page->context = context;
 page->system = JS_DupValue(context, system);
 page->root = root;
 page->timer = lv_timer_create(poll_load, 50, NULL);
 if (!page->timer) { publish_error("浏览器加载定时器创建失败"); return false; }
 JS_SetPropertyStr(context, system, "browserLoading", JS_TRUE);
 if (!ov_browser_async_start(&page->load, &loader, OV_BROWSER_HOST_ABI, entry)) {
  lv_timer_delete(page->timer); page->timer = NULL;
  publish_error(page->load.host.error[0] ? page->load.host.error : "浏览器启动目录无效");
  return false;
 }
 return true;
}
void ov_browser_native_stop(void)
{
 if (!page) return;
 if (page->timer) { lv_timer_delete(page->timer); page->timer = NULL; }
 /* Loading outlives the page, but all UI references expire now. */
 if (ov_browser_async_state(&page->load) == OV_BROWSER_READY)
  ov_browser_host_detach(&page->load.host);
 if (page->context) JS_FreeValue(page->context, page->system);
 page->context = NULL;
 page->root = NULL;
}
