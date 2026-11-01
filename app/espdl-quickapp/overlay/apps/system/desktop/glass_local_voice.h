/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "glass_local_model.h"
enum glv_phase {GLV_IDLE,GLV_CHECKING,GLV_LOADING_LLM,GLV_GENERATING,GLV_FRONTEND,GLV_LOADING_TTS,GLV_SYNTHESIZING,GLV_SPEAKING,GLV_STOPPING,GLV_FAILED,GLV_LOADING_ASR,GLV_TRANSCRIBING,GLV_RECORDING,GLV_CHECKING_ASR,GLV_TOKENIZING,GLV_PREFILL,GLV_PHASE_COUNT};
enum glv_cancel_source {GLV_CANCEL_NONE,GLV_CANCEL_API,GLV_CANCEL_UI_STOP,GLV_CANCEL_UI_NEW,GLV_CANCEL_CLI_STOP,GLV_CANCEL_CLI_NEW,GLV_CANCEL_CONFIG,GLV_CANCEL_VIEW_CLOSE,GLV_CANCEL_STORAGE,GLV_CANCEL_UNATTRIBUTED,GLV_CANCEL_AUDIO};
enum glv_mode {GLV_DIALOGUE,GLV_READ};
struct glv_snapshot {
  uint32_t request,revision;enum glv_phase phase;
  bool busy,models_ready,asr_ready,local_wake,wake_enabled,config_pending;
  int error,model_error;unsigned tokens,elapsed_ms,first_token_ms,first_audio_ms;
  size_t memory_peak;char detail[128];
  int asr_error;unsigned input_level,capture_ms,asr_tokens,asr_first_token_ms,asr_elapsed_ms;
  bool capture_pending;
  enum glv_cancel_source cancel_source;
  enum glv_phase cancel_phase;
  unsigned cancel_ms,stage_ms[GLV_PHASE_COUNT],progress_done,progress_total;
  unsigned q4_bits,q4_matrix_calls,q4_dot_calls;
  bool q4_selftest_passed;
  bool tts_single_dot;
  bool tts_serial_fp;
  bool tts_fma;
  bool tts_bulk_rows;
  bool tts_window_plan;
  bool tts_parallel_input;
  bool tts_parallel_pie;
  bool tts_weight_lut;
  bool tts_int8; /* Diagnostic only; worker admits W8A8 for no-play job9. */
  bool tts_int8_reduce; /* Default-off, no-play full-row S8 diagnostic. */
  bool tts_quant_fast;
  bool tts_short;
  bool tts_coarse_profile;
  unsigned tts_bench_limit_kib;
  struct glv_profile profile;
  int wake_error;
  bool session_resident,session_pending;
  size_t session_bytes;
};
/* Initialize only from the persistent desktop task, before opening CLI entrypoints. */
int glass_local_voice_start(void);
int glass_local_voice_send(const char *,enum glv_mode,uint32_t *);
/* Diagnostic fixed text synthesis, same buffering but no audio device open. */
int glass_local_voice_tts_benchmark(const char *,uint32_t *);
int glass_local_voice_tts_kernel(bool);
int glass_local_voice_tts_parallel(bool);
/* Volatile diagnostic only, default off; explicit FMA changes FP rounding. */
int glass_local_voice_tts_fma(bool);
int glass_local_voice_tts_rows(bool);
int glass_local_voice_tts_windows(bool);
int glass_local_voice_tts_input(bool);
int glass_local_voice_tts_pie_parallel(bool);
int glass_local_voice_tts_weight_lut(bool);
int glass_local_voice_tts_int8(bool);
int glass_local_voice_tts_int8_reduce(bool);
int glass_local_voice_tts_quant_fast(bool);
int glass_local_voice_tts_short(bool);
int glass_local_voice_tts_profile(bool);
int glass_local_voice_tts_bench_limit(unsigned);
int glass_local_voice_check(uint32_t *);
int glass_local_voice_new(void);
int glass_local_voice_save(uint32_t *);
/* Volatile diagnostic backend choice. Default scalar; PIE requires selftest. */
int glass_local_voice_q4_selftest(uint32_t *);
int glass_local_voice_q4_backend(unsigned);
/* Staged board gate, not the microphone entry and does not set asr_ready. */
int glass_local_voice_asr_test(unsigned,uint32_t *);
int glass_local_voice_listen(uint32_t *);
int glass_local_voice_finish_capture(uint32_t);
bool glass_local_voice_get_transcript(uint32_t,char *,size_t);
void glass_local_voice_stop(uint32_t);
void glass_local_voice_cancel(void);
/* Retire retained model state on its owner worker; never free from the UI. */
void glass_local_voice_release(void);
int glass_local_voice_yield_model(unsigned);
/* First cancellation cause wins; stale request IDs cannot cancel a new job. */
void glass_local_voice_stop_from(uint32_t,enum glv_cancel_source);
void glass_local_voice_cancel_from(enum glv_cancel_source);
int glass_local_voice_new_from(enum glv_cancel_source);
bool glass_local_voice_config_idle(void);
void glass_local_voice_get(struct glv_snapshot *,char *,size_t);
bool glass_local_voice_wake_selected(void);
bool glass_local_voice_wake_enabled(void);
bool glass_local_voice_wake_ready(void);
void glass_local_voice_wake_failure(int);
int glass_local_voice_set_wake(bool);
int glass_local_voice_enable_wake(bool);
const char *glass_local_voice_error(int);
