/* SPDX-License-Identifier: Apache-2.0 */
/* JBoxScript wire-compatible with ProgrammableMatrixS3/client/jbox_lang.py. */
#include "jbox_script.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { LOW=1,HIGH,INPUT,OUTPUT,OPEN_DRAIN,DELAY,SLEEP,WAIT_LOW,WAIT_HIGH,
  CAPTURE_LOW,CAPTURE_HIGH,SAMPLE_EMIT,EMIT_CONST,TIMED_BEGIN,TIMED_END,
  PUSH,LOAD,STORE,ADD,SUB,MUL,SHL,SHR,AND,OR,XOR,GT,EQ,ASSERT,EMIT,
  MEASURE_LOW,MEASURE_HIGH,SAMPLE,CAPTURE,PULSE_COUNT,PULSE_US,PULSE_LEVEL };
struct parser {
  const char *source, *cursor;
  char token[64], locals[16][64];
  unsigned local_count, block_depth, expression_depth;
  int error;
  struct jbox_script_program *out;
};
static bool alpha(unsigned char c)
{return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static bool digit(unsigned char c){return c>='0'&&c<='9';}
static int fail(struct parser *p,int error)
{
  if(!p->error){p->error=error;p->out->error_offset=(size_t)(p->cursor-p->source);}
  return p->error;
}
static void next(struct parser *p)
{
  if(p->error)return;
  const char *s=p->cursor;
  for(;;){
    while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n'||*s=='\v'||*s=='\f')s++;
    if(*s=='#'||(s[0]=='/'&&s[1]=='/')){while(*s&&*s!='\n')s++;continue;}
    break;
  }
  const char *start=s;
  if(alpha((unsigned char)*s)){do{s++;}while(alpha((unsigned char)*s)||digit((unsigned char)*s));}
  else if(digit((unsigned char)*s)){do{s++;}while(digit((unsigned char)*s));}
  else if(*s){
    if(!strchr("{}(),;=+*|^&><-",*s)){p->cursor=s;fail(p,-EINVAL);return;}
    char c=*s++;
    if((c=='='||c=='<'||c=='>')&&*s==c)s++;
    else if(c=='<'){p->cursor=start;fail(p,-EINVAL);return;}
  }
  size_t length=(size_t)(s-start);p->cursor=s;
  if(length>=sizeof(p->token)){fail(p,-E2BIG);return;}
  memcpy(p->token,start,length);p->token[length]=0;
}
static bool is(struct parser *p,const char *s){return !strcmp(p->token,s);}
static int take(struct parser *p,const char *s)
{if(p->error)return p->error;if(!is(p,s))return fail(p,-EINVAL);next(p);return p->error;}
static int identifier(struct parser *p,char name[64])
{
  if(p->error)return p->error;
  if(!alpha((unsigned char)p->token[0]))return fail(p,-EINVAL);
  strcpy(name,p->token);next(p);return p->error;
}
static int number(struct parser *p,unsigned *value)
{
  if(p->error)return p->error;
  if(!digit((unsigned char)p->token[0]))return fail(p,-EINVAL);
  unsigned n=0;
  for(const char *s=p->token;*s;s++){
    if(!digit((unsigned char)*s)||n>6553||(n==6553&&*s>'5'))return fail(p,-ERANGE);
    n=n*10+(unsigned)(*s-'0');
  }
  *value=n;next(p);return p->error;
}
static int emit(struct parser *p,unsigned kind,unsigned arg,unsigned value)
{
  if(p->error)return p->error;
  if(p->out->count>=1024)return fail(p,-E2BIG);
  uint8_t *b=p->out->code+4*p->out->count++;
  b[0]=kind;b[1]=arg;b[2]=value;b[3]=value>>8;return 0;
}
static int pin(struct parser *p,unsigned *index)
{
  if(p->error)return p->error;
  for(unsigned i=0;i<p->out->arguments;i++)if(is(p,p->out->parameters[i])){
    *index=i;next(p);return p->error;
  }
  return fail(p,-EINVAL);
}
static int wait_args(struct parser *p,unsigned *arg,unsigned *high,unsigned *us)
{
  if(pin(p,arg)||take(p,","))return p->error;
  if(is(p,"high")||is(p,"1"))*high=1;
  else if(is(p,"low")||is(p,"0"))*high=0;
  else return fail(p,-EINVAL);
  next(p);
  if(take(p,",")||number(p,us)||take(p,")"))return p->error;
  return 0;
}
static int precedence(const char *s,unsigned *op)
{
  static const struct {const char *text;unsigned kind,rank;} ops[]={
    {"|",OR,1},{"^",XOR,2},{"&",AND,3},{"==",EQ,4},{">",GT,5},
    {"<<",SHL,6},{">>",SHR,6},{"+",ADD,7},{"-",SUB,7},{"*",MUL,8}};
  for(unsigned i=0;i<sizeof(ops)/sizeof(ops[0]);i++)if(!strcmp(s,ops[i].text)){
    *op=ops[i].kind;return (int)ops[i].rank;
  }
  return 0;
}
static int expression(struct parser *p,int minimum)
{
  if(p->error)return p->error;
  if(++p->expression_depth>32)return fail(p,-E2BIG);
  unsigned value=0,arg=0,high=0;
  if(is(p,"(")){
    next(p);if(expression(p,0)||take(p,")"))return p->error;
  }else if(digit((unsigned char)p->token[0])){
    if(number(p,&value)||emit(p,PUSH,0,value))return p->error;
  }else{
    char name[64];if(identifier(p,name))return p->error;
    if(is(p,"(")){
      next(p);
      if(!strcmp(name,"measure")){
        if(wait_args(p,&arg,&high,&value)||emit(p,high?MEASURE_HIGH:MEASURE_LOW,arg,value))return p->error;
      }else if(!strcmp(name,"sample")){
        if(pin(p,&arg)||take(p,")")||emit(p,SAMPLE,arg,0))return p->error;
      }else if(!strcmp(name,"pulse_count")){
        if(take(p,")")||emit(p,PULSE_COUNT,0,0))return p->error;
      }else if(!strcmp(name,"pulse_us")||!strcmp(name,"pulse_level")){
        if(expression(p,0)||take(p,")")||emit(p,!strcmp(name,"pulse_us")?PULSE_US:PULSE_LEVEL,0,0))return p->error;
      }else return fail(p,-EINVAL);
    }else{
      unsigned i=0;while(i<p->local_count&&strcmp(name,p->locals[i]))i++;
      if(i==p->local_count)return fail(p,-EINVAL);
      if(emit(p,LOAD,0,i))return p->error;
    }
  }
  while(!p->error){
    unsigned op=0;int rank=precedence(p->token,&op);if(!rank||rank<minimum)break;
    next(p);if(expression(p,rank+1)||emit(p,op,0,0))return p->error;
  }
  p->expression_depth--;return p->error;
}
static int block(struct parser *p)
{
  if(p->error)return p->error;
  if(++p->block_depth>8)return fail(p,-E2BIG);
  if(take(p,"{"))return p->error;
  while(!is(p,"}")){
    char name[64];unsigned value=0,arg=0,high=0;
    if(identifier(p,name))return p->error;
    if(!strcmp(name,"repeat")){
      if(take(p,"(")||number(p,&value)||take(p,")"))return p->error;
      if(value<1||value>128)return fail(p,-ERANGE);
      unsigned start=p->out->count;
      if(block(p))return p->error;
      unsigned length=p->out->count-start;
      if(length && value-1>(1024-p->out->count)/length)return fail(p,-E2BIG);
      for(unsigned i=1;i<value;i++){
        memcpy(p->out->code+4*p->out->count,p->out->code+4*start,4*length);
        p->out->count+=length;
      }
    }else if(!strcmp(name,"timed")){
      if(emit(p,TIMED_BEGIN,0,0)||block(p)||emit(p,TIMED_END,0,0))return p->error;
    }else if(!strcmp(name,"let")){
      if(p->local_count>=16||identifier(p,name))return fail(p,-E2BIG);
      for(unsigned i=0;i<p->local_count;i++)if(!strcmp(name,p->locals[i]))return fail(p,-EINVAL);
      for(unsigned i=0;i<p->out->arguments;i++)if(!strcmp(name,p->out->parameters[i]))return fail(p,-EINVAL);
      if(take(p,"=")||expression(p,0)||take(p,";"))return p->error;
      strcpy(p->locals[p->local_count],name);
      if(emit(p,STORE,0,p->local_count++))return p->error;
    }else{
      unsigned local=0;while(local<p->local_count&&strcmp(name,p->locals[local]))local++;
      if(local<p->local_count&&is(p,"=")){
        next(p);if(expression(p,0)||take(p,";")||emit(p,STORE,0,local))return p->error;
        continue;
      }
      if(take(p,"("))return p->error;
      unsigned op=0;
      if(!strcmp(name,"emit")||!strcmp(name,"assert")){
        if(expression(p,0)||take(p,")"))return p->error;
        op=!strcmp(name,"emit")?EMIT:ASSERT;
      }else if(!strcmp(name,"low")||!strcmp(name,"high")||!strcmp(name,"release")||
          !strcmp(name,"input")||!strcmp(name,"output")||!strcmp(name,"open_drain")){
        if(pin(p,&arg)||take(p,")"))return p->error;
        op=!strcmp(name,"low")?LOW:!strcmp(name,"input")?INPUT:
          !strcmp(name,"output")?OUTPUT:!strcmp(name,"open_drain")?OPEN_DRAIN:HIGH;
      }else if(!strcmp(name,"capture")){
        if(pin(p,&arg)||take(p,",")||number(p,&value)||take(p,")"))return p->error;
        op=CAPTURE;
      }else if(!strcmp(name,"delay_us")||!strcmp(name,"sleep_ms")){
        if(number(p,&value)||take(p,")"))return p->error;
        op=!strcmp(name,"delay_us")?DELAY:SLEEP;
      }else if(!strcmp(name,"wait")){
        if(wait_args(p,&arg,&high,&value))return p->error;
        op=high?WAIT_HIGH:WAIT_LOW;
      }else return fail(p,-EINVAL);
      if(take(p,";")||emit(p,op,arg,value))return p->error;
    }
  }
  if(take(p,"}"))return p->error;
  p->block_depth--;return 0;
}
/* Independent static bounds before any upload; S3 validates again. */
static int validate(struct parser *p)
{
  unsigned total=0,section=0,outputs=0,stack=0,initialized=0;
  bool timed=false,captured=false;
  for(unsigned i=0;i<p->out->count;i++){
    uint8_t *b=p->out->code+4*i;unsigned op=b[0],v=b[2]|((unsigned)b[3]<<8),cost=1;
    if(op==TIMED_BEGIN){if(timed)return fail(p,-EINVAL);timed=true;section=0;}
    else if(op==TIMED_END){if(!timed)return fail(p,-EINVAL);timed=false;}
    else if(op==SLEEP){if(timed||!v||v>250)return fail(p,-ERANGE);cost+=v*1000;}
    else if(op==CAPTURE){if(timed||!v||v>10000)return fail(p,-ERANGE);cost+=50000;captured=true;}
    else if(op==DELAY||op==WAIT_LOW||op==WAIT_HIGH||op==MEASURE_LOW||op==MEASURE_HIGH){
      if(!v||v>10000)return fail(p,-ERANGE);
      cost+=v;
    }
    if(timed&&(op==INPUT||op==OUTPUT||op==OPEN_DRAIN))return fail(p,-EINVAL);
    if(op>=PULSE_COUNT&&!captured)return fail(p,-EINVAL);
    if((op==PULSE_US||op==PULSE_LEVEL)&&!stack)return fail(p,-EINVAL);
    if(op==PUSH||op==SAMPLE||op==MEASURE_LOW||op==MEASURE_HIGH||op==PULSE_COUNT)stack++;
    if(op==LOAD){if(!(initialized&(1u<<v)))return fail(p,-EINVAL);stack++;}
    if(op==STORE){if(!stack)return fail(p,-EINVAL);stack--;initialized|=1u<<v;}
    if(op>=ADD&&op<=EQ){if(stack<2)return fail(p,-EINVAL);stack--;}
    if(op==ASSERT||op==EMIT){if(!stack)return fail(p,-EINVAL);stack--;}
    if(stack>32)return fail(p,-E2BIG);
    if(op==EMIT)outputs++;
    total+=cost;if(timed)section+=cost;
    if(total>500000||section>10000||outputs>128)return fail(p,-E2BIG);
  }
  return timed||stack?fail(p,-EINVAL):0;
}
int jbox_script_compile(const char *source,struct jbox_script_program *out)
{
  if(!source||!out)return -EINVAL;
  memset(out,0,sizeof(*out));
  if(strnlen(source,JBOX_SCRIPT_SOURCE_MAX+1)>JBOX_SCRIPT_SOURCE_MAX)return -E2BIG;
  struct parser *p=calloc(1,sizeof(*p));if(!p)return -ENOMEM;
  p->source=p->cursor=source;p->out=out;next(p);
  int ret=take(p,"fn");
  if(!ret)ret=identifier(p,out->name);
  if(!ret)ret=take(p,"(");
  if(!ret&&!is(p,")"))for(;;){
    if(out->arguments>=4){ret=fail(p,-E2BIG);break;}
    char *name=out->parameters[out->arguments];ret=identifier(p,name);if(ret)break;
    for(unsigned i=0;i<out->arguments;i++)if(!strcmp(name,out->parameters[i]))ret=fail(p,-EINVAL);
    if(ret)break;
    out->arguments++;
    if(!is(p,","))break;
    next(p);
  }
  if(!ret)ret=take(p,")");
  if(!ret)ret=block(p);
  if(!ret&&(p->token[0]||!out->count))ret=fail(p,-EINVAL);
  if(!ret)ret=validate(p);
  if(ret){memset(out->code,0,sizeof(out->code));out->count=out->arguments=0;}
  free(p);return ret;
}

#include "jbox_script_examples.h"
const char *jbox_script_example(const char *name)
{return name&&!strcmp(name,"dht11")?jbox_dht11_source:NULL;}
