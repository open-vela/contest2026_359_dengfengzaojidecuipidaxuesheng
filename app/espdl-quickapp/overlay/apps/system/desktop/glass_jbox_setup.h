/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define GLASS_JBOX_SETUP_LIMIT 16
enum glass_jbox_setup_phase {
  JBOX_SETUP_IDLE,
  JBOX_SETUP_SCAN,
  JBOX_SETUP_SELECT,
  JBOX_SETUP_BLE,
  JBOX_SETUP_WIFI,
  JBOX_SETUP_FIND,
  JBOX_SETUP_BIND,
  JBOX_SETUP_VERIFY,
  JBOX_SETUP_READY,
  JBOX_SETUP_CANCEL,
  JBOX_SETUP_ERROR
};
struct glass_jbox_setup_device {
  bool bluetooth, pairing_open;
  char name[32], host[16], device_id[13];
};
struct glass_jbox_setup_snapshot {
  bool busy, cancelling, binding_saved;
  unsigned generation, count;
  enum glass_jbox_setup_phase phase;
  int error, cleanup_error;
  struct glass_jbox_setup_device devices[GLASS_JBOX_SETUP_LIMIT];
};
/* Init is called only by the resident desktop task group, never by NSH/UI. */
int glass_jbox_setup_init(void);
int glass_jbox_setup_read(struct glass_jbox_setup_snapshot *);
int glass_jbox_setup_scan(void);
/* Local touch authorizes sharing only the board's currently connected STA
 * credentials with this selected device. No network override or AP fallback. */
int glass_jbox_setup_start(unsigned generation, unsigned index);
void glass_jbox_setup_cancel(void);
