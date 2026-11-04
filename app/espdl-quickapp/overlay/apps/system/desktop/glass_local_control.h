/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <cJSON.h>
#define GLC_MAX_RESOURCES 8
struct glc_catalog {cJSON *root;const cJSON *rows;};
struct glc_transport {
 void *opaque;uint32_t revision;
 uint32_t (*current_revision)(void *);
 /* Returns heap-owned JSON from the existing shared Home Assistant tools.
  * Expected revision must reach the atomic service submission check. */
 char *(*call)(void *,const char *,const char *,bool,uint32_t,atomic_bool *);
 atomic_bool *cancel;
};
int glc_open(struct glc_catalog *,const char *);
void glc_close(struct glc_catalog *);
int glc_catalog_valid(const char *);
int glc_prompt(const struct glc_catalog *,const char *,char *,size_t);
int glc_request(const struct glc_catalog *,const char *,char *,size_t,bool *,bool *);
int glc_execute(const struct glc_catalog *,const char *,const char *,const struct glc_transport *,char *,size_t);
