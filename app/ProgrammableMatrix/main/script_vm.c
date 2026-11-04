#include "script_vm.h"
#include "route_plan.h"

static unsigned operand(const uint8_t *op) {return op[2]|((unsigned)op[3]<<8);}
static int pin_op(unsigned op) {
    return op==SC_LOW || op==SC_HIGH || op==SC_INPUT || op==SC_OUTPUT ||
           op==SC_OPEN_DRAIN || (op>=SC_WAIT_LOW && op<=SC_SAMPLE) ||
           op==SC_MEASURE_LOW || op==SC_MEASURE_HIGH || op==SC_SAMPLE_VALUE;
}
static int wait_op(unsigned op) {
    return (op>=SC_WAIT_LOW && op<=SC_CAPTURE_HIGH) || op==SC_MEASURE_LOW || op==SC_MEASURE_HIGH;
}

int script_validate(const script_program_t *p)
{
    if(!p || p->argc>SCRIPT_MAX_ARGS || !p->count || p->count>SCRIPT_MAX_OPS)return J_BAD_ARG;
    unsigned total=0,section=0,outputs=0,stack=0,initialized=0;
    int timed=0;
    for(unsigned i=0;i<p->count;i++) {
        const uint8_t *op=p->code+4*i;
        unsigned kind=op[0],arg=op[1],v=operand(op),cost=1;
        if(kind<SC_LOW || kind>SC_SAMPLE_VALUE)return J_BAD_ARG;
        if(pin_op(kind) ? arg>=p->argc : arg!=0)return J_BAD_ARG;
        if(kind==SC_TIMED_BEGIN) {
            if(timed || v)return J_BAD_ARG;
            timed=1;section=0;
        }else if(kind==SC_TIMED_END) {
            if(!timed || v)return J_BAD_ARG;
            timed=0;
        }else if(kind==SC_SLEEP_MS) {
            if(timed || !v || v>250)return J_BAD_ARG;
            cost+=v*1000;
        }else if(kind==SC_DELAY_US || wait_op(kind)) {
            if(!v || v>SCRIPT_TIMED_US)return J_BAD_ARG;
            cost+=v;
        }else if(kind==SC_LOAD || kind==SC_STORE) {
            if(v>=16)return J_BAD_ARG;
        }else if(kind!=SC_EMIT && kind!=SC_PUSH && v) {
            return J_BAD_ARG;
        }
        if(timed && (kind==SC_INPUT || kind==SC_OUTPUT || kind==SC_OPEN_DRAIN))return J_BAD_ARG;
        if(kind==SC_PUSH || kind==SC_SAMPLE_VALUE || kind==SC_MEASURE_LOW || kind==SC_MEASURE_HIGH)stack++;
        if(kind==SC_LOAD){if(!(initialized&(1u<<v)))return J_BAD_ARG;stack++;}
        if(kind==SC_STORE){if(!stack)return J_BAD_ARG;stack--;initialized|=1u<<v;}
        if(kind>=SC_ADD && kind<=SC_EQ){if(stack<2)return J_BAD_ARG;stack--;}
        if(kind==SC_ASSERT || kind==SC_EMIT_VALUE){if(!stack)return J_BAD_ARG;stack--;}
        if(stack>32)return J_BAD_ARG;
        if(kind==SC_CAPTURE_LOW || kind==SC_CAPTURE_HIGH || kind==SC_SAMPLE || kind==SC_EMIT || kind==SC_EMIT_VALUE)outputs++;
        total+=cost;
        if(timed)section+=cost;
        if(total>SCRIPT_MAX_TIME_US || section>SCRIPT_TIMED_US || outputs>SCRIPT_MAX_RESULTS)return J_BAD_ARG;
    }
    return (timed || stack)?J_BAD_ARG:J_OK;
}

