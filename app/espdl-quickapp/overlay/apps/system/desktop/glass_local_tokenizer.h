/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_LOCAL_TOKENIZER_H
#define GLASS_LOCAL_TOKENIZER_H
#include "glass_local_model.h"
struct glv_tokenizer {
  struct glv_memory *memory;
  unsigned char *blob;size_t bytes;
  const uint32_t *tokens,*byte_ids,*merges,*maps,*sequence,*ccc,*compose,*classes;
  const unsigned char *strings;
  uint32_t *hash;unsigned nmaps,nsequence,nccc,ncompose,nclasses,nstrings,nmerges;
};
int glv_tokenizer_open(struct glv_tokenizer *,const char *,struct glv_memory *);
void glv_tokenizer_close(struct glv_tokenizer *);
int glv_encode(struct glv_tokenizer *,const char *,int *,unsigned);
int glv_decode(struct glv_tokenizer *,const int *,unsigned,char *,size_t);
int glv_normalize_codepoints(struct glv_tokenizer *,const unsigned char *,size_t,uint32_t **,unsigned *);
#endif
