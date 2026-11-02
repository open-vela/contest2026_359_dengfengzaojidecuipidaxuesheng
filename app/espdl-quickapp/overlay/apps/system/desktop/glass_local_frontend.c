/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_frontend.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct frontend {
  unsigned char *data;const uint32_t *characters,*phrases,*codepoints,*phones;
  const unsigned char *kind;unsigned nc,np,ncp,nphone,sil,pause;
};
static int load_frontend(struct frontend *s,const char *root,struct glv_memory *memory)
{
  memset(s,0,sizeof(*s));char path[384];FILE *f=NULL;size_t size=0;
  if(snprintf(path,sizeof(path),"%s/tts-v2/frontend.bin",root)>=(int)sizeof(path))return -ENAMETOOLONG;
  int ret=glv_open_asset(path,&f,&size);if(ret)return ret;ret=-EBADMSG;
  if(size<32+261||size>8u*1024u*1024u)goto done;
  s->data=glv_alloc(memory,size);if(!s->data){ret=-ENOMEM;goto done;}
  if(glv_read_exact(f,s->data,size))goto done;
  if(glv_u32(s->data)!=0x31504c47||glv_u32(s->data+4)!=1)goto done;
  s->nc=glv_u32(s->data+8);s->np=glv_u32(s->data+12);s->ncp=glv_u32(s->data+16);s->nphone=glv_u32(s->data+20);
  s->sil=glv_u32(s->data+24);s->pause=glv_u32(s->data+28);
  if(s->nc>65536||s->np>100000||s->ncp>1000000||s->nphone>1000000||s->sil>=261||s->pause>=261)goto done;
  size_t expected=32u+(size_t)s->nc*12+(size_t)s->np*20+(size_t)s->ncp*4+(size_t)s->nphone*4+261;
  if(size!=expected)goto done;
  s->characters=(const uint32_t *)(s->data+32);s->phrases=s->characters+s->nc*3;
  s->codepoints=s->phrases+s->np*5;s->phones=s->codepoints+s->ncp;s->kind=(const unsigned char *)(s->phones+s->nphone);
  for(unsigned i=0;i<s->nc;i++){
    const uint32_t *c=s->characters+i*3;
    if((i&&c[-3]>=c[0])||c[0]>0x10ffff||!c[2]||c[2]>16||c[1]>s->nphone||c[2]>s->nphone-c[1])goto done;
  }
  for(unsigned i=0;i<s->np;i++){
    const uint32_t *p=s->phrases+i*5;
    if((i&&p[-5]>p[0])||!p[1]||p[1]>64||p[2]>s->ncp||p[1]>s->ncp-p[2]||
       p[3]>s->nphone||p[4]>s->nphone-p[3]||!p[4]||p[0]!=s->codepoints[p[2]])goto done;
  }
  for(unsigned i=0;i<s->nphone;i++)if(s->phones[i]>=261)goto done;
  for(unsigned i=0;i<261;i++)if(s->kind[i]>2)goto done;
  ret=0;
done:fclose(f);if(ret){glv_free(s->data);memset(s,0,sizeof(*s));}return ret;
}
static bool punctuation(uint32_t cp)
{
  static const uint32_t chars[]={0x3002,0x3001,0x2014,0x2026,0x201c,0x201d,0x300c,0x300d,
    0x300a,0x300b,0x3010,0x3011,0x2018,0x2019,0xb7};
  if(cp==' '||(cp>=9&&cp<=13)||(cp<128&&strchr(",.!?;:()[]\"'",(int)cp)))return true;
  for(unsigned i=0;i<sizeof(chars)/sizeof(chars[0]);i++)if(cp==chars[i])return true;
  return false;
}
static bool digit(uint32_t c){return c>='0'&&c<='9';}
static int append_cp(uint32_t *out,unsigned *count,uint32_t cp)
{if(*count>=4096)return -E2BIG;out[(*count)++]=cp;return 0;}
static bool latin(uint32_t cp)
{return (cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z');}
static int append_word(uint32_t *out,unsigned *count,const char *word)
{
  const unsigned char *p=(const unsigned char *)word,*end=p+strlen(word);int ret=0;
  while(p<end&&!ret){uint32_t cp;ret=glv_utf8_read(&p,end,&cp);if(!ret)ret=append_cp(out,count,cp);}return ret;
}
static int integer(const uint32_t *digits,unsigned length,bool ordinal,uint32_t *out,unsigned *count)
{
  const uint32_t numbers[]={0x96f6,0x4e00,0x4e8c,0x4e09,0x56db,0x4e94,0x516d,0x4e03,0x516b,0x4e5d};
  const uint32_t small[]={0,0x5341,0x767e,0x5343},large[]={0,0x4e07,0x4ebf,0x5146};int ret=0;
  if(ordinal||length>16||(length>1&&digits[0]=='0')){
    for(unsigned i=0;i<length&&!ret;i++)ret=append_cp(out,count,numbers[digits[i]-'0']);return ret;
  }
  unsigned groups=(length+3)/4;bool wrote=false,gap=false;
  for(int group=(int)groups-1;group>=0&&!ret;group--){
    unsigned begin=length>(unsigned)(group+1)*4?length-(unsigned)(group+1)*4:0;
    unsigned end=length-(unsigned)group*4,value=0;
    for(unsigned i=begin;i<end;i++)value=value*10+digits[i]-'0';
    if(!value){if(wrote)gap=true;continue;}
    if(wrote&&(gap||value<1000)){ret=append_cp(out,count,numbers[0]);if(ret)break;}gap=false;
    unsigned divisor=1000;bool zero=false,group_wrote=false;
    for(int place=3;place>=0&&!ret;place--,divisor/=10){
      unsigned d=value/divisor%10;
      if(!d){if(group_wrote)zero=true;continue;}
      if(zero){ret=append_cp(out,count,numbers[0]);zero=false;if(ret)break;}
      if(!(d==1&&place==1&&!wrote))ret=append_cp(out,count,numbers[d]);
      if(!ret&&place)ret=append_cp(out,count,small[place]);wrote=true;group_wrote=true;
    }
    if(!ret&&group)ret=append_cp(out,count,large[group]);
  }
  if(!ret&&!wrote)ret=append_cp(out,count,numbers[0]);return ret;
}
static int expand_numbers(const uint32_t *cp,unsigned n,uint32_t *out,unsigned *count)
{
  *count=0;int ret=0;
  for(unsigned at=0;at<n&&!ret;){
    /* The fixed V2 voice is Mandarin. Read Latin letters through its existing
     * Chinese dictionary; keep the displayed answer and original phone data.
     * A standalone AI abbreviation uses its Chinese meaning. Unknown scripts
     * still fail explicitly rather than disappearing from the spoken reply. */
    if(latin(cp[at])){
      if(cp[at]=='A'&&at+1<n&&cp[at+1]=='I'&&(!at||!latin(cp[at-1]))&&(at+2==n||!latin(cp[at+2]))){
        ret=append_word(out,count,"人工智能");at+=2;continue;
      }
      static const char *letters[]={"诶","比","西","迪","伊","艾弗","吉","艾尺","艾","杰","开","艾勒","艾姆","恩","欧","批","丘","阿尔","艾丝","提","优","维","达不溜","艾克斯","歪","贼德"};
      unsigned letter=(cp[at]>='a'?cp[at]-'a':cp[at]-'A');ret=append_word(out,count,letters[letter]);at++;continue;
    }
    if(!digit(cp[at])){ret=append_cp(out,count,cp[at++]);continue;}
    unsigned end=at;while(end<n&&digit(cp[end]))end++;
    unsigned decimal=end;
    if(end+1<n&&cp[end]=='.'&&digit(cp[end+1])){decimal=end+1;end=decimal;while(end<n&&digit(cp[end]))end++;}
    bool percent=end<n&&cp[end]=='%',temperature=end+1<n&&cp[end]==0xb0&&cp[end+1]=='C';
    if(percent){ret=append_cp(out,count,0x767e);if(!ret)ret=append_cp(out,count,0x5206);if(!ret)ret=append_cp(out,count,0x4e4b);}
    if(temperature){ret=append_cp(out,count,0x6444);if(!ret)ret=append_cp(out,count,0x6c0f);}
    unsigned whole=decimal!=end?decimal-1:end;
    if(!ret)ret=integer(cp+at,whole-at,end<n&&cp[end]==0x5e74,out,count);
    if(!ret&&decimal!=end){ret=append_cp(out,count,0x70b9);if(!ret)ret=integer(cp+decimal,end-decimal,true,out,count);}
    if(!ret&&temperature)ret=append_cp(out,count,0x5ea6);
    at=end+(percent?1:temperature?2:0);
  }
  return ret;
}
static int add_phone(struct frontend *s,struct glv_phone_part *p,unsigned id)
{
  if(id>=261)return -EBADMSG;if(p->count>=256)return -E2BIG;
  p->ids[p->count]=id;p->kind[p->count++]=s->kind[id];return 0;
}
static int phonemize(struct frontend *s,const uint32_t *cp,unsigned n,struct glv_phone_part *part)
{
  int ret=add_phone(s,part,s->sil);bool spoken=false;
  for(unsigned at=0;at<n&&!ret;){
    if(punctuation(cp[at])){
      if((unsigned)part->ids[part->count-1]!=s->pause)ret=add_phone(s,part,s->pause);at++;continue;
    }
    unsigned lo=0,hi=s->np;
    while(lo<hi){unsigned mid=(lo+hi)/2;if(s->phrases[mid*5]<cp[at])lo=mid+1;else hi=mid;}
    const uint32_t *match=NULL;
    for(unsigned i=lo;i<s->np&&s->phrases[i*5]==cp[at];i++){
      const uint32_t *p=s->phrases+i*5;
      if(p[1]>n-at||(match&&p[1]<=match[1]))continue;
      if(!memcmp(cp+at,s->codepoints+p[2],p[1]*4u))match=p;
    }
    unsigned offset=0,phones=0,take=1;
    if(match){take=match[1];offset=match[3];phones=match[4];}
    else {
      lo=0;hi=s->nc;while(lo<hi){unsigned mid=(lo+hi)/2;if(s->characters[mid*3]<cp[at])lo=mid+1;else hi=mid;}
      if(lo==s->nc||s->characters[lo*3]!=cp[at])return -ENOTSUP;
      offset=s->characters[lo*3+1];phones=s->characters[lo*3+2];
    }
    for(unsigned i=0;i<phones&&!ret;i++)ret=add_phone(s,part,s->phones[offset+i]);at+=take;spoken=true;
  }
  if(!ret&&!spoken)return -ENODATA;
  if(!ret)ret=add_phone(s,part,s->sil);return ret;
}
static bool sentence_end(uint32_t cp)
{return cp==0x3002||cp=='!'||cp=='?'||cp=='\n';}
/* Bound each acoustic pass by a short phrase. Prefer punctuation and never
 * cut a known dictionary phrase or an ASCII word at the character cap. Number
 * expansion and NFKC have already run, so decimal punctuation is not split. */
static bool clause_end(uint32_t c)
{return sentence_end(c)||c==','||c==';'||c==':'||c=='.'||c==0x3001||c==0xff0c||c==0xff1b||c==0xff1a||c==0xff01||c==0xff1f;}
static unsigned atomic_text(struct frontend *s,const uint32_t *cp,unsigned n)
{
  unsigned take=1;
  if(latin(cp[0])){while(take<n&&latin(cp[take]))take++;return take;}
  unsigned lo=0,hi=s->np;
  while(lo<hi){unsigned mid=(lo+hi)/2;if(s->phrases[mid*5]<cp[0])lo=mid+1;else hi=mid;}
  for(unsigned i=lo;i<s->np&&s->phrases[i*5]==cp[0];i++){
    const uint32_t *p=s->phrases+i*5;
    if(p[1]<=n&&p[1]>take&&!memcmp(cp,s->codepoints+p[2],p[1]*4u))take=p[1];
  }
  return take;
}
int glv_frontend_plan(const char *root,const char *text,struct glv_memory *memory,struct glv_phone_plan **result)
{
  *result=NULL;if(!text||strnlen(text,4097)>4096)return -E2BIG;
  struct frontend front={0};struct glv_tokenizer unicode={0};struct glv_phone_plan *plan=NULL;
  uint32_t *original=NULL,*cp=NULL;unsigned count=0,n=0;int ret=load_frontend(&front,root,memory);if(ret)goto done;
  char path[384];if(snprintf(path,sizeof(path),"%s/edgeidiot/tokenizer.bin",root)>=(int)sizeof(path)){ret=-ENAMETOOLONG;goto done;}
  ret=glv_tokenizer_open(&unicode,path,memory);if(ret)goto done;
  ret=glv_normalize_codepoints(&unicode,(const unsigned char *)text,strlen(text),&original,&count);if(ret)goto done;
  if(count>500){ret=-E2BIG;goto done;}
  cp=glv_alloc(memory,4096*4u);if(!cp){ret=-ENOMEM;goto done;}
  ret=expand_numbers(original,count,cp,&n);if(ret)goto done;
  /* Legacy preserves the V2 <=80-character policy; short mode is bounded. */
  unsigned starts[GLV_PHONE_SEGMENTS],lengths[GLV_PHONE_SEGMENTS],segments=0,start=0,used=0,at=0;
  if(memory->tts_short){
    while(at<n){
      start=at;used=0;
      while(at<n){
        if(clause_end(cp[at])){
          do{at++;}while(at<n&&clause_end(cp[at]));break;
        }
        unsigned take=atomic_text(&front,cp+at,n-at);
        if(used&&used+take>12)break;
        at+=take;used+=take;
        if(used>=12){while(at<n&&clause_end(cp[at]))at++;break;}
      }
      if(segments>=GLV_PHONE_SEGMENTS){ret=-E2BIG;goto done;}
      starts[segments]=start;lengths[segments++]=at-start;
    }
  }
  else if(n<=80){starts[0]=0;lengths[0]=n;segments=1;}
  else while(at<n){
    unsigned end=at;while(end<n&&!sentence_end(cp[end]))end++;
    while(end<n&&sentence_end(cp[end]))end++;
    if(used&&used+end-at>80){if(segments>=GLV_PHONE_LEGACY_SEGMENTS){ret=-E2BIG;goto done;}starts[segments]=start;lengths[segments++]=used;used=0;}
    while(end-at>80){
      if(used){if(segments>=GLV_PHONE_LEGACY_SEGMENTS){ret=-E2BIG;goto done;}starts[segments]=start;lengths[segments++]=used;used=0;}
      if(segments>=GLV_PHONE_LEGACY_SEGMENTS){ret=-E2BIG;goto done;}starts[segments]=at;lengths[segments++]=80;at+=80;
    }
    if(!used)start=at;used+=end-at;at=end;
    if(at==n&&used){if(segments>=GLV_PHONE_LEGACY_SEGMENTS){ret=-E2BIG;goto done;}starts[segments]=start;lengths[segments++]=used;}
  }
  unsigned capacity=memory->tts_short?segments:GLV_PHONE_LEGACY_SEGMENTS;
  plan=glv_alloc(memory,sizeof(*plan)+(size_t)capacity*sizeof(plan->parts[0]));
  if(!plan){ret=-ENOMEM;goto done;}
  for(unsigned i=0;i<segments;i++){
    ret=glv_cancel(memory);if(ret)goto done;
    memset(&plan->parts[plan->count],0,sizeof(plan->parts[plan->count]));
    ret=phonemize(&front,cp+starts[i],lengths[i],&plan->parts[plan->count]);
    if(ret==-ENODATA){ret=0;continue;}if(ret)goto done;plan->count++;
  }
  if(!plan->count){ret=-ENODATA;goto done;}*result=plan;plan=NULL;
done:glv_free(original);glv_free(cp);glv_free(plan);glv_free(front.data);glv_tokenizer_close(&unicode);return ret;
}
