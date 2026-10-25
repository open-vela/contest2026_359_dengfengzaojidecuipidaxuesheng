/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_speech_tts.h"
#define GLASS_ESP_TTS_RATE 16000
/* Official range 0..5. A running phrase retains its starting speed. */
int glass_esp_tts_set_speed(unsigned speed);
unsigned glass_esp_tts_speed(void);
/* Load verified official voice bytes. Release owned after vendor handles are
 * destroyed; cached immutable bytes can serve subsequent phrases. */
int glass_esp_tts_voice_load(const unsigned char **data,void **owned,
                             speech_cancel_fn cancel,void *user);
void glass_esp_tts_voice_release(void *owned);
void glass_esp_tts_cache_clear(void);
/* Nonblocking housekeeping; keep 60 s of reuse after the last borrower. */
size_t glass_esp_tts_cache_trim(void);
const char *glass_esp_tts_voice_name(void);
/* Selection is refused while voice bytes are loading or borrowed. */
int glass_esp_tts_voice_select(const char *name);
/* Worker only. Voice data is immutable flash storage; all mutable vendor
 * state belongs to this invocation and is freed before capture can resume. */
int glass_esp_synthesize(const char *text, speech_pcm_fn write_pcm,
                         speech_cancel_fn cancel, void *user);
