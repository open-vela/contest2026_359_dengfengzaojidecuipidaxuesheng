/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
#ifndef GLV_CHAT_ROOT
#ifdef __NuttX__
#define GLV_CHAT_ROOT "/sdcard/local-voice/models/edgeidiot-9m9-w8a8-20261102"
#else
/* Existing engine fixtures may deliberately bind both roles to one fixture. */
#define GLV_CHAT_ROOT GLV_RELEASE_ROOT
#endif
#endif
#ifndef GLV_RELEASE_ROOT
#define GLV_RELEASE_ROOT "/sdcard/local-voice/models/edgeidiot-9m9-w8a8-9000-20261102"
#endif
#ifdef __cplusplus
extern "C" {
#endif
int glv_assets_check(const char *,struct glv_memory *,char *,size_t);
enum glv_asset_group {GLV_ASSET_LLM=1,GLV_ASSET_TTS=2,GLV_ASSET_WAKE=4,GLV_ASSET_ALL=7};
int glv_assets_check_group(const char *,unsigned,struct glv_memory *,char *,size_t);
int glv_asset_read_small(const char *,const char *,size_t,struct glv_memory *,void **,size_t *);
int glv_asset_check(const char *,const char *,struct glv_memory *);
struct glv_asset_lease;
/* Keep exact open-file identities for a storage-generation-bound session.
 * Validation is metadata-only and is never a substitute for the initial SHA. */
int glv_asset_lease_open(const char *,struct glv_memory *,struct glv_asset_lease **);
int glv_asset_lease_validate(struct glv_asset_lease *);
void glv_asset_lease_close(struct glv_asset_lease *);
#ifdef __cplusplus
}
#endif
