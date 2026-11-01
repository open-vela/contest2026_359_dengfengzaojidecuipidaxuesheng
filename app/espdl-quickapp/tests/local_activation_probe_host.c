/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only oracle masks AFTER SiLU*up. This is not a sparse SD backend. */
#include "glass_local_llm.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct activation_stat {
  unsigned long long vectors,values,zero,small[4],groups,skippable_groups;
  double magnitude,removed;
};
static struct activation_stat stats[2][12];
static unsigned mask_mode,percent;
struct ranked {float magnitude;unsigned index;};
static int compare(const void *a,const void *b)
{
  const struct ranked *x=a,*y=b;
  if(x->magnitude<y->magnitude)return -1;if(x->magnitude>y->magnitude)return 1;
  return x->index<y->index?-1:x->index>y->index;
}
void glv_activation_probe(unsigned layer,float *values,unsigned width,unsigned batch,int prefill)
{
  assert(layer<12&&width==1408&&batch&&batch<=32);
  struct activation_stat *s=&stats[prefill?0:1][layer];
  bool unused[44];for(unsigned g=0;g<44;g++)unused[g]=true;
  for(unsigned b=0;b<batch;b++){
    float *v=values+b*width,max=0;bool drop[1408]={0};struct ranked order[1408];
    for(unsigned i=0;i<width;i++){
      float a=fabsf(v[i]);assert(glv_finite(a));if(a>max)max=a;
      s->magnitude+=a;s->zero+=a==0;
    }
    s->vectors++;s->values+=width;
    for(unsigned i=0;i<width;i++){
      float a=fabsf(v[i]);const float ratio[4]={.0001f,.001f,.01f,.1f};
      for(unsigned j=0;j<4;j++)s->small[j]+=a<=max*ratio[j];
    }
    unsigned entries=mask_mode==2?44:width;
    for(unsigned i=0;i<entries;i++){
      float a=0;
      if(mask_mode==2){for(unsigned j=0;j<32;j++)a+=fabsf(v[i*32+j]);}
      else a=fabsf(v[i]);
      order[i]=(struct ranked){a,i};
    }
    if(mask_mode){
      qsort(order,entries,sizeof(*order),compare);
      for(unsigned i=0;i<entries*percent/100;i++){
        unsigned first=order[i].index*(mask_mode==2?32:1),n=mask_mode==2?32:1;
        for(unsigned j=0;j<n;j++)drop[first+j]=true;
      }
    }
    for(unsigned i=0;i<width;i++){
      if(drop[i]){s->removed+=fabsf(v[i]);v[i]=0;}
      if(v[i]!=0)unused[i/32]=false;
    }
  }
  /* Prefill weights are shared across all positions in a batch. A group is
   * skippable only if EVERY position has that entire group zeroed. */
  s->groups+=44;
  for(unsigned g=0;g<44;g++)s->skippable_groups+=unused[g];
}
static void json_string(const char *text)
{
  putchar('"');for(const unsigned char *p=(const unsigned char *)text;*p;p++){
    if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}
    else if(*p<32)printf("\\u%04x",*p);else putchar(*p);
  }putchar('"');
}
int main(int argc,char **argv)
{
  assert(argc==6);mask_mode=(unsigned)strtoul(argv[2],NULL,10);percent=(unsigned)strtoul(argv[3],NULL,10);
  assert(mask_mode<=2&&percent<=50);
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};struct glv_llm *llm=NULL;
  assert(!glv_llm_open(argv[1],&memory,&llm));char reply[4096];
  int ret=glv_llm_generate(llm,argv[4],(unsigned)strtoul(argv[5],NULL,10),reply,sizeof(reply),NULL,NULL);
  assert(ret>=0);glv_llm_close(llm);assert(!memory.used);
  printf("{\"reply\":");json_string(reply);printf(",\"reply_bytes\":%d,\"peak_bytes\":%zu,\"layers\":[",ret,memory.peak);
  for(unsigned p=0;p<2;p++)for(unsigned l=0;l<12;l++){
    struct activation_stat *s=&stats[p][l];
    if(p||l)putchar(',');
    printf("{\"prefill\":%s,\"layer\":%u,\"vectors\":%llu,\"values\":%llu,\"exact_zero\":%llu,"
           "\"relative_small\":[%llu,%llu,%llu,%llu],\"abs_sum\":%.9g,\"removed_abs_sum\":%.9g,"
           "\"q4_groups\":%llu,\"skippable_q4_groups\":%llu}",p?"false":"true",l,s->vectors,s->values,s->zero,
           s->small[0],s->small[1],s->small[2],s->small[3],s->magnitude,s->removed,s->groups,s->skippable_groups);
  }
  puts("]}");return 0;
}
