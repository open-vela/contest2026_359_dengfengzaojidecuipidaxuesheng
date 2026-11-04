#include "glass_material.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
 unsigned slices=0;
 for(unsigned id=0;id<75;id++){
  size_t n=gmat_size(id);assert(n);unsigned char *reference=malloc(n),*part=malloc(n);assert(reference&&part);
  assert(!gmat_verify(id)&&!gmat_read(id,0,reference,n));
  for(unsigned k=0;k<20;k++){
   size_t at=(size_t)k*(n/20),count=n-at;if(count>17001)count=17001;
   assert(!gmat_read(id,at,part,count)&&!memcmp(reference+at,part,count));slices++;
  }
  assert(gmat_read(id,n,part,1)==-EINVAL);free(reference);free(part);
 }
 void *lease=gmat_acquire(0,16370,200);assert(lease);unsigned char saved[200];memcpy(saved,lease,200);
 gmat_pressure(true);assert(!memcmp(saved,lease,200));gmat_release(lease);gmat_pressure(false);gmat_trim();
 struct gmat_stats s;gmat_status(&s);assert(!s.cache_bytes&&!s.blobs&&!s.blob_bytes);
 printf("MATERIAL_CACHE_OK assets=75 slices=%u pressure_lease_stable=1 cache=0 blobs=0\n",slices);return 0;
}
