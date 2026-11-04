/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_LOCAL_LLM_H
#define GLASS_LOCAL_LLM_H
#include "glass_local_tokenizer.h"
struct glv_llm;
typedef int (*glv_text_sink)(void *,const char *,unsigned);
int glv_llm_open(const char *,struct glv_memory *,struct glv_llm **);
void glv_llm_close(struct glv_llm *);
/* Owner worker only: discard optional token state, retain authenticated INT8. */
size_t glv_llm_trim_kv(struct glv_llm *);
size_t glv_llm_resident_bytes(const struct glv_llm *);
size_t glv_llm_kv_bytes(const struct glv_llm *);
int glv_llm_forward(struct glv_llm *,int,float *);
int glv_llm_prefill(struct glv_llm *,const int *,unsigned,float *);
int glv_llm_generate(struct glv_llm *,const char *,unsigned,char *,size_t,glv_text_sink,void *);
/* Tool protocol uses raw greedy decoding, without chat repetition penalties.
 * This changes decoding only; callers still validate calls against the catalog. */
int glv_llm_generate_tool(struct glv_llm *,const char *,unsigned,char *,size_t,glv_text_sink,void *);
int glv_llm_encode(struct glv_llm *,const char *,int *,unsigned);
#endif
