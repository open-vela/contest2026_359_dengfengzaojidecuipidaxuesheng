/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_schedule.h"
#include "openvela_script.h"

#include <stdbool.h>
#include <limits.h>
#include <string.h>

static struct ov_scheduler *active_scheduler;

static bool entry_before(const struct ov_scheduler_entry *a,
			const struct ov_scheduler_entry *b)
{
	return a->deadline_ms < b->deadline_ms ||
	       (a->deadline_ms == b->deadline_ms && a->sequence < b->sequence);
}

static void entry_swap(struct ov_scheduler_entry *a,
		       struct ov_scheduler_entry *b)
{
	struct ov_scheduler_entry tmp = *a;
	*a = *b;
	*b = tmp;
}

static void sift_up(struct ov_scheduler *scheduler, size_t index)
{
	while (index > 0) {
		size_t parent = (index - 1u) / 2u;
		if (!entry_before(&scheduler->entries[index],
				  &scheduler->entries[parent]))
			break;
		entry_swap(&scheduler->entries[index], &scheduler->entries[parent]);
		index = parent;
	}
}

static void sift_down(struct ov_scheduler *scheduler, size_t index)
{
	for (;;) {
		size_t left = index * 2u + 1u;
		size_t right = left + 1u;
		size_t first = index;
		if (left < scheduler->count &&
		    entry_before(&scheduler->entries[left],
				&scheduler->entries[first]))
			first = left;
		if (right < scheduler->count &&
		    entry_before(&scheduler->entries[right],
				&scheduler->entries[first]))
			first = right;
		if (first == index)
			break;
		entry_swap(&scheduler->entries[index], &scheduler->entries[first]);
		index = first;
	}
}

static void remove_at(struct ov_scheduler *scheduler, size_t index)
{
	scheduler->count--;
	if (index == scheduler->count)
		return;
	scheduler->entries[index] = scheduler->entries[scheduler->count];
	if (index > 0 &&
	    entry_before(&scheduler->entries[index],
			 &scheduler->entries[(index - 1u) / 2u]))
		sift_up(scheduler, index);
	else
		sift_down(scheduler, index);
}

static bool remove_callback(struct ov_scheduler *scheduler,
			    void (*callback)(void *), void *ctx)
{
	for (size_t i = 0; i < scheduler->count; i++) {
		if (scheduler->entries[i].callback == callback &&
		    scheduler->entries[i].ctx == ctx) {
			remove_at(scheduler, i);
			return true;
		}
	}
	return false;
}

nserror ov_scheduler_init(struct ov_scheduler *scheduler,
			  ov_now_ms_fn now_ms, void *clock_ctx)
{
	if (scheduler == NULL || now_ms == NULL)
		return NSERROR_BAD_PARAMETER;
	memset(scheduler, 0, sizeof(*scheduler));
	scheduler->now_ms = now_ms;
	scheduler->clock_ctx = clock_ctx;
	return NSERROR_OK;
}

nserror ov_scheduler_schedule(struct ov_scheduler *scheduler, int delay_ms,
			     void (*callback)(void *ctx), void *ctx)
{
	if (scheduler == NULL || scheduler->now_ms == NULL || callback == NULL)
		return NSERROR_BAD_PARAMETER;
	bool replaced = remove_callback(scheduler, callback, ctx);
	if (delay_ms < 0)
		return replaced ? NSERROR_OK : NSERROR_NOT_FOUND;
	if (scheduler->count == OV_SCHEDULER_CAPACITY)
		return NSERROR_NOSPACE;
	uint64_t now = scheduler->now_ms(scheduler->clock_ctx);
	uint64_t delay = (uint64_t)delay_ms;
	uint64_t deadline = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
	size_t index = scheduler->count++;
	scheduler->entries[index] = (struct ov_scheduler_entry){
		.deadline_ms = deadline,
		.sequence = scheduler->next_sequence++,
		.callback = callback,
		.ctx = ctx,
	};
	sift_up(scheduler, index);
	return NSERROR_OK;
}

int ov_scheduler_next_delay_ms(struct ov_scheduler *scheduler)
{
	if (scheduler == NULL || scheduler->now_ms == NULL || scheduler->count == 0)
		return -1;
	uint64_t now = scheduler->now_ms(scheduler->clock_ctx);
	if (scheduler->entries[0].deadline_ms <= now)
		return 0;
	uint64_t delay = scheduler->entries[0].deadline_ms - now;
	return delay > INT_MAX ? INT_MAX : (int)delay;
}

int ov_scheduler_run_due(struct ov_scheduler *scheduler,
			 unsigned callback_budget, uint32_t time_budget_ms)
{
	if (scheduler == NULL || scheduler->now_ms == NULL)
		return -1;
	if (callback_budget == 0)
		return ov_scheduler_next_delay_ms(scheduler);
	uint64_t started = scheduler->now_ms(scheduler->clock_ctx);
	for (unsigned run = 0; run < callback_budget && scheduler->count > 0; run++) {
		uint64_t now = scheduler->now_ms(scheduler->clock_ctx);
		if (scheduler->entries[0].deadline_ms > now)
			break;
		struct ov_scheduler_entry due = scheduler->entries[0];
		remove_at(scheduler, 0);
		due.callback(due.ctx);
		if (time_budget_ms != 0 &&
		    scheduler->now_ms(scheduler->clock_ctx) - started >= time_budget_ms)
			break;
	}
	return ov_scheduler_next_delay_ms(scheduler);
}

static nserror schedule_callback(int delay_ms,
				 void (*callback)(void *ctx), void *ctx)
{
	if (active_scheduler == NULL)
		return NSERROR_INIT_FAILED;
	return ov_scheduler_schedule(active_scheduler, delay_ms, callback, ctx);
}

static struct gui_misc_table misc_table = {
	.schedule = schedule_callback,
	.script_control = ov_script_control,
	.script_memory_limit = ov_script_memory_limit,
};

nserror ov_scheduler_bind(struct ov_scheduler *scheduler)
{
	if (scheduler == NULL || scheduler->now_ms == NULL ||
	    active_scheduler != NULL)
		return NSERROR_BAD_PARAMETER;
	active_scheduler = scheduler;
	return NSERROR_OK;
}

void ov_scheduler_unbind(struct ov_scheduler *scheduler)
{
	if (active_scheduler == scheduler)
		active_scheduler = NULL;
}

struct gui_misc_table *ov_scheduler_misc_table(void)
{
	return &misc_table;
}
