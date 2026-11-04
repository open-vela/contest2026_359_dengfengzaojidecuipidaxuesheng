#include "script_service.h"
#include "script_vm.h"
#include "peripherals.h"
#include "board.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define SLOTS 4u
static script_program_t programs[SLOTS];
static portMUX_TYPE store_lock=portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE timing_lock=portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t generation;

void script_clear_all(void)
{
    portENTER_CRITICAL(&store_lock);
    generation++;
    memset(programs,0,sizeof programs);
    portEXIT_CRITICAL(&store_lock);
}
int script_clear(unsigned slot)
{
    if(slot>=SLOTS)return J_BAD_ARG;
    portENTER_CRITICAL(&store_lock);
    memset(&programs[slot],0,sizeof programs[slot]);
    portEXIT_CRITICAL(&store_lock);
    return J_OK;
}
int script_load(unsigned slot,unsigned argc,const uint8_t *code,size_t bytes)
{
    if(slot>=SLOTS || argc>SCRIPT_MAX_ARGS || !code || !bytes || bytes%4 || bytes>SCRIPT_MAX_OPS*4)return J_BAD_ARG;
    script_program_t *p=calloc(1,sizeof *p);
    if(!p)return J_INTERNAL;
    p->argc=argc;p->count=bytes/4;memcpy(p->code,code,bytes);
    int rc=script_validate(p);
    if(rc==J_OK){
        portENTER_CRITICAL(&store_lock);
        programs[slot]=*p;
        portEXIT_CRITICAL(&store_lock);
    }
    free(p);return rc;
}
typedef struct {uint8_t args[SCRIPT_MAX_ARGS];uint32_t generation;} call_ctx;
static uint32_t now_us(void *ctx){(void)ctx;return (uint32_t)esp_timer_get_time();}
static int cancelled(void *ctx){return ((call_ctx*)ctx)->generation!=generation;}
static int set_mode(void *ctx,unsigned arg,unsigned mode)
{
    call_ctx *c=ctx;
    return io_config(c->args[arg],mode,mode==2?1:0,mode==2?1:0);
}
static int write_pin(void *ctx,unsigned arg,unsigned level)
{
    unsigned io=((call_ctx*)ctx)->args[arg];
    return io_write_mask(1u<<io,level<<io);
}
static int read_pin(void *ctx,unsigned arg,unsigned *level)
{
    unsigned io=((call_ctx*)ctx)->args[arg];
    uint32_t levels=0;
    int rc=io_read_mask(1u<<io,&levels);
    *level=(levels>>io)&1;
    return rc;
}
static void delay_us(void *ctx,unsigned us){(void)ctx;esp_rom_delay_us(us);}
static void sleep_ms(void *ctx,unsigned ms){(void)ctx;vTaskDelay(pdMS_TO_TICKS(ms)+1);}
static void timed(void *ctx,int enter)
{
    (void)ctx;
    if(enter)portENTER_CRITICAL(&timing_lock);
    else portEXIT_CRITICAL(&timing_lock);
}
int script_call(unsigned slot,const uint8_t *args,size_t argc,uint32_t *result,size_t *count)
{
    if(slot>=SLOTS || argc>SCRIPT_MAX_ARGS || !result || !count)return J_BAD_ARG;
    script_program_t *p=malloc(sizeof *p);
    if(!p)return J_INTERNAL;
    call_ctx ctx={0};
    portENTER_CRITICAL(&store_lock);
    *p=programs[slot];ctx.generation=generation;
    portEXIT_CRITICAL(&store_lock);
    int rc=J_OK;
    if(!p->count)rc=J_NOT_READY;
    else if(p->argc!=argc)rc=J_BAD_ARG;
    uint8_t modes[BOARD_IO],leases[BOARD_IO];io_snapshot(modes,leases);
    uint32_t used=0;
    for(size_t i=0;rc==J_OK && i<argc;i++){
        if(!io_valid(args[i]) || (used&(1u<<args[i])))rc=J_BAD_ARG;
        else if(leases[args[i]] || modes[args[i]]==3)rc=J_BUSY;
        else {ctx.args[i]=args[i];used|=1u<<args[i];}
    }
    if(rc==J_OK){
        const script_backend_t b={.ctx=&ctx,.now_us=now_us,.mode=set_mode,
            .write=write_pin,.read=read_pin,.delay_us=delay_us,.sleep_ms=sleep_ms,
            .timed=timed,.cancelled=cancelled};
        rc=script_execute(p,&b,result,count);
        if(rc!=J_OK)for(size_t i=0;i<argc;i++)io_config(args[i],0,0,0);
    }
    free(p);return rc;
}
