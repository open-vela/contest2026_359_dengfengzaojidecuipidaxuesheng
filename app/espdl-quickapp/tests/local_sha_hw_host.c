/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_model.h"
#include "esp_sha.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,cancel_at;
static bool cancelled(void *unused){(void)unused;return cancel_at&&++checks>=cancel_at;}
static FILE *file_of(const unsigned char *data,size_t bytes)
{FILE *f=tmpfile();assert(f);assert(fwrite(data,1,bytes,f)==bytes);rewind(f);return f;}
static void *exercise(void *unused)
{
  (void)unused;unsigned char data[16399],expected[32],out[32];
  for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i*17+5);
  const unsigned lengths[]={0,1,55,56,63,64,65,127,128,129,4095,4096,4097,16399};
  for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++){
    unsigned n=lengths[i];SHA256(data,n,expected);FILE *f=file_of(data,n);
    struct glv_memory m={.limit=GLV_MEMORY_LIMIT};
    assert(!glv_sha_file(f,n,out,&m));fclose(f);assert(!memcmp(out,expected,32));
    assert(m.profile.sha_hw_backend==1&&m.profile.sha_hw_files==1&&m.profile.sha_hw_selftests==1);
    assert(m.profile.sha_hw_updates==(n+4095)/4096&&m.profile.sha_bytes==n&&!m.used);
  }
  return NULL;
}
int main(int argc,char **argv)
{
  glv_test_sha_fault=argc>1?atoi(argv[1]):0;
  if(glv_test_sha_fault&&glv_test_sha_fault!=6){
    struct glv_memory m={.limit=GLV_MEMORY_LIMIT};unsigned char out[32],zero[32];memset(out,0xa5,32);memcpy(zero,out,32);
    FILE *f=tmpfile();assert(f);int ret=glv_sha_file(f,0,out,&m);fclose(f);
    assert(ret==(glv_test_sha_fault==1?-ENODEV:glv_test_sha_fault==5?-EILSEQ:-EIO));
    assert(!memcmp(out,zero,32)&&!m.profile.sha_hw_files&&!m.profile.sha_hw_selftests&&!m.used);
  }else{
    pthread_t a,b;assert(!pthread_create(&a,NULL,exercise,NULL));assert(!pthread_create(&b,NULL,exercise,NULL));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    unsigned char data[8192]={0},out[32],before[32];memset(out,0xa5,32);memcpy(before,out,32);
    struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled};m.profile.sha_hw_selftests=1;
    FILE *f=file_of(data,sizeof(data));checks=0;cancel_at=3;
    assert(glv_sha_file(f,sizeof(data),out,&m)==-ECANCELED);assert(!memcmp(out,before,32)&&!m.used);fclose(f);cancel_at=0;
    memset(&m,0,sizeof(m));f=file_of(data,17);
    assert(glv_sha_file(f,18,out,&m)==-EIO);assert(!memcmp(out,before,32)&&!m.profile.sha_hw_files&&!m.used);fclose(f);
  }
  assert(atomic_load(&glv_test_sha_initializations)==1);
  puts("SHA_ADAPTER_BOUNDARY_CONCURRENT_CANCEL_SHORT_READ_FAULT_OK");return 0;
}
