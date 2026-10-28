/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_native_host.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef OV_BROWSER_SD_ROOT
#define OV_BROWSER_SD_ROOT "/sdcard/qpk"
#endif
#ifndef OV_BROWSER_DATA_ROOT
#define OV_BROWSER_DATA_ROOT "/data/qpk"
#endif

static bool fail(struct ov_browser_host *host, const char *message)
{
	snprintf(host->error, sizeof(host->error), "%s", message);
	return false;
}
static bool module_path(const char *entry, char *out, size_t bytes)
{
	static const char *entries[] = {
		OV_BROWSER_SD_ROOT "/com.netsurf.browser/app.js",
		OV_BROWSER_DATA_ROOT "/com.netsurf.browser/app.js"
	};
	for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (!strcmp(entry, entries[i])) {
			int n = snprintf(out, bytes, "%.*snative/browser.elf",
				(int)(strlen(entry) - strlen("app.js")), entry);
			return n > 0 && (size_t)n < bytes;
		}
	}
	return false;
}
bool ov_browser_host_load(struct ov_browser_host *host, const struct ov_browser_loader *loader,
	const char *host_abi,
	const char *package, const char *entry)
{
	if (!host || !loader || !loader->open || !loader->query || !loader->close || !host_abi)
		return false;
	host->error[0] = 0;
	if (!package || strcmp(package, "com.netsurf.browser")) return true;
	char path[192], abi_path[192], abi[66] = {0};
	if (!entry || !module_path(entry, path, sizeof(path)))
		return fail(host, "浏览器必须从已安装的应用目录启动");
	if (host->attached) return fail(host, "浏览器页面尚未退出");
	if (!host->api) {
		snprintf(abi_path, sizeof(abi_path), "%.*shost.abi",
			(int)(strlen(path) - strlen("browser.elf")), path);
		FILE *file = fopen(abi_path, "rb");
		if (!file) return fail(host, "浏览器原生模块或兼容性文件缺失");
		size_t n = fread(abi, 1, sizeof(abi) - 1, file);
		bool read_error = ferror(file) != 0;
		fclose(file);
		if (n == 65 && abi[64] == '\n') abi[64] = 0;
		if (read_error || (n != 64 && n != 65) || strlen(host_abi) != 64 || strcmp(abi, host_abi))
			return fail(host, "浏览器模块与系统 ABI 不兼容，请安装配套版本");
		struct stat info;
		if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 16 * 1024 * 1024)
			return fail(host, "浏览器模块文件无效或过大");
		void *handle = loader->open(path);
		if (!handle) return fail(host, "浏览器模块加载失败：请检查内存和系统原生模块支持");
		ov_browser_module_query_fn query = loader->query(handle);
		const struct ov_browser_module *api = query ? query() : NULL;
		if (!api || api->abi != OV_BROWSER_MODULE_ABI || api->bytes != sizeof(*api) ||
			!api->host_abi || strcmp(api->host_abi, host_abi) || !api->attach || !api->detach) {
			loader->close(handle);
			return fail(host, "浏览器模块接口不兼容");
		}
		host->handle = handle;
		host->api = api;
	}
	return true;
}
bool ov_browser_host_attach(struct ov_browser_host *host, const struct ov_browser_loader *loader,
	const char *host_abi, JSContext *context, JSValue system, lv_obj_t *root,
	const char *package, const char *entry)
{
	if (!ov_browser_host_load(host, loader, host_abi, package, entry)) return false;
	if (!package || strcmp(package, "com.netsurf.browser")) return true;
	/* Retain the module even if attach fails: it may have started its owner
	 * thread before discovering a missing font or failed transport. */
	if (!host->api->attach(context, system, root, package))
		return fail(host, "浏览器初始化失败，请检查字体、证书和可用内存");
	host->attached = true;
	return true;
}
void ov_browser_host_detach(struct ov_browser_host *host)
{
	if (host && host->api && host->attached) {
		host->api->detach();
		host->attached = false;
	}
}
