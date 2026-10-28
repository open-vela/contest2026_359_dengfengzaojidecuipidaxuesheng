/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_script.h"
#include <stdio.h>

static struct ov_script_control *active;

bool ov_script_bind(struct ov_script_control *control, ov_now_ms_fn clock, void *ctx)
{
	if (!control || !clock || active) return false;
	atomic_init(&control->generation, 0);
	control->now_ms = clock;
	control->clock_ctx = ctx;
	atomic_init(&control->executing, false);
	active = control;
	return true;
}
void ov_script_unbind(struct ov_script_control *control)
{
	if (active == control) active = NULL;
}
void ov_script_cancel(struct ov_script_control *control)
{
	if (control) atomic_fetch_add_explicit(&control->generation, 1, memory_order_relaxed);
}
uint32_t ov_script_control(enum gui_script_control action)
{
	struct ov_script_control *control = active;
	if (!control) return 0;
	if (action == GUI_SCRIPT_BEGIN) {
		/* A microtask checkpoint begins each job but ends only once.
		 * Keep the first deadline across its short jobs. */
		if (!control->executing)
			control->deadline_ms = control->now_ms(control->clock_ctx) + OV_SCRIPT_TASK_BUDGET_MS;
		control->executing = true;
	} else if (action == GUI_SCRIPT_POLL && control->executing &&
		   control->now_ms(control->clock_ctx) >= control->deadline_ms) {
		ov_script_cancel(control);
		control->executing = false;
		fprintf(stderr, "[browser-script] task watchdog expired after %u ms\n", OV_SCRIPT_TASK_BUDGET_MS);
	} else if (action == GUI_SCRIPT_END || action == GUI_SCRIPT_CANCELLED) {
		if (control->executing) {
			uint64_t elapsed=control->now_ms(control->clock_ctx) -
				(control->deadline_ms-OV_SCRIPT_TASK_BUDGET_MS);
			if (elapsed >= 250)
				fprintf(stderr,"[browser-script] task finished after %llu ms\n",
					(unsigned long long)elapsed);
		}
		control->executing = false;
	}
	return atomic_load_explicit(&control->generation, memory_order_relaxed);
}
size_t ov_script_memory_limit(void) { return 6u * 1024u * 1024u; }
