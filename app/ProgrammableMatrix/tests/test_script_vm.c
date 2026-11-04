#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "script_vm.h"
#include "route_plan.h"

typedef struct {
    uint32_t clock,release_at,low_at,cancel_at;
    unsigned latch,mode,critical,entries;
    int started,missing;
    uint8_t bytes[5];
} simulator;
static uint32_t clock_us(void *v){return ((simulator*)v)->clock++;}
static int mode(void *v,unsigned arg,unsigned m){assert(arg==0);((simulator*)v)->mode=m;return 0;}
static int write_pin(void *v,unsigned arg,unsigned level)
{
    simulator *s=v;assert(arg==0);
    if(!s->mode)return J_BUSY;
    if(!level)s->low_at=s->clock;
    else if(!s->latch && s->clock-s->low_at>=18000){s->release_at=s->clock;s->started=1;}
    s->latch=level;return 0;
}
static int read_pin(void *v,unsigned arg,unsigned *level)
{
    simulator *s=v;assert(arg==0);
    if(!s->latch){*level=0;return 0;}
    if(s->missing || !s->started){*level=1;return 0;}
    uint32_t t=s->clock-s->release_at;
    if(t<25){*level=1;return 0;}
    if(t<105){*level=0;return 0;}
    if(t<185){*level=1;return 0;}
    t-=185;
    for(unsigned bit=0;bit<40;bit++){
        unsigned high=(s->bytes[bit/8]&(1u<<(7-bit%8)))?70:28;
        if(t<50){*level=0;return 0;}
        t-=50;
        if(t<high){*level=1;return 0;}
        t-=high;
    }
    *level=t>=50;return 0;
}
static void delay(void *v,unsigned us){((simulator*)v)->clock+=us;}
static void sleep_ms(void *v,unsigned ms){((simulator*)v)->clock+=ms*1000;}
static void timed(void *v,int enter)
{
    simulator *s=v;
    if(enter){assert(!s->critical);s->critical=1;s->entries++;}
    else{assert(s->critical);s->critical=0;}
}
static int cancelled(void *v){simulator *s=v;return s->cancel_at && s->clock>=s->cancel_at;}
static script_backend_t backend(simulator *s)
{
    script_backend_t b={s,clock_us,mode,write_pin,read_pin,delay,sleep_ms,timed,cancelled};return b;
}
static void instruction(script_program_t *p,unsigned kind,unsigned value)
{
    uint8_t *op=p->code+4*p->count++;op[0]=kind;op[1]=0;op[2]=value;op[3]=value>>8;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    FILE *file=fopen(argv[1],"rb");assert(file);
    unsigned char header[3];assert(fread(header,1,3,file)==3);
    script_program_t p={.argc=header[0],.count=header[1]|((unsigned)header[2]<<8)};
    assert(p.count<=SCRIPT_MAX_OPS);
    assert(fread(p.code,4,p.count,file)==p.count);fclose(file);
    assert(script_validate(&p)==J_OK);
    simulator s={.latch=1,.bytes={55,0,24,0,79}};
    uint32_t result[SCRIPT_MAX_RESULTS];size_t count=0;
    script_backend_t b=backend(&s);
    assert(script_execute(&p,&b,result,&count)==J_OK);
    assert(count==2 && result[0]==240 && result[1]==550);
    assert(!s.critical && s.entries==1 && s.latch==1);
    s=(simulator){.latch=1,.bytes={55,0,24,0,78}};
    assert(script_execute(&p,&b,result,&count)==J_HARDWARE && count==0);
    assert(!s.critical);
    s=(simulator){.latch=1,.missing=1};
    assert(script_execute(&p,&b,result,&count)==J_TIMEOUT && count==0);
    assert(!s.critical);
    s=(simulator){.latch=1,.cancel_at=22500,.bytes={55,0,24,0,79}};
    assert(script_execute(&p,&b,result,&count)==J_NOT_READY && count==0);
    assert(!s.critical && s.entries==1);

    p=(script_program_t){0};
    instruction(&p,SC_PUSH,65535);instruction(&p,SC_PUSH,2);
    instruction(&p,SC_MUL,0);instruction(&p,SC_EMIT_VALUE,0);
    s=(simulator){0};assert(script_execute(&p,&b,result,&count)==J_OK);
    assert(count==1 && result[0]==131070);
    p=(script_program_t){0};instruction(&p,SC_ADD,0);
    assert(script_validate(&p)==J_BAD_ARG);
    p=(script_program_t){0};instruction(&p,SC_LOAD,0);instruction(&p,SC_EMIT_VALUE,0);
    assert(script_validate(&p)==J_BAD_ARG);
    p=(script_program_t){0};
    instruction(&p,SC_TIMED_BEGIN,0);instruction(&p,SC_SLEEP_MS,20);instruction(&p,SC_TIMED_END,0);
    assert(script_validate(&p)==J_BAD_ARG);
    p=(script_program_t){0};
    instruction(&p,SC_TIMED_BEGIN,0);instruction(&p,SC_DELAY_US,10000);instruction(&p,SC_TIMED_END,0);
    assert(script_validate(&p)==J_BAD_ARG);
    p=(script_program_t){0};
    instruction(&p,SC_PUSH,1);instruction(&p,SC_PUSH,32);
    instruction(&p,SC_SHL,0);instruction(&p,SC_EMIT_VALUE,0);
    assert(script_execute(&p,&b,result,&count)==J_BAD_ARG && count==0);
    puts("VM: uploaded DHT11 waveform/decode/checksum, timeout, cancellation, arithmetic and validation PASS");
}
