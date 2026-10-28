#include "openvela_platform.h"

#include <assert.h>
#include <stdio.h>

static unsigned wake_count;

static void test_wake(void *ctx)
{
	(void)ctx;
	wake_count++;
}

static void test_touch_burst(void)
{
	struct ov_event_queue q; struct ov_event e;
	ov_event_queue_init(&q, NULL);
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_DOWN, 20, 1200, 0, NULL));
	for (int i=1; i<=1000; i++)
		assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_MOVE, 20, 1200-i, i, NULL));
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_UP, 20, 199, 1001, NULL));
	assert(ov_event_queue_size(&q)==3 && ov_event_queue_dropped(&q)==0);
	assert(ov_event_pop(&q,&e) && e.type==OV_EVENT_TOUCH_DOWN);
	assert(ov_event_pop(&q,&e) && e.type==OV_EVENT_TOUCH_MOVE && e.payload.touch.y==200 && e.timestamp_ms==1000);
	assert(ov_event_pop(&q,&e) && e.type==OV_EVENT_TOUCH_UP);
	/* A turn followed by a return must still be seen as a drag, not a tap. */
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_DOWN, 20, 100, 0, NULL));
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_MOVE, 20, 20, 1, NULL));
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_MOVE, 20, 100, 2, NULL));
	assert(ov_frontend_publish_touch(&q, OV_EVENT_TOUCH_UP, 20, 100, 3, NULL));
	assert(ov_event_queue_size(&q)==4);
	while (ov_event_pop(&q,&e)) {}
	/* Use a wrapped ring and interleaved control events. A full queue may
	 * discard a position to retain UP/CANCEL, but must keep control order. */
	for (unsigned i=0;i<100;i++) {
		e=(struct ov_event){.type=OV_EVENT_TICK}; assert(ov_event_push(&q,&e));
		assert(ov_event_pop(&q,&e));
	}
	for (unsigned boundary=OV_EVENT_TOUCH_DOWN;boundary<=OV_EVENT_TOUCH_CANCEL;boundary++) {
		if (boundary==OV_EVENT_TOUCH_MOVE) continue;
		for (unsigned i=0;i<OV_EVENT_QUEUE_CAPACITY;i++) {
			e=(struct ov_event){.type=i==10?OV_EVENT_TOUCH_MOVE:OV_EVENT_TICK,.timestamp_ms=i};
			assert(ov_event_push(&q,&e));
		}
		e=(struct ov_event){.type=boundary}; assert(ov_event_push(&q,&e));
		for (unsigned i=0;i<OV_EVENT_QUEUE_CAPACITY;i++) {
			if (i==10) continue;
			assert(ov_event_pop(&q,&e) && e.type==OV_EVENT_TICK && e.timestamp_ms==i);
		}
		assert(ov_event_pop(&q,&e) && e.type==(boundary==OV_EVENT_TOUCH_UP?
			OV_EVENT_TOUCH_CANCEL:(enum ov_event_type)boundary));
		assert(!ov_event_pop(&q,&e));
	}
	/* Navigation is a barrier and may never be coalesced across. */
	assert(ov_frontend_publish_touch(&q,OV_EVENT_TOUCH_MOVE,1,100,0,NULL));
	assert(ov_frontend_publish_navigation(&q,"about:blank",0,NULL));
	assert(ov_frontend_publish_touch(&q,OV_EVENT_TOUCH_MOVE,1,50,0,NULL));
	assert(ov_event_queue_size(&q)==3);
	puts("PASS: 1000 moves -> 3 ordered events; turns, barriers and full-ring gesture boundaries retained");
}

int main(void)
{
	test_touch_burst();
	struct ov_event_queue queue;
	struct ov_frontend_ops ops = {.wake_core = test_wake};
	struct ov_event event = {.type = OV_EVENT_TICK};
	ov_frontend_init(&queue, &ops);
	assert(ov_frontend_publish(&queue, &event, &ops));
	assert(wake_count == 1);
	assert(ov_event_queue_size(&queue) == 1);
	assert(ov_event_pop(&queue, &event));
	assert(event.type == OV_EVENT_TICK);
	assert(ov_event_queue_size(&queue) == 0);

	struct ov_event net_event = {.type = OV_EVENT_NET_DATA};
	net_event.payload.net_data.length = OV_NET_EVENT_DATA_BYTES;
	assert(ov_event_push(&queue, &net_event));
	assert(ov_event_pop(&queue, &net_event));
	if (OV_NET_EVENT_DATA_BYTES < UINT16_MAX) {
		net_event.payload.net_data.length = OV_NET_EVENT_DATA_BYTES + 1u;
		assert(!ov_event_push(&queue, &net_event));
		assert(ov_event_queue_dropped(&queue) == 1);
	}

	for (size_t i = 0; i < OV_EVENT_QUEUE_CAPACITY; i++)
		assert(ov_event_push(&queue, &event));
	assert(!ov_event_push(&queue, &event));
	assert(ov_event_queue_dropped(&queue) ==
	       (OV_NET_EVENT_DATA_BYTES < UINT16_MAX ? 2u : 1u));

	struct ov_damage damage;
	ov_damage_reset(&damage);
	ov_damage_add(&damage, (struct ov_rect){-10, -10, 20, 20}, 100, 80);
	ov_damage_add(&damage, (struct ov_rect){20, 10, 40, 30}, 100, 80);
	assert(damage.count == 1);
	assert(damage.rects[0].x0 == 0 && damage.rects[0].y0 == 0);
	assert(damage.rects[0].x1 == 40 && damage.rects[0].y1 == 30);

	for (int i = 0; i < (int)OV_MAX_DAMAGE_RECTS; i++)
		ov_damage_add(&damage, (struct ov_rect){i * 5, 40, i * 5 + 2, 42}, 100, 80);
	assert(!ov_damage_is_empty(&damage));
	printf("openvela platform smoke test passed\n");
	return 0;
}
