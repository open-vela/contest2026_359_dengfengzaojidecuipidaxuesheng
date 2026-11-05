#include "script_capture.h"
#include "peripherals.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdlib.h>

#define SYMBOLS (SCRIPT_MAX_PULSES/2)
typedef struct {
    QueueHandle_t done;
    rmt_symbol_word_t symbols[SYMBOLS];
} capture_buffer;
static portMUX_TYPE arm_lock=portMUX_INITIALIZER_UNLOCKED;

static bool receive_done(rmt_channel_handle_t channel,const rmt_rx_done_event_data_t *event,void *user)
{
    (void)channel;
    capture_buffer *buffer=user;
    BaseType_t wake=pdFALSE;
    size_t count=event->num_symbols;
    xQueueSendFromISR(buffer->done,&count,&wake);
    return wake==pdTRUE;
}

int script_capture(unsigned io,unsigned idle_us,script_pulse_t *pulses,size_t *count,
                   int (*cancelled)(void *),void *ctx)
{
    if(!io_valid(io) || !idle_us || idle_us>SCRIPT_TIMED_US || !pulses || !count || !cancelled)return J_BAD_ARG;
    *count=0;
    uint8_t modes[BOARD_IO],leases[BOARD_IO];io_snapshot(modes,leases);
    if(leases[io] || modes[io]!=2)return J_BUSY;
    int64_t started=esp_timer_get_time();
    capture_buffer *buffer=heap_caps_calloc(1,sizeof *buffer,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!buffer)return J_INTERNAL;
    buffer->done=xQueueCreate(1,sizeof(size_t));
    if(!buffer->done){free(buffer);return J_INTERNAL;}
    rmt_channel_handle_t channel=NULL;
    bool enabled=false;
    int rc=J_HARDWARE;
    rmt_rx_channel_config_t config={.gpio_num=BOARD_IO_GPIOS[io],.clk_src=RMT_CLK_SRC_DEFAULT,
        .resolution_hz=1000000,.mem_block_symbols=SYMBOLS};
    esp_err_t error=rmt_new_rx_channel(&config,&channel);
    if(error!=ESP_OK){rc=error==ESP_ERR_NOT_FOUND?J_BUSY:J_HARDWARE;goto cleanup;}
    const rmt_rx_event_callbacks_t callbacks={.on_recv_done=receive_done};
    if(rmt_rx_register_event_callbacks(channel,&callbacks,buffer)!=ESP_OK)goto cleanup;
    if(rmt_enable(channel)!=ESP_OK)goto cleanup;
    enabled=true;
    const rmt_receive_config_t receive={.signal_range_min_ns=1000,.signal_range_max_ns=idle_us*1000};
    if(cancelled(ctx)){rc=J_NOT_READY;goto cleanup;}
    // Arm before releasing the line; prevent scheduling between these two steps.
    // The RMT driver supports rmt_receive from ISR context and allocates nothing here.
    portENTER_CRITICAL(&arm_lock);
    error=rmt_receive(channel,buffer->symbols,sizeof buffer->symbols,&receive);
    if(error==ESP_OK)gpio_set_level(BOARD_IO_GPIOS[io],1);
    portEXIT_CRITICAL(&arm_lock);
    if(error!=ESP_OK)goto cleanup;
    size_t symbols=0;
    while(true){
        if(cancelled(ctx)){rc=J_NOT_READY;goto cleanup;}
        if(esp_timer_get_time()-started>=SCRIPT_CAPTURE_US){rc=J_TIMEOUT;goto cleanup;}
        if(xQueueReceive(buffer->done,&symbols,pdMS_TO_TICKS(2)+1)==pdTRUE)break;
    }
    // A full buffer could be a truncated frame. Never decode it as complete.
    if(symbols>=SYMBOLS){rc=J_OVERFLOW;goto cleanup;}
    for(size_t i=0;i<symbols;i++){
        rmt_symbol_word_t symbol=buffer->symbols[i];
        if(symbol.duration0)pulses[(*count)++]=(script_pulse_t){symbol.duration0,symbol.level0};
        if(symbol.duration1)pulses[(*count)++]=(script_pulse_t){symbol.duration1,symbol.level1};
    }
    rc=cancelled(ctx)?J_NOT_READY:J_OK;
cleanup:
    if(enabled)rmt_disable(channel);
    if(channel)rmt_del_channel(channel);
    vQueueDelete(buffer->done);free(buffer);
    if(cancelled(ctx)){rc=J_NOT_READY;io_config(io,0,0,0);}
    // The GPIO remains released in open-drain mode on success; the service
    // restores floating inputs on errors. No sensor protocol lives here.
    return rc;
}
