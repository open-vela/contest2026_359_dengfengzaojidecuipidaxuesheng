/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_sha.h"
#include <errno.h>
#include <string.h>
int glv_test_sha_fault;
atomic_uint glv_test_sha_initializations;
int esp_sha_init(void)
{atomic_fetch_add(&glv_test_sha_initializations,1);return glv_test_sha_fault==1?-ENODEV:glv_test_sha_fault==6?-EBUSY:0;}
void esp_sha256_init(struct esp_sha256_context_s *c){memset(c,0,sizeof(*c));}
int esp_sha256_starts(struct esp_sha256_context_s *c,bool short_hash)
{if(glv_test_sha_fault==2)return -EIO;if(short_hash)return -EINVAL;return SHA256_Init(&c->reference)?0:-EIO;}
int esp_sha256_update(struct esp_sha256_context_s *c,const unsigned char *data,size_t bytes)
{return glv_test_sha_fault==3?-EIO:SHA256_Update(&c->reference,data,bytes)?0:-EIO;}
int esp_sha256_finish(struct esp_sha256_context_s *c,unsigned char out[32])
{if(glv_test_sha_fault==4)return -EIO;if(!SHA256_Final(out,&c->reference))return -EIO;if(glv_test_sha_fault==5)out[0]^=1;return 0;}
