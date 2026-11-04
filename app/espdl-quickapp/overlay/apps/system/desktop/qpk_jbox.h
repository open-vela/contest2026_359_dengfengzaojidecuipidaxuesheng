/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "qpk_hardware.h"
#define JBOX_PORT 4747
#define JBOX_MAX_PAYLOAD 8192
struct jbox_client;
/* Blocking calls belong to the hardware worker, never the LVGL thread. */
int jbox_discover(cJSON *out,unsigned timeout_ms,struct qpk_hw_progress *progress);
int jbox_pair(const char *host,uint8_t key[32],struct qpk_hw_progress *progress);
int jbox_connect(struct jbox_client **client,const char *host,const uint8_t key[32],
                 unsigned timeout_ms,struct qpk_hw_progress *progress);
void jbox_close(struct jbox_client *client);
/* A transport/authentication failure closes the socket; actions are never replayed.
 * Remote errors are returned in status, separate from local errno values. */
int jbox_rpc(struct jbox_client *client,uint8_t command,const void *request,size_t length,
             void *reply,size_t capacity,size_t *received,int32_t *status,
             struct qpk_hw_progress *progress);
bool jbox_connected(const struct jbox_client *client);
int qpk_jbox_execute(void **state,const char *operation,const cJSON *arguments,
                     cJSON *out,struct qpk_hw_progress *progress);
void qpk_jbox_destroy(void *state);
/* Explicit trusted-console pairing; credentials are never returned to JS/AI. */
int qpk_jbox_bind(const char *host,const char *device_id);
