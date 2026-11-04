/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_MATERIAL_H
#define GLASS_MATERIAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "glass_material_ids.h"
#ifdef __cplusplus
extern "C" {
#endif
struct gmat_stats {size_t cache_bytes,blob_bytes,peak_bytes;uint64_t reads,hits,decoded_bytes,errors;unsigned blobs;bool pressure;};
size_t gmat_size(unsigned id);
int gmat_read(unsigned id,size_t offset,void *dst,size_t bytes);
int gmat_verify(unsigned id);
void *gmat_acquire(unsigned id,size_t offset,size_t bytes);
/* Worker-only full-asset load: checked block by block, gives inference priority. */
void *gmat_acquire_background(unsigned id,bool (*cancel)(void));
void gmat_boot_enable(bool enabled);
bool gmat_boot_enabled(void);
void gmat_release(void *data);
FILE *gmat_fopen(unsigned id);
void gmat_pressure(bool active);
bool gmat_under_pressure(void);
void gmat_trim(void);
void gmat_status(struct gmat_stats *out);
/* Safe bounded LZ4 block decoder, shared by host fault tests. */
int gmat_lz4_decode(const unsigned char *,size_t,unsigned char *,size_t);
#ifdef __cplusplus
}
#endif
#endif
