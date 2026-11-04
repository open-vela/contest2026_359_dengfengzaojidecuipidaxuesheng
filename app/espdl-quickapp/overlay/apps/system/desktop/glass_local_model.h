/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_LOCAL_MODEL_H
#define GLASS_LOCAL_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GLV_ABI 1
#define GLV_MEMORY_LIMIT (16u*1024u*1024u)
#define GLV_SYSTEM_RESERVE (2u*1024u*1024u)
#define GLV_MAX_CONTEXT 512
#define GLV_MAX_REPLY 4096
#define GLV_MATBATCH_MAX 64u
/* NuttX math.h expands isfinite through a double HUGE_VAL expression. RV32F
 * then performs a software double multiply for every decoded coefficient.
 * The builtin preserves IEEE NaN/Inf rejection using the native float class. */
static inline bool glv_finite(float value){return __builtin_isfinite(value);}
enum glv_llm_progress {GLV_LLM_TOKENIZING,GLV_LLM_PREFILL,GLV_LLM_DECODING};
struct glv_verified_asset;
struct glv_row_cache;
struct glv_profile {
  unsigned llm_w8a8_calls,llm_w8a8_selftests;
  uint64_t llm_matrix_ticks,llm_quant_ticks,llm_row_ticks,llm_check_ticks,llm_compute_ticks,llm_prepare_ticks;
  uint64_t llm_fast_rows,llm_checkpoints;
  unsigned llm_spec_blocks,llm_spec_drafted,llm_spec_accepted,llm_spec_fallbacks,llm_spec_previous,llm_spec_prompt;
  uint64_t llm_planar_rows,llm_repack_ticks,llm_attn_qk_ticks,llm_attn_softmax_ticks,llm_attn_value_ticks,llm_head_ticks;
  uint64_t llm_quant_reuses,llm_attention_ticks,llm_norm_ticks,llm_activation_ticks;
  size_t llm_resident_bytes,llm_kv_bytes;
  uint64_t llm_w8a8_dots;
  unsigned llm_q8_pie_calls;
  uint64_t llm_q8_pie_dots;
  uint64_t sha_bytes,sha_read_us,sha_compute_us;
  unsigned sha_hw_backend,sha_hw_files,sha_hw_updates,sha_hw_selftests;
  uint64_t tts_parallel_us,tts_parallel_overlap_us;
  unsigned tts_parallel_jobs,tts_parallel_fallbacks,tts_parallel_cpu0,tts_parallel_cpu1,tts_parallel_stack_peak;
  unsigned tts_parallel_mlp_jobs;
  unsigned tts_input_max_jobs,tts_input_quant_jobs,tts_input_rounding_fallbacks;
  unsigned tts_pie_parallel_eligible,tts_pie_parallel_jobs,tts_pie_context_checks;
  unsigned tts_pie_parallel_scratch_fallbacks,tts_pie_parallel_rounding_fallbacks;
  uint64_t tts_weight_lut_hits,tts_weight_lut_misses;
  unsigned tts_weight_lut_rows,tts_weight_lut_rounding_fallbacks;
  unsigned tts_fma_upsample_calls,tts_fma_rejections;
  unsigned tts_parallel_owner0,tts_parallel_owner1,tts_parallel_pin_retries;
  unsigned tts_parallel_reasons[7],tts_parallel_last_reason,tts_parallel_last_error;
  uint64_t tts_parallel_setup_us,tts_parallel_join_us;
  uint64_t weight_bytes,weight_read_us,weight_seek_us,weight_decode_us,matmul_us;
  uint64_t weight_read_calls,weight_q8_bulk_rows,weight_q8_legacy_rows;
  uint64_t weight_cache_peak_bytes,weight_cache_hits,weight_cache_hit_bytes;
  uint64_t llm_block_loads,llm_block_hits,llm_block_read_bytes;
  size_t llm_block_peak_bytes;
  uint64_t asr_pie_dots,asr_pie_fallbacks,asr_pie_selftests,asr_pie_selftest_failures;
  uint64_t asr_parallel_jobs,asr_parallel_workers,asr_parallel_fallbacks,asr_parallel_cpu0,asr_parallel_cpu1,asr_parallel_overlap_us;
  uint64_t asr_block_loads,asr_block_hits,asr_block_bytes,asr_block_peak_bytes;
  uint64_t asr_io_us,asr_rows_us,asr_quant_us,asr_math_us,asr_conv_us,asr_wait_us;
  uint64_t asr_encoder_us,asr_decoder_us,asr_joiner_us,asr_front_us,asr_op_us[28];
  uint64_t asr_hot_peak_bytes,asr_hot_hits,asr_hot_evictions;
  uint64_t asr_dsp_calls,asr_dsp_elements,asr_dsp_selftests,asr_dsp_fallbacks;
  unsigned verified_reuses;
  unsigned kv_reused_tokens,prefill_tokens,session_reuses,llm_kv_retries;
  uint64_t tts_mel_us,tts_vocoder_us;
  unsigned tts_chunks,tts_context_frames,tts_output_frames,tts_memory_fallbacks;
  unsigned tts_window_checks,tts_window_skips,tts_window_attempts,tts_window_failed_frames;
  uint64_t tts_window_failed_us;
  uint64_t tts_pie_us,tts_pie_dot_calls;
  uint64_t tts_pie_input_us,tts_pie_weights_us,tts_pie_pack_us,tts_pie_compute_us,tts_pie_output_us,tts_upsample_us;
  unsigned tts_pie_batches,tts_wave_fnv;
  unsigned tts_pie_calls,tts_pie_selftests,tts_samples,tts_ready_ms;
  unsigned tts_int8_calls,tts_int8_kept_calls,tts_int8_selftests,tts_int16_selftests;
  unsigned tts_int8_reduce_calls,tts_int8_reduce_selftests,tts_int8_fast_input_calls;
  uint64_t tts_int8_conversions_saved,tts_int8_input_conversions_saved;
  unsigned tts_quant_fast_calls,tts_quant_fast_selftests;
  uint64_t tts_quant_fast_input_saved;
  unsigned tts_clauses,tts_planned_clauses,tts_first_clause_ms,tts_peak_buffered_samples;
  unsigned tts_upsample_pair_calls,tts_upsample_pair_fallbacks;
  uint64_t tts_upsample_pack_us,tts_upsample_compute_us,tts_upsample_stage_us[4];
};
struct glv_memory {
  size_t used,peak,limit;
  bool (*cancelled)(void *);
  void *opaque;
  /* Optional, coarse-grained observation; never called per coefficient. */
  void (*llm_progress)(void *,enum glv_llm_progress,unsigned,unsigned);
  /* Per-request experiment; zero keeps the original FP32 accumulation. */
  unsigned q4_bits,q4_matrix_calls,q4_dot_calls;
  bool tts_single_dot;
  bool tts_serial_fp;
  bool tts_fma;
  bool tts_bulk_rows;
  bool tts_window_plan;
  bool tts_parallel_input;
  bool tts_parallel_pie;
  bool tts_weight_lut;
  bool tts_int8; /* Diagnostic only; worker admits W8A8 for no-play job9. */
  /* Volatile diagnostic: omit only per-eight-patch phase clocks. */
  bool tts_coarse_profile;
  /* Request-local ownership only: never reuse a verification across jobs. */
  bool retain_verified;
  /* Opt-in only for an explicitly retained, media-bound LLM session. */
  bool reuse_kv;
  struct glv_verified_asset *verified;
  struct glv_profile profile;
  unsigned char tts_base_digest[32];
  unsigned char *tts_int8_policy;
  void *tts_int8_owner;
  bool tts_int8_reduce; /* Job9-only full-row S8 accumulation, default off. */
  bool tts_quant_fast; /* Exact stable W8A16 input quantization. */
  bool tts_short; /* Short clauses, sequential synthesis/playback. */
  bool tts_upsample_pair; /* Exact dual-output FP32 upsample, diagnostic default off. */
  /* ASR-only, per request. Zero enables the compiled accelerated paths. */
  bool asr_reference,asr_serial,asr_no_pie,asr_no_blocks,asr_pool_unavailable;
  int asr_pie_state;
  int asr_dsp_state;
  void *asr_pool;
};
struct glv_tensor {
  char name[80];uint32_t shape[4],ndim,type,offset,bytes,stride;
};
struct glv_model {
  struct glv_memory *memory;
  struct glv_tensor *tensors;
  unsigned count;size_t bytes,cursor;
  FILE *file;unsigned char *data,*io;
  /* TTS-only opt-in. Other model consumers retain their existing decoder. */
  bool bulk_q8;
  /* Immutable private resident LLM: descriptor/scales/S8 range checked once. */
  bool w8a8_validated;
  /* Private authenticated Q8 rows: aligned codes plane, then scale plane.
   * One bit per directory entry; partial preparation remains well defined. */
  unsigned char w8a8_planar[256];
  void *data_storage;
  void *w8a8_workspace;
  const float *w8a8_input;
  unsigned w8a8_batch,w8a8_columns;
  /* Optional raw rows; owned by this open model, never shared across jobs. */
  struct glv_row_cache *cache,*cache_last;
  const struct glv_tensor *cache_last_tensor;
  size_t cache_bytes;
  /* LLM-only packed tensor blocks. One owner, reusable buffer, no new math.
   * KV and activations remain separate and mandatory. No async SD worker. */
  unsigned char *block_data;
  size_t block_capacity,block_offset,block_bytes;
  const struct glv_tensor *block_tensor;
};
size_t glv_alloc_charge(size_t);
void *glv_alloc(struct glv_memory *,size_t);
void glv_free(void *);
int glv_cancel(struct glv_memory *);
int glv_sha_file(FILE *,size_t,unsigned char[32],struct glv_memory *);
int glv_sha_buffer(const void *,size_t,unsigned char[32],struct glv_memory *);
uint64_t glv_now_us(void);
int glv_verified_keep(struct glv_memory *,const char *,FILE *,unsigned char *,size_t,const unsigned char[32]);
void glv_verified_clear(struct glv_memory *);
int glv_model_open(struct glv_model *,const char *,struct glv_memory *,bool);
void glv_model_close(struct glv_model *);
/* Reserve optional storage only; rows populate after successful normal reads.
 * Caller reserves mandatory KV/workspace first. Failure leaves streaming usable. */
