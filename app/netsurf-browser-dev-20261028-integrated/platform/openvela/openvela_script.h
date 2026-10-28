/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_SCRIPT_H
#define NETSURF_OPENVELA_SCRIPT_H
#include <stdbool.h>
#include <stdatomic.h>
#include "openvela_schedule.h"

/* Initial scripts and feature detection can take seconds on a 400 MHz MCU.
 * UI cancellation stays independent of this finite task watchdog. */
#define OV_SCRIPT_TASK_BUDGET_MS 15000u

struct ov_script_control {
	atomic_uint generation;
	ov_now_ms_fn now_ms;
	void *clock_ctx;
	uint64_t deadline_ms;
	atomic_bool executing;
};
bool ov_script_bind(struct ov_script_control *control, ov_now_ms_fn clock, void *ctx);
void ov_script_unbind(struct ov_script_control *control);
/* UI thread: interrupt without waiting for the core event loop. */
void ov_script_cancel(struct ov_script_control *control);
uint32_t ov_script_control(enum gui_script_control action);
size_t ov_script_memory_limit(void);
#endif
