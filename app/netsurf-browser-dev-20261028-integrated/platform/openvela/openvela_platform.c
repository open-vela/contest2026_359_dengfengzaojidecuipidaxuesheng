#include "openvela_platform.h"

#include <string.h>

static void queue_lock(struct ov_event_queue *queue)
{
	if (queue->sync.lock != NULL)
		queue->sync.lock(queue->sync.ctx);
}

static void queue_unlock(struct ov_event_queue *queue)
{
	if (queue->sync.unlock != NULL)
		queue->sync.unlock(queue->sync.ctx);
}

void ov_event_queue_init(struct ov_event_queue *queue,
			 const struct ov_queue_sync *sync)
{
	if (queue == NULL)
		return;
	memset(queue, 0, sizeof(*queue));
	if (sync != NULL && sync->lock != NULL && sync->unlock != NULL)
		queue->sync = *sync;
}

static bool continues_motion(const struct ov_event *before,
		const struct ov_event *last, const struct ov_event *next)
{
	if (before->type != OV_EVENT_TOUCH_DOWN && before->type != OV_EVENT_TOUCH_MOVE)
		return false;
	int64_t ax = (int64_t)last->payload.touch.x - before->payload.touch.x;
	int64_t ay = (int64_t)last->payload.touch.y - before->payload.touch.y;
	int64_t bx = (int64_t)next->payload.touch.x - last->payload.touch.x;
	int64_t by = (int64_t)next->payload.touch.y - last->payload.touch.y;
	/* Retain turns: they can start a drag even if the finger returns to down. */
	if ((ax < 0 && bx > 0) || (ax > 0 && bx < 0) ||
	    (ay < 0 && by > 0) || (ay > 0 && by < 0))
		return false;
	if (bx == 0 && by == 0) return true;
	if (ax < 0) ax = -ax;
	if (ay < 0) ay = -ay;
	if (bx < 0) bx = -bx;
	if (by < 0) by = -by;
	return (ax >= ay) == (bx >= by);
}

bool ov_event_push(struct ov_event_queue *queue, const struct ov_event *event)
{
	if (queue == NULL || event == NULL)
		return false;
	bool cancel_release = false;
	queue_lock(queue);
	if (event->type == OV_EVENT_NET_DATA &&
	    event->payload.net_data.length > OV_NET_EVENT_DATA_BYTES) {
		queue->dropped++;
		queue_unlock(queue);
		return false;
	}
	if (event->type == OV_EVENT_TOUCH_MOVE && queue->count >= 2) {
		size_t last = (queue->tail + OV_EVENT_QUEUE_CAPACITY - 1) % OV_EVENT_QUEUE_CAPACITY;
		size_t before = (last + OV_EVENT_QUEUE_CAPACITY - 1) % OV_EVENT_QUEUE_CAPACITY;
		if (queue->entries[last].type == OV_EVENT_TOUCH_MOVE &&
		    continues_motion(&queue->entries[before], &queue->entries[last], event)) {
			queue->entries[last] = *event;
			queue_unlock(queue);
			return true;
		}
	}
	if (queue->count == OV_EVENT_QUEUE_CAPACITY &&
	    (event->type == OV_EVENT_TOUCH_DOWN || event->type == OV_EVENT_TOUCH_UP ||
	     event->type == OV_EVENT_TOUCH_CANCEL)) {
		/* Sacrifice a stale position, never the gesture boundary. Preserve the
		 * relative order of navigation, network and other touch boundaries. */
		for (size_t i = 0; i < queue->count; i++) {
			size_t pos = (queue->head + i) % OV_EVENT_QUEUE_CAPACITY;
			if (queue->entries[pos].type != OV_EVENT_TOUCH_MOVE) continue;
			for (size_t j = i; j + 1 < queue->count; j++)
				queue->entries[(queue->head+j)%OV_EVENT_QUEUE_CAPACITY] =
					queue->entries[(queue->head+j+1)%OV_EVENT_QUEUE_CAPACITY];
			queue->tail = (queue->tail + OV_EVENT_QUEUE_CAPACITY - 1) % OV_EVENT_QUEUE_CAPACITY;
			queue->count--;
			queue->dropped++;
			/* The removed path may have been the only evidence of a drag.
			 * End safely instead of turning an overloaded gesture into a tap. */
			cancel_release = event->type == OV_EVENT_TOUCH_UP;
			break;
		}
	}
	if (queue->count == OV_EVENT_QUEUE_CAPACITY) {
		/* Keep memory bounded; the owner can observe the loss and recover. */
		queue->dropped++;
		queue_unlock(queue);
		return false;
	}
	queue->entries[queue->tail] = *event;
	if (cancel_release) queue->entries[queue->tail].type = OV_EVENT_TOUCH_CANCEL;
	queue->tail = (queue->tail + 1u) % OV_EVENT_QUEUE_CAPACITY;
	queue->count++;
	queue_unlock(queue);
	return true;
}

