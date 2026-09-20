/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MOUSE_CORE_H
#define MOUSE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MOUSE_HOLD_MS 1500u
#define MOUSE_REPORT_ID 2
struct mouse_core
{
  bool enabled, connected, encrypted, suspended, boot;
  bool report_notify, boot_notify, disconnect_required;
  uint8_t buttons;
  uint32_t epoch;
  uint64_t held_at;
  int error;
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
extern const uint8_t mouse_report_map[];
extern const size_t mouse_report_map_size;
#endif
