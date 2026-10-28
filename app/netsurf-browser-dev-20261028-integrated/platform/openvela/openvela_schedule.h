/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_SCHEDULE_H
#define NETSURF_OPENVELA_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/misc.h"

#define OV_SCHEDULER_CAPACITY 128u

typedef uint64_t (*ov_now_ms_fn)(void *ctx);

struct ov_scheduler_entry {
	uint64_t deadline_ms;
	uint64_t sequence;
	void (*callback)(void *ctx);
	void *ctx;
};

struct ov_scheduler {
	struct ov_scheduler_entry entries[OV_SCHEDULER_CAPACITY];
	size_t count;
	uint64_t next_sequence;
	ov_now_ms_fn now_ms;
	void *clock_ctx;
};

nserror ov_scheduler_init(struct ov_scheduler *scheduler,
			  ov_now_ms_fn now_ms, void *clock_ctx);
nserror ov_scheduler_schedule(struct ov_scheduler *scheduler, int delay_ms,
			     void (*callback)(void *ctx), void *ctx);
/* Run at most callback_budget callbacks or until time_budget_ms elapses.
 * Returns milliseconds until the next callback, or -1 when idle. */
int ov_scheduler_run_due(struct ov_scheduler *scheduler,
			 unsigned callback_budget, uint32_t time_budget_ms);
int ov_scheduler_next_delay_ms(struct ov_scheduler *scheduler);

/* NetSurf's gui_misc_table.schedule callback uses the currently bound loop. */
nserror ov_scheduler_bind(struct ov_scheduler *scheduler);
void ov_scheduler_unbind(struct ov_scheduler *scheduler);
struct gui_misc_table *ov_scheduler_misc_table(void);

#endif