bool ov_event_pop(struct ov_event_queue *queue, struct ov_event *event)
{
	if (queue == NULL || event == NULL)
		return false;
	queue_lock(queue);
	if (queue->count == 0) {
		queue_unlock(queue);
		return false;
	}
	*event = queue->entries[queue->head];
	queue->head = (queue->head + 1u) % OV_EVENT_QUEUE_CAPACITY;
	queue->count--;
	queue_unlock(queue);
	return true;
}

size_t ov_event_queue_size(struct ov_event_queue *queue)
{
	if (queue == NULL)
		return 0;
	queue_lock(queue);
	size_t count = queue->count;
	queue_unlock(queue);
	return count;
}

uint32_t ov_event_queue_dropped(struct ov_event_queue *queue)
{
	if (queue == NULL)
		return 0;
	queue_lock(queue);
	uint32_t dropped = queue->dropped;
	queue_unlock(queue);
	return dropped;
}

static bool rect_empty(struct ov_rect rect)
{
	return rect.x0 >= rect.x1 || rect.y0 >= rect.y1;
}

static struct ov_rect rect_intersect(struct ov_rect a, struct ov_rect b)
{
	struct ov_rect result = {
		.x0 = a.x0 > b.x0 ? a.x0 : b.x0,
		.y0 = a.y0 > b.y0 ? a.y0 : b.y0,
		.x1 = a.x1 < b.x1 ? a.x1 : b.x1,
		.y1 = a.y1 < b.y1 ? a.y1 : b.y1,
	};
	return result;
}

static bool rect_overlaps_or_touches(struct ov_rect a, struct ov_rect b)
{
	return a.x0 <= b.x1 && a.x1 >= b.x0 &&
	       a.y0 <= b.y1 && a.y1 >= b.y0;
}

static struct ov_rect rect_union(struct ov_rect a, struct ov_rect b)
{
	struct ov_rect result = {
		.x0 = a.x0 < b.x0 ? a.x0 : b.x0,
		.y0 = a.y0 < b.y0 ? a.y0 : b.y0,
		.x1 = a.x1 > b.x1 ? a.x1 : b.x1,
		.y1 = a.y1 > b.y1 ? a.y1 : b.y1,
	};
	return result;
}

void ov_damage_reset(struct ov_damage *damage)
{
	if (damage != NULL)
		memset(damage, 0, sizeof(*damage));
}

void ov_damage_add(struct ov_damage *damage, struct ov_rect rect,
			   int width, int height)
{
	if (damage == NULL || damage->full || width <= 0 || height <= 0)
		return;

	struct ov_rect bounds = {0, 0, width, height};
	rect = rect_intersect(rect, bounds);
	if (rect_empty(rect))
		return;

	/* Expand the new rectangle until it no longer touches any existing area.
	 * A merge may bridge previously separate regions, so restart the scan. */
	for (size_t i = 0; i < damage->count;) {
		if (!rect_overlaps_or_touches(damage->rects[i], rect)) {
			i++;
			continue;
		}
		rect = rect_union(damage->rects[i], rect);
		damage->rects[i] = damage->rects[damage->count - 1u];
		damage->count--;
		i = 0;
	}

	if (damage->count == OV_MAX_DAMAGE_RECTS) {
		/* A fragmented page is more expensive than a bounded full frame. */
		damage->count = 0;
		damage->full = true;
		return;
	}
	damage->rects[damage->count++] = rect;
}

bool ov_damage_is_empty(const struct ov_damage *damage)
{
	return damage == NULL || (!damage->full && damage->count == 0);
}

void ov_frontend_init(struct ov_event_queue *queue,
			      const struct ov_frontend_ops *ops)
{
	(void)ops;
	ov_event_queue_init(queue, ops != NULL ? &ops->queue_sync : NULL);
}

bool ov_frontend_publish(struct ov_event_queue *queue,
				 const struct ov_event *event,
				 const struct ov_frontend_ops *ops)
{
	bool accepted = ov_event_push(queue, event);
	if (accepted && ops != NULL && ops->wake_core != NULL)
		ops->wake_core(ops->ctx);
	return accepted;
}

bool ov_frontend_publish_touch(struct ov_event_queue *queue,
				       enum ov_event_type type,
				       int x, int y, uint64_t timestamp_ms,
				       const struct ov_frontend_ops *ops)
{
	if (type < OV_EVENT_TOUCH_DOWN || type > OV_EVENT_TOUCH_CANCEL)
		return false;
	struct ov_event event = {
		.type = type,
		.timestamp_ms = timestamp_ms,
		.payload.touch = {.x = x, .y = y},
	};
	return ov_frontend_publish(queue, &event, ops);
}

bool ov_frontend_publish_navigation(struct ov_event_queue *queue,
					    const char *url,
					    uint64_t timestamp_ms,
					    const struct ov_frontend_ops *ops)
{
	if (url == NULL || url[0] == '\0')
		return false;
	struct ov_event event = {.type = OV_EVENT_NAVIGATE,
					 .timestamp_ms = timestamp_ms};
	size_t length = strlen(url);
	if (length >= sizeof(event.payload.navigate.url))
		return false;
	memcpy(event.payload.navigate.url, url, length + 1u);
	return ov_frontend_publish(queue, &event, ops);
}

