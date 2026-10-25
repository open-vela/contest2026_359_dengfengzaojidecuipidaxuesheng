/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {void *pointer;size_t bytes;} allocated[16];
static size_t used,peak;
static unsigned calls,fail_at;
static void *record_allocation(void *pointer,size_t bytes)
{
  if(!pointer)return NULL;
  for(unsigned i=0;i<16;i++)if(!allocated[i].pointer){
    allocated[i].pointer=pointer;allocated[i].bytes=bytes;used+=bytes;
    if(used>peak)peak=used;return pointer;
  }
  abort();
}
static void *checked_malloc(size_t bytes)
{
  if(++calls==fail_at)return NULL;
  return record_allocation(malloc(bytes),bytes);
}
static void *checked_calloc(size_t count,size_t bytes)
{
  if(++calls==fail_at)return NULL;
  return record_allocation(calloc(count,bytes),count*bytes);
}
static void checked_free(void *pointer)
{
  if(!pointer)return;
  for(unsigned i=0;i<16;i++)if(allocated[i].pointer==pointer){
    used-=allocated[i].bytes;allocated[i].pointer=NULL;free(pointer);return;
  }
  assert(!"unowned/double free");
}
#define malloc checked_malloc
#define calloc checked_calloc
#define free checked_free
#include "glass_custom_wake.c"
#undef malloc
#undef calloc
#undef free
int main(int argc,char **argv)
{
  assert(argc==2);struct glass_custom_wake *state=NULL;
  assert(!glass_custom_wake_create(argv[1],&state)&&state);
  unsigned allocations=calls;size_t steady=used,maximum=peak;
  glass_custom_wake_destroy(state);assert(!used);
  for(unsigned failure=1;failure<=allocations;failure++){
    calls=0;fail_at=failure;state=(void *)1;
    assert(glass_custom_wake_create(argv[1],&state)==-ENOMEM);
    assert(!state&&!used);
  }
  calls=0;fail_at=0;assert(!glass_custom_wake_create(argv[1],&state));
  glass_custom_wake_destroy(state);assert(!used);
  printf("WAKE_MODEL_MEMORY {\"allocations\":%u,\"peak_requested_bytes\":%zu,\"steady_requested_bytes\":%zu,\"failure_paths\":%u}\n",
    allocations,maximum,steady,allocations);
  return 0;
}
