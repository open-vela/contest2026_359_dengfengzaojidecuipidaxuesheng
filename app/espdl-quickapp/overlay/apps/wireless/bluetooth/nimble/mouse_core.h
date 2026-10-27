/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MOUSE_CORE_H
#define MOUSE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MOUSE_HOLD_MS 1500u
#define MOUSE_REPORT_ID 2
#define MOUSE_QUEUE_LIMIT 8u
#define MOUSE_REPORT_MS 10u
#define MOUSE_INPUT_MS 250u
#define MOUSE_RELEASE_MS 500u
#define MOUSE_DELTA_LIMIT 1024
struct mouse_frame
{
  int x, y, wheel;
  uint64_t created;
  uint8_t buttons;
  bool edge;
};
struct mouse_core
{
  bool enabled, connected, encrypted, suspended, boot;
  bool report_notify, boot_notify, disconnect_required;
  uint8_t buttons;
  uint32_t epoch;
  uint64_t held_at;
  int error;
  struct mouse_frame frames[MOUSE_QUEUE_LIMIT];
  unsigned head, queued;
  bool release_pending;
  uint64_t retry_at, release_until;
  uint32_t accepted, submitted, merged, retries, discarded, cancelled;
};
typedef int (*mouse_send_fn)(void *arg, const uint8_t *report, size_t length,
                             bool boot);
void mouse_core_connect(struct mouse_core *s);
void mouse_core_disconnect(struct mouse_core *s);
int mouse_core_move(struct mouse_core *s, uint32_t epoch, int buttons,
                    int x, int y, int wheel, uint64_t now,
                    mouse_send_fn send, void *arg);
int mouse_core_release(struct mouse_core *s, mouse_send_fn send, void *arg);
int mouse_core_stop(struct mouse_core *s, mouse_send_fn send, void *arg);
int mouse_core_mode(struct mouse_core *s, unsigned mode,
                    mouse_send_fn send, void *arg);
int mouse_core_suspend(struct mouse_core *s, unsigned value,
                       mouse_send_fn send, void *arg);
int mouse_core_tick(struct mouse_core *s, uint64_t now,
                    mouse_send_fn send, void *arg);
bool mouse_core_transient(int error);
int mouse_core_enqueue(struct mouse_core *s, uint32_t epoch, int buttons,
                       int x, int y, int wheel, uint64_t now);
int mouse_core_enqueue_click(struct mouse_core *s, uint32_t epoch,
                             unsigned button, uint64_t now);
int mouse_core_keepalive(struct mouse_core *s, uint32_t epoch, uint64_t now);
void mouse_core_cancel_input(struct mouse_core *s, uint64_t now);
extern const uint8_t mouse_report_map[];
extern const size_t mouse_report_map_size;
#endif
