/*
 * OpenVela frontend substrate for the NetSurf ESP32-P4 port.
 *
 * This header is platform-neutral by design. The OpenVela integration owns a
 * single browser-core thread and uses this queue to cross from LVGL/input and
 * network callbacks into that thread.
 */
#ifndef NETSURF_OPENVELA_PLATFORM_H
#define NETSURF_OPENVELA_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OV_EVENT_QUEUE_CAPACITY 32u
#define OV_MAX_DAMAGE_RECTS 16u
#define OV_INPUT_TEXT_MAX 512u
#define OV_INPUT_CHAR_MAX 120

/* Passed only to the trusted App, never to webpage JavaScript. Password
 * values are deliberately omitted. IDs remain valid until consumed/cancelled. */
struct ov_input_request {
	uint32_t id;
	int max_length;
	bool password;
	char value[OV_INPUT_TEXT_MAX];
};
#ifndef OV_NET_EVENT_DATA_BYTES
#define OV_NET_EVENT_DATA_BYTES 512u
#endif
#if OV_NET_EVENT_DATA_BYTES == 0 || OV_NET_EVENT_DATA_BYTES > UINT16_MAX
#error "OV_NET_EVENT_DATA_BYTES must fit the event length field"
#endif

enum ov_event_type {
	OV_EVENT_NONE = 0,
	OV_EVENT_TICK,
	OV_EVENT_TOUCH_DOWN,
	OV_EVENT_TOUCH_MOVE,
	OV_EVENT_TOUCH_UP,
	OV_EVENT_TOUCH_CANCEL,
	OV_EVENT_NAVIGATE,
	OV_EVENT_BACK,
	OV_EVENT_FORWARD,
	OV_EVENT_RELOAD,
	OV_EVENT_STOP,
	OV_EVENT_SCROLL,
	OV_EVENT_RESIZE,
	OV_EVENT_INPUT,
	OV_EVENT_NET_DATA,
	OV_EVENT_NET_DONE,
	OV_EVENT_NET_ERROR,
};

struct ov_rect {
	int x0;
	int y0;
	int x1;
	int y1;
};

/* Payloads are bounded and owned by the event value. Navigation text and
 * network chunks are copied into the event, so producers may reuse buffers
 * after ov_event_push() returns. */
struct ov_event {
	enum ov_event_type type;
	uint64_t timestamp_ms;
	union {
		struct {
			int x;
			int y;
		} touch;
		struct {
			char url[256];
		} navigate;
		struct {
			int delta_x;
			int delta_y;
		} scroll;
		struct {
			int x;
			int y;
			int width;
			int height;
		} resize;
		struct {
			uint32_t id;
			bool cancel;
			char text[OV_INPUT_TEXT_MAX];
		} input;
		struct {
			uint32_t request_id;
			uint16_t length;
			uint8_t data[OV_NET_EVENT_DATA_BYTES];
		} net_data;
		struct {
			uint32_t request_id;
			int error;
		} net_result;
	} payload;
};

struct ov_queue_sync {
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
	void *ctx;
};

struct ov_event_queue {
	struct ov_event entries[OV_EVENT_QUEUE_CAPACITY];
	struct ov_queue_sync sync;
	size_t head;
	size_t tail;
	size_t count;
	uint32_t dropped;
};

struct ov_damage {
	struct ov_rect rects[OV_MAX_DAMAGE_RECTS];
	size_t count;
	bool full;
};

/* The page frame is owned by the OpenVela/LVGL adapter after submit. The
 * platform substrate only records the frame metadata and does not retain the
 * pixel pointer. */
struct ov_frame {
	const void *pixels;
	int width;
	int height;
	int stride_bytes;
	uint64_t serial;
	struct ov_damage damage;
};

struct ov_frontend_ops {
	uint64_t (*now_ms)(void *ctx);
	void (*wake_core)(void *ctx);
	void (*frame_ready)(void *ctx, const struct ov_frame *frame);
	void (*show_status)(void *ctx, const char *text);
	struct ov_queue_sync queue_sync;
	void *ctx;
};

void ov_event_queue_init(struct ov_event_queue *queue,
			 const struct ov_queue_sync *sync);
bool ov_event_push(struct ov_event_queue *queue, const struct ov_event *event);
bool ov_event_pop(struct ov_event_queue *queue, struct ov_event *event);
size_t ov_event_queue_size(struct ov_event_queue *queue);
uint32_t ov_event_queue_dropped(struct ov_event_queue *queue);

void ov_damage_reset(struct ov_damage *damage);
void ov_damage_add(struct ov_damage *damage, struct ov_rect rect,
			   int width, int height);
bool ov_damage_is_empty(const struct ov_damage *damage);

void ov_frontend_init(struct ov_event_queue *queue,
			      const struct ov_frontend_ops *ops);
bool ov_frontend_publish(struct ov_event_queue *queue,
				 const struct ov_event *event,
				 const struct ov_frontend_ops *ops);
bool ov_frontend_publish_touch(struct ov_event_queue *queue,
				       enum ov_event_type type,
				       int x, int y, uint64_t timestamp_ms,
				       const struct ov_frontend_ops *ops);
bool ov_frontend_publish_navigation(struct ov_event_queue *queue,
					    const char *url,
					    uint64_t timestamp_ms,
					    const struct ov_frontend_ops *ops);

#endif
