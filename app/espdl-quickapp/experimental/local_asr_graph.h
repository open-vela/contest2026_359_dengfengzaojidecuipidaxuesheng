/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLV_EXPERIMENTAL_ASR_GRAPH_H
#define GLV_EXPERIMENTAL_ASR_GRAPH_H
#include "local_asr_ops.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Fixed N=1 Zipformer graph. Model constants and the plan are SD assets, never
 * linked into firmware. Inputs and outputs are borrowed for one synchronous
 * run; the caller owns/counts them. All graph metadata/intermediates/scratch
 * use the same glv_memory. No Python/ONNX runtime is needed by this API.
 * Caller must validate the external resource manifest (including plan SHA)
 * before open. This loader additionally bounds the plan and hashes data. */
struct glv_asr_graph;
struct glv_asr_io {void *data;size_t bytes;};
struct glv_asr_io_stats {uint64_t bytes,reads,seeks;};
int glv_asr_graph_open(struct glv_asr_graph **,const char *,struct glv_memory *);
void glv_asr_graph_close(struct glv_asr_graph *);
int glv_asr_graph_run(struct glv_asr_graph *,const struct glv_asr_io *,unsigned,
                       const struct glv_asr_io *,unsigned);
int glv_asr_graph_exchange(struct glv_asr_graph *,struct glv_asr_io *,unsigned,struct glv_asr_io *,unsigned);
unsigned glv_asr_graph_inputs(const struct glv_asr_graph *);
unsigned glv_asr_graph_outputs(const struct glv_asr_graph *);
/* Last node index is diagnostic only and never a continuation/retry token. */
unsigned glv_asr_graph_last_node(const struct glv_asr_graph *);
int glv_asr_graph_io_info(const struct glv_asr_graph *,bool,unsigned,
                          struct glv_asr_shape *,unsigned *,size_t *);
void glv_asr_graph_io_statistics(const struct glv_asr_graph *,struct glv_asr_io_stats *);
/* Optional packed-data cache, worker-owned and fully counted. Keeps weights
 * external on SD; only the bounded selected graph is copied to PSRAM. */
int glv_asr_graph_cache(struct glv_asr_graph *,size_t);
/* Required full packed residency; verified RAM, closes backing file. */
int glv_asr_graph_make_resident(struct glv_asr_graph *);
/* Optional prefix cache, <=6MiB, budget-sized and evictable for scratch. */
int glv_asr_graph_hot_prefix(struct glv_asr_graph *,size_t);
/* Owner-only, before inference; 0 disables optional prefetch. Max 1 MiB. */
int glv_asr_graph_block_limit(struct glv_asr_graph *,size_t);
size_t glv_asr_graph_cached_bytes(const struct glv_asr_graph *);
#ifdef __cplusplus
}
#endif
#endif