int glv_model_cache_tensor(struct glv_model *,const char *);
void glv_model_cache_clear(struct glv_model *);
int glv_model_block_reserve(struct glv_model *,size_t);
void glv_model_block_select(struct glv_model *,const char *);
void glv_model_block_release(struct glv_model *);
const struct glv_tensor *glv_tensor(struct glv_model *,const char *);
bool glv_model_q8_planar(const struct glv_model *,const struct glv_tensor *);
int glv_tensor_row(struct glv_model *,const struct glv_tensor *,unsigned,float *,unsigned);
/* Complete, still-packed Q4 row; shares cache, cancellation and IO accounting. */
int glv_tensor_q4_packed(struct glv_model *,const struct glv_tensor *,unsigned,void *,size_t);
/* Q8 scale+codes only, same row I/O policy; no float expansion. Caller owns
 * count+4 bytes. Quantized math/overflow checks remain the consumer's job. */
int glv_tensor_q8_packed(struct glv_model *,const struct glv_tensor *,unsigned,void *,size_t,unsigned);
int glv_vector(struct glv_model *,const char *,float *,unsigned);
int glv_matvec(struct glv_model *,const char *,const float *,float *,unsigned,unsigned);
int glv_matbatch(struct glv_model *,const char *,const float *,float *,unsigned,unsigned,unsigned);
int glv_read_exact(FILE *,void *,size_t);
int glv_open_asset(const char *,FILE **,size_t *);
uint32_t glv_u32(const unsigned char *);
int glv_utf8_read(const unsigned char **,const unsigned char *,uint32_t *);
unsigned glv_utf8_write(uint32_t,unsigned char *);
#ifdef __cplusplus
}
#endif
#endif
