/* Include the production engine to compare its complete final logits. */
#include "glass_local_llm.c"
#include <assert.h>
static bool cancel_now;
static bool is_cancelled(void *p){(void)p;return cancel_now;}
static int cancel_sink(void *p,const char *s,unsigned n){(void)p;(void)s;(void)n;cancel_now=true;return -ECANCELED;}
static void run(const char *root,const char *prompt,bool tight,char *text,float *logits)
{
 struct glv_memory m={.limit=GLV_MEMORY_LIMIT};struct glv_llm *s=NULL;
 assert(!glv_llm_open(root,&m,&s));assert(glv_llm_resident_bytes(s)==10046336);
 if(tight)m.limit=m.used+768u*1024u;
 int ret=glv_llm_generate_tool(s,prompt,8,text,4096,NULL,NULL);fprintf(stderr,"MEMORY_CASE tight=%u result=%d used=%zu limit=%zu\n",tight,ret,m.used,m.limit);assert(ret>0);
 assert(!s->model.block_data&&!s->model.block_bytes);
 fprintf(stderr,"BLOCK_IO tight=%u reads=%llu bytes=%llu loads=%llu hits=%llu peak=%lu\n",tight,(unsigned long long)m.profile.weight_read_calls,(unsigned long long)m.profile.weight_bytes,(unsigned long long)m.profile.llm_block_loads,(unsigned long long)m.profile.llm_block_hits,(unsigned long)m.profile.llm_block_peak_bytes);
 memcpy(logits,s->logits,8192*sizeof(float));
 assert((glv_llm_resident_bytes(s)==0)==tight);
 assert(m.profile.llm_w8a8_calls>0);
 if(tight){assert(!glv_model_block_reserve(&s->model,24u*1024u));glv_llm_trim_kv(s);assert(!s->model.block_data&&!s->keys&&!s->values);}
 glv_llm_close(s);assert(m.used==0);
}
int main(int argc,char **argv)
{
 assert(argc==3);FILE *f=fopen(argv[2],"rb");assert(f);char prompt[8192];size_t n=fread(prompt,1,sizeof(prompt)-1,f);fclose(f);prompt[n]=0;
 char normal[4096],streamed[4096];float a[8192],b[8192];
 run(argv[1],prompt,false,normal,a);run(argv[1],prompt,true,streamed,b);
 assert(!strcmp(normal,streamed));assert(!memcmp(a,b,sizeof(a)));
 struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=is_cancelled};struct glv_llm *s=NULL;assert(!glv_llm_open(argv[1],&m,&s));m.limit=m.used+768u*1024u;
 assert(glv_llm_generate_tool(s,prompt,8,streamed,sizeof(streamed),cancel_sink,NULL)==-ECANCELED);
 glv_llm_close(s);assert(m.used==0);
 puts("MEMORY_FALLBACK_BIT_EXACT_OK complete8192-logits/8-token-text/resident-to-streamed/unchanged-W8A8/cancellation/zero-live-allocations");return 0;
}
