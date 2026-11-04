/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "qpk_volume.h"
#include "glass_store_http.h"
#define GLASS_STORE_MAX 24
#define GLASS_STORE_SOURCE_MAX (64u * 1024u)
enum glass_store_action {GLASS_STORE_REFRESH, GLASS_STORE_INSTALL, GLASS_STORE_REMOVE, GLASS_STORE_CONFIGURE};
struct glass_store_entry {
  char package[48],name[64],version[24],summary[192],sha256[65];
  unsigned bytes,category,icon;
  bool local,cloud;
  /* Bit 0: internal, bit 1: SD. Conflicts remain visible but cannot be removed. */
  unsigned installed,managed;
};
/* Bounded native presentation presets. Catalog data never supplies UI code. */
struct glass_store_feature {
  char package[48],title[96],subtitle[128],value[24],unit[24];
  unsigned art,tone; /* art: timer/bars/none; tone: forest/sand/lavender */
};
struct glass_store_snapshot {
  unsigned revision,count;
  bool busy,sd_repository;
  int result,catalog_error;
  int cloud_error;
  bool cloud_repository,local_home;
  char cloud_url[GLASS_STORE_URL_CAP];
  int completed_action;
  uint64_t free_bytes[2];
  char headline[96],tagline[128];
  unsigned featured_count;
  struct glass_store_feature featured[2];
  struct glass_store_entry entries[GLASS_STORE_MAX];
};
/* Called by the long-lived desktop UI task. No filesystem work in these APIs.
 * One bounded worker; close cancels and releases the catalog after completion. */
int glass_store_open(void);
void glass_store_close(void);
void glass_store_get(struct glass_store_snapshot *out);
int glass_store_submit(enum glass_store_action action,unsigned index,enum qpk_volume volume);
void glass_store_cancel(void);
/* Empty URL explicitly disables the remote source. Saved in internal config. */
int glass_store_configure(const char *base_url);
