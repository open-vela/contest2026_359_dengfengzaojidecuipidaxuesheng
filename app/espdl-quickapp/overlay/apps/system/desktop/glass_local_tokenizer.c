/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_tokenizer.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define HASH_SIZE 32768u
#define NORMAL_MAX 32768u

static const uint32_t *section(struct glv_tokenizer *t,size_t *at,unsigned count,unsigned stride)
{
  size_t n=(size_t)count*stride*4;
  if(*at>t->bytes||n>t->bytes-*at)return NULL;
  const uint32_t *p=(const uint32_t *)(t->blob+*at);*at+=n;return p;
}
static unsigned pair_hash(uint32_t a,uint32_t b)
{return ((a*16777619u)^b)&(HASH_SIZE-1);}
void glv_tokenizer_close(struct glv_tokenizer *t)
{glv_free(t->hash);glv_free(t->blob);memset(t,0,sizeof(*t));}
int glv_tokenizer_open(struct glv_tokenizer *t,const char *path,struct glv_memory *memory)
{
  memset(t,0,sizeof(*t));t->memory=memory;FILE *f=NULL;size_t size=0;
  int ret=glv_open_asset(path,&f,&size);if(ret)return ret;
  ret=-EBADMSG;if(size<64||size>2u*1024u*1024u)goto done;
  t->blob=glv_alloc(memory,size);if(!t->blob){ret=-ENOMEM;goto done;}t->bytes=size;
  if(glv_read_exact(f,t->blob,size))goto done;
  const unsigned char *h=t->blob;
  if(glv_u32(h)!=0x3154564c||glv_u32(h+4)!=1||glv_u32(h+8)!=8192)goto done;
  t->nmerges=glv_u32(h+12);t->nmaps=glv_u32(h+16);t->nsequence=glv_u32(h+20);
  t->nccc=glv_u32(h+24);t->ncompose=glv_u32(h+28);t->nclasses=glv_u32(h+32);t->nstrings=glv_u32(h+36);
  if(t->nmerges>8192||t->nmaps>65536||t->nsequence>131072||t->nccc>4096||
     t->ncompose>4096||t->nclasses>8192||t->nstrings>262144)goto done;
  size_t at=64;t->tokens=section(t,&at,8192,2);
  if(!t->tokens||at>size||t->nstrings>size-at)goto done;
  t->strings=t->blob+at;at=(at+t->nstrings+3)&~(size_t)3;
  t->byte_ids=section(t,&at,256,1);t->merges=section(t,&at,t->nmerges,3);
  t->maps=section(t,&at,t->nmaps,3);t->sequence=section(t,&at,t->nsequence,1);
  t->ccc=section(t,&at,t->nccc,2);t->compose=section(t,&at,t->ncompose,3);
  t->classes=section(t,&at,t->nclasses,3);
  if(!t->classes||at!=size)goto done;
  for(unsigned i=0;i<8192;i++)if(t->tokens[i*2]>t->nstrings||t->tokens[i*2+1]>t->nstrings-t->tokens[i*2])goto done;
  for(unsigned i=0;i<256;i++){
    unsigned id=t->byte_ids[i];if(id>=8192||t->tokens[id*2+1]!=1||t->strings[t->tokens[id*2]]!=i)goto done;
  }
  for(unsigned i=0;i<t->nmaps;i++){
    const uint32_t *m=t->maps+i*3;
    if((i&&m[-3]>=m[0])||m[0]>0x10ffff||!m[2]||m[2]>32||m[1]>t->nsequence||m[2]>t->nsequence-m[1])goto done;
  }
  for(unsigned i=0;i<t->nsequence;i++)if(t->sequence[i]>0x10ffff)goto done;
  for(unsigned i=0;i<t->nccc;i++)if((i&&t->ccc[i*2-2]>=t->ccc[i*2])||t->ccc[i*2+1]>255)goto done;
  for(unsigned i=0;i<t->nclasses;i++){
    const uint32_t *c=t->classes+i*3;
    if(c[0]>c[1]||c[1]>0x10ffff||(c[2]&~7u)||(i&&c[-2]>=c[0]))goto done;
  }
  for(unsigned i=0;i<t->ncompose;i++){
    const uint32_t *c=t->compose+i*3;
    if(c[0]>0x10ffff||c[1]>0x10ffff||c[2]>0x10ffff||
       (i&&(c[-3]>c[0]||(c[-3]==c[0]&&c[-2]>=c[1]))))goto done;
  }
  t->hash=glv_alloc(memory,HASH_SIZE*sizeof(uint32_t));if(!t->hash){ret=-ENOMEM;goto done;}
  for(unsigned i=0;i<t->nmerges;i++){
    const uint32_t *p=t->merges+i*3;if(p[0]>=8192||p[1]>=8192||p[2]>=8192)goto done;
    unsigned slot=pair_hash(p[0],p[1]);
    while(t->hash[slot]){
      const uint32_t *old=t->merges+(t->hash[slot]-1)*3;
      if(old[0]==p[0]&&old[1]==p[1])goto done;
      slot=(slot+1)&(HASH_SIZE-1);
    }
    t->hash[slot]=i+1;
  }
  ret=0;
done:
  fclose(f);if(ret)glv_tokenizer_close(t);return ret;
}
static const uint32_t *find_code(const uint32_t *p,unsigned count,unsigned stride,uint32_t cp)
{
  unsigned lo=0,hi=count;
  while(lo<hi){unsigned mid=(lo+hi)/2;if(p[mid*stride]<cp)lo=mid+1;else hi=mid;}
  return lo<count&&p[lo*stride]==cp?p+lo*stride:NULL;
}
static unsigned combining(struct glv_tokenizer *t,uint32_t cp)
{const uint32_t *p=find_code(t->ccc,t->nccc,2,cp);return p?p[1]:0;}
static unsigned char_class(struct glv_tokenizer *t,uint32_t cp)
{
  unsigned lo=0,hi=t->nclasses;
  while(lo<hi){unsigned mid=(lo+hi)/2;if(t->classes[mid*3+1]<cp)lo=mid+1;else hi=mid;}
  return lo<t->nclasses&&t->classes[lo*3]<=cp?t->classes[lo*3+2]:0;
}
static uint32_t compose(struct glv_tokenizer *t,uint32_t a,uint32_t b)
{
  if(a>=0x1100&&a<0x1113&&b>=0x1161&&b<0x1176)return 0xac00+((a-0x1100)*21+b-0x1161)*28;
  if(a>=0xac00&&a<=0xd7a3&&!((a-0xac00)%28)&&b>0x11a7&&b<0x11c3)return a+b-0x11a7;
  unsigned lo=0,hi=t->ncompose;
  while(lo<hi){unsigned mid=(lo+hi)/2;const uint32_t *p=t->compose+mid*3;
    if(p[0]<a||(p[0]==a&&p[1]<b))lo=mid+1;else hi=mid;}
  const uint32_t *p=t->compose+lo*3;
  return lo<t->ncompose&&p[0]==a&&p[1]==b?p[2]:0;
}
int glv_normalize_codepoints(struct glv_tokenizer *t,const unsigned char *input,size_t bytes,uint32_t **result,unsigned *count)
{
  uint32_t *out=glv_alloc(t->memory,NORMAL_MAX*4u);if(!out)return -ENOMEM;
  const unsigned char *end=input+bytes;unsigned n=0;int ret=0;
  while(input<end){
    uint32_t cp,hangul[3];ret=glv_utf8_read(&input,end,&cp);if(ret)goto fail;
    const uint32_t *map=find_code(t->maps,t->nmaps,3,cp),*values=&cp;unsigned length=1;
    if(cp>=0xac00&&cp<=0xd7a3){unsigned s=cp-0xac00;hangul[0]=0x1100+s/588;hangul[1]=0x1161+s%588/28;
      hangul[2]=0x11a7+s%28;values=hangul;length=s%28?3:2;}
    else if(map){values=t->sequence+map[1];length=map[2];}
    if(length>NORMAL_MAX-n){ret=-E2BIG;goto fail;}
    for(unsigned j=0;j<length;j++){
      unsigned cc=combining(t,values[j]),at=n;out[n++]=values[j];
      while(cc&&at&&combining(t,out[at-1])>cc){uint32_t swap=out[at-1];out[at-1]=out[at];out[at--]=swap;}
    }
  }
  if(n){
    unsigned starter=0,write=1,last=0;
    for(unsigned i=1;i<n;i++){
      unsigned cc=combining(t,out[i]);uint32_t merged=compose(t,out[starter],out[i]);
      if(merged&&(!last||last<cc)){out[starter]=merged;continue;}
      if(!cc)starter=write;out[write++]=out[i];last=cc;
    }
    n=write;
  }
  *result=out;*count=n;return 0;
fail:glv_free(out);return ret;
}
static unsigned merge_rank(struct glv_tokenizer *t,uint32_t a,uint32_t b)
{
  unsigned slot=pair_hash(a,b);
  while(t->hash[slot]){unsigned rank=t->hash[slot]-1;const uint32_t *p=t->merges+rank*3;
    if(p[0]==a&&p[1]==b)return rank;slot=(slot+1)&(HASH_SIZE-1);}
  return UINT32_MAX;
}
static int encode_piece(struct glv_tokenizer *t,const uint32_t *cp,unsigned length,int *out,unsigned capacity)
{
  uint32_t *ids=glv_alloc(t->memory,(size_t)length*4*4);if(!ids)return -ENOMEM;
  unsigned n=0;int ret=0;
  for(unsigned i=0;i<length;i++){
    unsigned char raw[4];unsigned bytes=glv_utf8_write(cp[i],raw);
    for(unsigned j=0;j<bytes;j++)ids[n++]=t->byte_ids[raw[j]];
  }
  while(n>1){
    unsigned rank=UINT32_MAX,at=0;
    for(unsigned i=0;i+1<n;i++){unsigned r=merge_rank(t,ids[i],ids[i+1]);if(r<rank){rank=r;at=i;}}
    if(rank==UINT32_MAX)break;
    ids[at]=t->merges[rank*3+2];memmove(ids+at+1,ids+at+2,(n-at-2)*4u);n--;
    if((ret=glv_cancel(t->memory)))goto done;
  }
  if(n>capacity){ret=-E2BIG;goto done;}
  for(unsigned i=0;i<n;i++)out[i]=(int)ids[i];ret=(int)n;
done:glv_free(ids);return ret;
}
static unsigned piece_end(struct glv_tokenizer *t,const uint32_t *cp,unsigned n,unsigned at)
{
  static const char *tail[]={"s","t","re","ve","m","ll","d"};
  if(cp[at]=='\'')for(unsigned j=0;j<7;j++){
    unsigned k=0;while(tail[j][k]&&at+1+k<n&&cp[at+1+k]==(unsigned char)tail[j][k])k++;
    if(!tail[j][k])return at+1+k;
  }
  unsigned begin=at;
  if(cp[at]==' '&&at+1<n&&!(char_class(t,cp[at+1])&4))at++;
  unsigned flags=char_class(t,cp[at]),kind=flags&1?1:flags&2?2:flags&4?4:0;
  unsigned end=at+1;
  while(end<n){unsigned f=char_class(t,cp[end]);unsigned k=f&1?1:f&2?2:f&4?4:0;if(k!=kind)break;end++;}
  if(kind==4&&end<n&&end-begin>1)end--;
  return end;
}
static int encode_span(struct glv_tokenizer *t,const unsigned char *text,size_t bytes,int *ids,unsigned cap)
{
  if(!bytes)return 0;uint32_t *cp=NULL;unsigned count=0,used=0;int ret=glv_normalize_codepoints(t,text,bytes,&cp,&count);
  if(ret)return ret;
  for(unsigned at=0;at<count;){unsigned end=piece_end(t,cp,count,at);
    ret=encode_piece(t,cp+at,end-at,ids+used,cap-used);if(ret<0)goto done;used+=ret;at=end;}
  ret=(int)used;
done:glv_free(cp);return ret;
}
int glv_encode(struct glv_tokenizer *t,const char *text,int *ids,unsigned capacity)
{
  static const char *special[]={"<|pad|>","<|unk|>","<|beginoftext|>","<|im_start|>","<|im_end|>"};
  if(!text||!ids)return -EINVAL;size_t size=strnlen(text,8193);if(size>8192)return -E2BIG;
  const char *at=text,*end=text+size;unsigned used=0;
  while(at<end){
    const char *first=end;int which=-1;
    for(unsigned i=0;i<5;i++){const char *p=strstr(at,special[i]);if(p&&p<first){first=p;which=(int)i;}}
    int n=encode_span(t,(const unsigned char *)at,(size_t)(first-at),ids+used,capacity-used);
    if(n<0)return n;used+=n;if(which<0)break;
    if(used==capacity)return -E2BIG;ids[used++]=which;at=first+strlen(special[which]);
  }
  return (int)used;
}
int glv_decode(struct glv_tokenizer *t,const int *ids,unsigned count,char *out,size_t capacity)
{
  size_t n=0;if(!capacity)return -EINVAL;
  for(unsigned i=0;i<count;i++){
    if(ids[i]<0||ids[i]>=8192)return -EINVAL;
    unsigned offset=t->tokens[ids[i]*2],len=t->tokens[ids[i]*2+1];
    if(len>=capacity-n)return -ENOSPC;
    memcpy(out+n,t->strings+offset,len);n+=len;
  }
  out[n]=0;return (int)n;
}