int script_execute(const script_program_t *p,const script_backend_t *b,
                   uint32_t result[SCRIPT_MAX_RESULTS],size_t *count)
{
    int rc=script_validate(p);
    if(rc!=J_OK)return rc;
    if(!b || !result || !count || !b->now_us || !b->mode || !b->write ||
       !b->read || !b->delay_us || !b->sleep_ms || !b->timed || !b->cancelled)return J_BAD_ARG;
    *count=0;
    uint32_t started=b->now_us(b->ctx),section_start=0;
    int timed=0;
    uint32_t stack[32]={0},locals[16]={0};unsigned sp=0;
    for(unsigned i=0;i<p->count;i++) {
        const uint8_t *op=p->code+4*i;
        unsigned kind=op[0],arg=op[1],v=operand(op),level=0;
        uint32_t elapsed=0;
        if(b->cancelled(b->ctx)){rc=J_NOT_READY;break;}
        if((uint32_t)(b->now_us(b->ctx)-started)>SCRIPT_MAX_TIME_US ||
           (timed && (uint32_t)(b->now_us(b->ctx)-section_start)>SCRIPT_TIMED_US)){
            rc=J_TIMEOUT;break;
        }
        switch(kind) {
        case SC_LOW:case SC_HIGH:
            rc=b->write(b->ctx,arg,kind==SC_HIGH);break;
        case SC_INPUT:case SC_OUTPUT:case SC_OPEN_DRAIN:
            rc=b->mode(b->ctx,arg,kind==SC_INPUT?0:kind==SC_OUTPUT?1:2);break;
        case SC_DELAY_US:b->delay_us(b->ctx,v);break;
        case SC_SLEEP_MS:b->sleep_ms(b->ctx,v);break;
        case SC_SAMPLE:
            rc=b->read(b->ctx,arg,&level);
            if(rc==J_OK)result[(*count)++]=level;
            break;
        case SC_EMIT:result[(*count)++]=v;break;
        case SC_PUSH:stack[sp++]=v;break;
        case SC_LOAD:stack[sp++]=locals[v];break;
        case SC_STORE:locals[v]=stack[--sp];break;
        case SC_ASSERT:if(!stack[--sp])rc=J_HARDWARE;break;
        case SC_EMIT_VALUE:result[(*count)++]=stack[--sp];break;
        case SC_SAMPLE_VALUE:
            rc=b->read(b->ctx,arg,&level);
            if(rc==J_OK)stack[sp++]=level;
            break;
        case SC_ADD:case SC_SUB:case SC_MUL:case SC_SHL:case SC_SHR:
        case SC_AND:case SC_OR:case SC_XOR:case SC_GT:case SC_EQ: {
            uint32_t right=stack[--sp],left=stack[sp-1],value=0;
            if((kind==SC_SHL || kind==SC_SHR) && right>31){rc=J_BAD_ARG;break;}
            switch(kind){
            case SC_ADD:value=left+right;break;
            case SC_SUB:value=left-right;break;
            case SC_MUL:value=left*right;break;
            case SC_SHL:value=left<<right;break;
            case SC_SHR:value=left>>right;break;
            case SC_AND:value=left&right;break;
            case SC_OR:value=left|right;break;
            case SC_XOR:value=left^right;break;
            case SC_GT:value=left>right;break;
            case SC_EQ:value=left==right;break;
            }
            stack[sp-1]=value;break;
        }
        case SC_TIMED_BEGIN:
            b->timed(b->ctx,1);timed=1;section_start=b->now_us(b->ctx);break;
        case SC_TIMED_END:b->timed(b->ctx,0);timed=0;break;
        default: {
            unsigned target=(kind==SC_WAIT_HIGH || kind==SC_CAPTURE_HIGH || kind==SC_MEASURE_HIGH);
            uint32_t start=b->now_us(b->ctx);
            do {
                rc=b->read(b->ctx,arg,&level);
                elapsed=b->now_us(b->ctx)-start;
                if(rc!=J_OK || level==target)break;
            }while(elapsed<v && !b->cancelled(b->ctx));
            if(rc==J_OK && b->cancelled(b->ctx))rc=J_NOT_READY;
            if(rc==J_OK && (level!=target || elapsed>v))rc=J_TIMEOUT;
            if(rc==J_OK && (kind==SC_CAPTURE_LOW || kind==SC_CAPTURE_HIGH))result[(*count)++]=elapsed;
            if(rc==J_OK && (kind==SC_MEASURE_LOW || kind==SC_MEASURE_HIGH))stack[sp++]=elapsed;
            break;
        }
        }
        if(rc!=J_OK)break;
    }
    if(timed)b->timed(b->ctx,0);
    if(rc==J_OK && (uint32_t)(b->now_us(b->ctx)-started)>SCRIPT_MAX_TIME_US)rc=J_TIMEOUT;
    if(rc!=J_OK)*count=0;
    return rc;
}
