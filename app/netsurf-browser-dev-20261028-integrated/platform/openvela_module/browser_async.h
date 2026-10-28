/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OV_BROWSER_ASYNC_H
#define OV_BROWSER_ASYNC_H
#include "browser_native_host.h"
#include <pthread.h>
enum ov_browser_load_state { OV_BROWSER_EMPTY, OV_BROWSER_LOADING, OV_BROWSER_READY, OV_BROWSER_FAILED };
/* Persistent loader; never destroy while its worker or module is resident. */
struct ov_browser_async {
	pthread_mutex_t lock;
	enum ov_browser_load_state state;
	struct ov_browser_host host;
	const struct ov_browser_loader *loader;
	const char *abi;
	char entry[192];
};
int ov_browser_async_init(struct ov_browser_async *);
bool ov_browser_async_start(struct ov_browser_async *, const struct ov_browser_loader *,
	const char *abi, const char *entry);
enum ov_browser_load_state ov_browser_async_state(struct ov_browser_async *);
#endif
