/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_frontend.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
int main(int argc,char **argv)
{
 assert(argc==2);struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.tts_short=true};
 const char *full[]={"你好，世界。","检查已经完成，结果一切正常。","温度是23.5度，湿度60%。","你好！再见？","你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好你好"};
 const char *parts[][5]={{"你好，","世界。"},{"检查已经完成，","结果一切正常。"},{"温度是23.5度，","湿度60%。"},{"你好！","再见？"},{"你好你好你好你好你好你好","你好你好你好你好你好你好","你好你好你好你好你好你好","你好你好"}};
 const unsigned counts[]={2,2,2,2,4};
 for(unsigned c=0;c<5;c++){
   struct glv_phone_plan *p=NULL;assert(!glv_frontend_plan(argv[1],full[c],&m,&p));assert(p->count==counts[c]);
   for(unsigned i=0;i<p->count;i++){
     struct glv_memory q={.limit=GLV_MEMORY_LIMIT};struct glv_phone_plan *ref=NULL;assert(!glv_frontend_plan(argv[1],parts[c][i],&q,&ref));
     assert(ref->count==1&&p->parts[i].count==ref->parts[0].count);
     assert(!memcmp(p->parts[i].ids,ref->parts[0].ids,p->parts[i].count*sizeof(int)));
     assert(!memcmp(p->parts[i].kind,ref->parts[0].kind,p->parts[i].count));
     glv_free(ref);assert(!q.used);
   }
   glv_free(p);assert(!m.used);
 }
 const char *punct_cases[]={"，，你好，。。世界。","你好你好你好你好你好你好，世界。"};
 const char *punct_parts[][2]={{"你好，","世界。"},{"你好你好你好你好你好你好，","世界。"}};
 for(unsigned c=0;c<2;c++){
   struct glv_phone_plan *p=NULL;assert(!glv_frontend_plan(argv[1],punct_cases[c],&m,&p)&&p->count==2);
   for(unsigned i=0;i<2;i++){
     struct glv_memory q={.limit=GLV_MEMORY_LIMIT};struct glv_phone_plan *ref=NULL;
     assert(!glv_frontend_plan(argv[1],punct_parts[c][i],&q,&ref)&&ref->count==1);
     assert(p->parts[i].count==ref->parts[0].count&&!memcmp(p->parts[i].ids,ref->parts[0].ids,ref->parts[0].count*sizeof(int)));
     glv_free(ref);assert(!q.used);
   }
   glv_free(p);assert(!m.used);
 }
 struct glv_phone_plan *p=NULL;assert(glv_frontend_plan(argv[1],"，。！？",&m,&p)==-ENODATA&&!p&&!m.used);
 char many[1100]={0};for(unsigned i=0;i<140;i++)strcat(many,"你，");
 assert(glv_frontend_plan(argv[1],many,&m,&p)==-E2BIG&&!p&&!m.used);
 puts("SHORT_FRONTEND_INDEPENDENT_PARTS_NO_DROP_DUPLICATE_NORMALIZE_NUMBERS_PUNCTUATION_CAP_EMPTY_LIMIT_CLEANUP_OK");
 return 0;
}
