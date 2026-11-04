/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
/* Idle-only pressure: 0 healthy, 1 discard KV, 2 retire resident weights. */
int glv_platform_memory_pressure(void);
/* Begin/end only publish GUI cache policy; all LVGL work stays on its owner. */
void glv_platform_memory_begin(void);
void glv_platform_memory_end(void);
int glv_platform_budget(size_t *,size_t *);
int glv_platform_audio_open(void **,bool (*)(void *),void *);
int glv_platform_audio_write(void *,const int16_t *,unsigned);
int glv_platform_audio_finish(void *,bool);
void glv_platform_wake_yield(void);
bool glv_platform_storage_ready(void);
struct glv_storage_stamp {uint32_t writes,media;};
/* Unsupported guard means no cross-request verification reuse. */
int glv_platform_storage_stamp(struct glv_storage_stamp *);
int glv_platform_session_budget(size_t,size_t *,size_t *);
