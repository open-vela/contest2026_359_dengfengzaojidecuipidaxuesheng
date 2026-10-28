/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_async.h"
#include <sched.h>
#include <stdio.h>
#include <string.h>

static void *load(void *arg)
{
	struct ov_browser_async *a = arg;
	bool ok = ov_browser_host_load(&a->host, a->loader, a->abi,
		"com.netsurf.browser", a->entry);
	pthread_mutex_lock(&a->lock);
	a->state = ok ? OV_BROWSER_READY : OV_BROWSER_FAILED;
	pthread_mutex_unlock(&a->lock);
	return NULL;
}
int ov_browser_async_init(struct ov_browser_async *a)
{
	memset(a, 0, sizeof(*a));
	return pthread_mutex_init(&a->lock, NULL);
}
enum ov_browser_load_state ov_browser_async_state(struct ov_browser_async *a)
{
	pthread_mutex_lock(&a->lock);
	enum ov_browser_load_state state = a->state;
	pthread_mutex_unlock(&a->lock);
	return state;
}
bool ov_browser_async_start(struct ov_browser_async *a, const struct ov_browser_loader *loader,
	const char *abi, const char *entry)
{
	/* Only the UI starts workers. It never reads host until completion. */
	if (ov_browser_async_state(a) == OV_BROWSER_LOADING) return true;
	if (!entry || strlen(entry) >= sizeof(a->entry)) return false;
	snprintf(a->entry, sizeof(a->entry), "%s", entry);
	a->abi = abi;
	a->loader = loader;
	pthread_attr_t attr;
	int error = pthread_attr_init(&attr);
	if (error) return false;
	error = pthread_attr_setstacksize(&attr, 64u * 1024u);
	if (!error) error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __NuttX__
	int policy;
	struct sched_param priority;
	if (!error) error = pthread_getschedparam(pthread_self(), &policy, &priority);
	if (!error && priority.sched_priority > sched_get_priority_min(policy)) {
		priority.sched_priority--;
		error = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		if (!error) error = pthread_attr_setschedpolicy(&attr, policy);
		if (!error) error = pthread_attr_setschedparam(&attr, &priority);
	}
#endif
	pthread_mutex_lock(&a->lock);
	a->state = OV_BROWSER_LOADING;
	if (!error) {
		pthread_t thread;
		error = pthread_create(&thread, &attr, load, a);
	}
	if (error) {
		snprintf(a->host.error, sizeof(a->host.error), "浏览器加载线程启动失败 (%d)", error);
		a->state = OV_BROWSER_FAILED;
	}
	pthread_mutex_unlock(&a->lock);
	pthread_attr_destroy(&attr);
	return error == 0;
}
