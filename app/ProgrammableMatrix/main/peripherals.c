#include "peripherals.h"
#include "board.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_IO_MASK ((1u<<BOARD_IO)-1u)
#define MAX_XFER 4096u
static const char *TAG="io";
static uint8_t iomode[BOARD_IO]; // 0 input, 1 push-pull, 2 OD, 3 ADC1
static uint8_t lease[BOARD_IO]; // 0 free; (1..2) i2c, (3..4) spi, (5..6) uart, (7..10) pwm
static adc_oneshot_unit_handle_t adc_handle;
static bool adc_configured[10];

typedef struct { bool live;uint8_t sda,scl;i2c_master_bus_handle_t h;
                 i2c_master_dev_handle_t dev[128]; } i2c_ctx;
static i2c_ctx is[2];
typedef struct {bool live;uint8_t mosi,miso,sck,cs;spi_device_handle_t dev;} spi_ctx;
static spi_ctx ss[2];
typedef struct {bool live;uint8_t tx,rx;} uart_ctx;
static uart_ctx us[2];
typedef struct {bool live;uint8_t io;} pwm_ctx;
static pwm_ctx ps[4];

static int free_io(unsigned io,unsigned owner) {
    return io_valid(io) && (lease[io]==0 || lease[io]==owner);
}
static int unique4(int a,int b,int c,int d) {
    int v[4]={a,b,c,d};
    for(int i=0;i<4;i++) for(int j=i+1;j<4;j++)
        if(v[i]>=0 && v[i]==v[j]) return 0;
    return 1;
}
static void release_io(unsigned io) {
    if(io_valid(io)) {
        lease[io]=0;
        gpio_reset_pin(BOARD_IO_GPIOS[io]);
        gpio_set_direction(BOARD_IO_GPIOS[io],GPIO_MODE_INPUT);
        gpio_set_pull_mode(BOARD_IO_GPIOS[io], GPIO_FLOATING);
        iomode[io]=0;
    }
}
void peripherals_init(void)
{
    memset(iomode,0,sizeof iomode);memset(lease,0,sizeof lease);
    for(unsigned i=0;i<BOARD_IO;i++) release_io(i);
    adc_oneshot_unit_init_cfg_t acfg={.unit_id=ADC_UNIT_1,.ulp_mode=ADC_ULP_MODE_DISABLE};
    if(adc_oneshot_new_unit(&acfg,&adc_handle)!=ESP_OK) {
        ESP_LOGW(TAG,"ADC1 unavailable; ADC requests will fail");
        adc_handle=NULL;
    }
}
void io_snapshot(uint8_t modes[BOARD_IO],uint8_t leases[BOARD_IO])
{
    memcpy(modes,iomode,BOARD_IO);memcpy(leases,lease,BOARD_IO);
}
int io_config(unsigned id,unsigned mode,unsigned pull,unsigned initial)
{
    if(!io_valid(id)||mode>3||pull>2||initial>1) return J_BAD_ARG;
    if(lease[id]) return J_BUSY;
    if(mode==3 && (BOARD_IO_GPIOS[id]<1 || BOARD_IO_GPIOS[id]>10 || !adc_handle)) return J_UNAVAILABLE;
    int pin=BOARD_IO_GPIOS[id];
    if(mode==3) {
        adc_channel_t ch=(adc_channel_t)(pin-1);
        adc_oneshot_chan_cfg_t cfg={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
        if(adc_oneshot_config_channel(adc_handle,ch,&cfg)!=ESP_OK) return J_HARDWARE;
        adc_configured[pin-1]=true;
    } else {
        gpio_reset_pin(pin);
        // Set the output latch BEFORE selecting output direction.
        gpio_set_level(pin,initial);
        // Keep input enabled so IO_READ and scripts can observe output pins.
        gpio_mode_t m=mode==0?GPIO_MODE_INPUT:mode==1?GPIO_MODE_INPUT_OUTPUT:GPIO_MODE_INPUT_OUTPUT_OD;
        if(gpio_set_direction(pin,m)!=ESP_OK) return J_HARDWARE;
        gpio_pull_mode_t p=pull==0?GPIO_FLOATING:pull==1?GPIO_PULLUP_ONLY:GPIO_PULLDOWN_ONLY;
        if(gpio_set_pull_mode(pin,p)!=ESP_OK) return J_HARDWARE;
    }
    iomode[id]=mode;
    return J_OK;
}
int io_write_mask(uint32_t mask,uint32_t levels)
{
    if(mask & ~MAX_IO_MASK) return J_BAD_ARG;
    for(unsigned i=0;i<BOARD_IO;i++) if((mask>>i)&1u)
        if(lease[i] || (iomode[i]!=1 && iomode[i]!=2)) return J_BUSY;
    for(unsigned i=0;i<BOARD_IO;i++) if((mask>>i)&1u)
        gpio_set_level(BOARD_IO_GPIOS[i],(levels>>i)&1u);
    return J_OK;
}
int io_read_mask(uint32_t mask,uint32_t *levels)
{
    if(!levels || (mask & ~MAX_IO_MASK)) return J_BAD_ARG;
    uint32_t v=0;
    for(unsigned i=0;i<BOARD_IO;i++) if((mask>>i)&1u) {
        if(iomode[i]==3) return J_BAD_ARG; // use ADC_READ
        if(gpio_get_level(BOARD_IO_GPIOS[i])) v|=1u<<i;
    }
    *levels=v;
    return J_OK;
}
int io_adc(unsigned id,int *raw)
{
    if(!raw || !io_valid(id) || lease[id] || iomode[id]!=3 || !adc_handle) return J_BAD_ARG;
    int pin=BOARD_IO_GPIOS[id];
    if(pin<1 || pin>10 || !adc_configured[pin-1])return J_UNAVAILABLE;
    return adc_oneshot_read(adc_handle,(adc_channel_t)(pin-1),raw)==ESP_OK?J_OK:J_HARDWARE;
}
int io_pwm_stop(unsigned ch)
{
    if(ch>=4)return J_BAD_ARG;
    if(!ps[ch].live)return J_OK;
    ledc_stop(LEDC_LOW_SPEED_MODE,(ledc_channel_t)ch,0);
    release_io(ps[ch].io);
    ps[ch].live=false;
    return J_OK;
}
int io_pwm(unsigned ch,unsigned id,uint32_t hz,uint16_t duty)
{
    if(ch>=4 || !io_valid(id)||hz<10||hz>40000||duty>8191) return J_BAD_ARG;
    if(!free_io(id,7+ch))return J_BUSY;
    if(ps[ch].live)io_pwm_stop(ch);
    ledc_timer_config_t tc={.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_13_BIT,.timer_num=(ledc_timer_t)ch,
        .freq_hz=hz,.clk_cfg=LEDC_AUTO_CLK};
    if(ledc_timer_config(&tc)!=ESP_OK)return J_HARDWARE;
    ledc_channel_config_t cc={.gpio_num=BOARD_IO_GPIOS[id],
        .speed_mode=LEDC_LOW_SPEED_MODE,.channel=(ledc_channel_t)ch,
        .intr_type=LEDC_INTR_DISABLE,.timer_sel=(ledc_timer_t)ch,
        .duty=duty,.hpoint=0};
    if(ledc_channel_config(&cc)!=ESP_OK)return J_HARDWARE;
    lease[id]=7+ch;ps[ch].live=true;ps[ch].io=id;
    return J_OK;
}
int i2c_close(unsigned bus)
{
    if(bus>1)return J_BAD_ARG;
    i2c_ctx *s=&is[bus];if(!s->live)return J_OK;
    for(unsigned a=0;a<128;a++)if(s->dev[a]){
        i2c_master_bus_rm_device(s->dev[a]);s->dev[a]=NULL;
    }
    esp_err_t e=i2c_del_master_bus(s->h);
    release_io(s->sda);release_io(s->scl);
    memset(s,0,sizeof(*s));return e==ESP_OK?J_OK:J_HARDWARE;
}
int i2c_setup(unsigned bus,unsigned sda,unsigned scl,uint32_t hz)
{
    if(bus>1 || !io_valid(sda)|| !io_valid(scl)||sda==scl||hz<10000||hz>400000) return J_BAD_ARG;
    if(!free_io(sda,1+bus)||!free_io(scl,1+bus))return J_BUSY;
    i2c_close(bus);
    i2c_master_bus_config_t bc={.i2c_port=(i2c_port_num_t)bus,
        .sda_io_num=BOARD_IO_GPIOS[sda],.scl_io_num=BOARD_IO_GPIOS[scl],
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t h=NULL;
    if(i2c_new_master_bus(&bc,&h)!=ESP_OK)return J_HARDWARE;
    is[bus].live=true;is[bus].sda=sda;is[bus].scl=scl;is[bus].h=h;
    // Wire frequency chosen at device handle creation, saved per bus.
    lease[sda]=lease[scl]=1+bus;
    (void)hz;
    return J_OK;
}
// Per-bus device speed; set by i2c_setup(), separate from the bus resource.
static uint32_t i2c_hz[2]={50000,50000};
void i2c_set_hz(unsigned bus,uint32_t hz) {if(bus<2)i2c_hz[bus]=hz;}
int i2c_xfer(unsigned bus,unsigned addr,const uint8_t *tx,size_t nt,uint8_t *rx,size_t nr)
{
    if(bus>1||!is[bus].live||addr<0x08||addr>0x77||nt>MAX_XFER||nr>MAX_XFER||nt+nr>MAX_XFER)return J_BAD_ARG;
    i2c_ctx *s=&is[bus];
    esp_err_t e;
    if(nt==0 && nr==0) {
        e=i2c_master_probe(s->h,addr,150);
        return e==ESP_OK?J_OK:e==ESP_ERR_TIMEOUT?J_TIMEOUT:J_HARDWARE;
    }
    if(!s->dev[addr]) {
        i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
            .device_address=addr,.scl_speed_hz=i2c_hz[bus]};
        if(i2c_master_bus_add_device(s->h,&dc,&s->dev[addr])!=ESP_OK)return J_HARDWARE;
    }
    if(nt && nr) e=i2c_master_transmit_receive(s->dev[addr],tx,nt,rx,nr,300);
    else if(nt) e=i2c_master_transmit(s->dev[addr],tx,nt,300);
    else e=i2c_master_receive(s->dev[addr],rx,nr,300);
    return e==ESP_OK?J_OK:e==ESP_ERR_TIMEOUT?J_TIMEOUT:J_HARDWARE;
}
int spi_close(unsigned bus)
{
    if(bus>1)return J_BAD_ARG;
    spi_ctx *s=&ss[bus];if(!s->live)return J_OK;
    spi_bus_remove_device(s->dev);
    esp_err_t e=spi_bus_free(bus==0?SPI2_HOST:SPI3_HOST);
    release_io(s->mosi);if(s->miso!=255)release_io(s->miso);
    release_io(s->sck);release_io(s->cs);
    memset(s,0,sizeof(*s));return e==ESP_OK?J_OK:J_HARDWARE;
}
int spi_setup(unsigned bus,unsigned mosi,unsigned miso,unsigned sck,
              unsigned cs,unsigned mode,uint32_t hz)
{
    if(bus>1 || !io_valid(mosi) || (!io_valid(miso)&&miso!=255) ||
       !io_valid(sck)|| !io_valid(cs)||mode>3||hz<1000||hz>20000000)return J_BAD_ARG;
    if(!unique4(mosi,miso==255?-1:(int)miso,sck,cs))return J_BAD_ARG;
    unsigned owner=3+bus;
    if(!free_io(mosi,owner)||!free_io(sck,owner)||!free_io(cs,owner)||
       (miso!=255&&!free_io(miso,owner)))return J_BUSY;
    // Matrix routing is intended for relatively low-frequency prototyping.
    if((mosi<9||miso<9||sck<9||cs<9) && hz>2000000)return J_BAD_ARG;
    spi_close(bus);
    spi_host_device_t host=bus==0?SPI2_HOST:SPI3_HOST;
    spi_bus_config_t bc={.mosi_io_num=BOARD_IO_GPIOS[mosi],
        .miso_io_num=miso==255?-1:BOARD_IO_GPIOS[miso],
        .sclk_io_num=BOARD_IO_GPIOS[sck],.quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=MAX_XFER};
    if(spi_bus_initialize(host,&bc,SPI_DMA_CH_AUTO)!=ESP_OK)return J_HARDWARE;
    spi_device_interface_config_t dc={.clock_speed_hz=(int)hz,.mode=mode,
        .spics_io_num=BOARD_IO_GPIOS[cs],.queue_size=2};
    spi_device_handle_t dev=NULL;
    if(spi_bus_add_device(host,&dc,&dev)!=ESP_OK){spi_bus_free(host);return J_HARDWARE;}
    ss[bus]=(spi_ctx){.live=true,.mosi=mosi,.miso=miso,.sck=sck,.cs=cs,.dev=dev};
    lease[mosi]=lease[sck]=lease[cs]=owner;if(miso!=255)lease[miso]=owner;
    return J_OK;
}
int spi_xfer(unsigned bus,const uint8_t *tx,uint8_t *rx,size_t len)
{
    if(bus>1 || !ss[bus].live||len>MAX_XFER||!len)return J_BAD_ARG;
    uint8_t *dma_tx=heap_caps_malloc(len,MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    uint8_t *dma_rx=heap_caps_malloc(len,MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    if(!dma_tx||!dma_rx){free(dma_tx);free(dma_rx);return J_INTERNAL;}
    memcpy(dma_tx,tx,len);
    spi_transaction_t t={.length=len*8,.tx_buffer=dma_tx,.rx_buffer=dma_rx};
    esp_err_t e=spi_device_polling_transmit(ss[bus].dev,&t);
    if(e==ESP_OK)memcpy(rx,dma_rx,len);
    free(dma_tx);free(dma_rx);
    return e==ESP_OK?J_OK:J_HARDWARE;
}
int uart_close(unsigned bus)
{
    if(bus>1)return J_BAD_ARG;
    uart_ctx *s=&us[bus];if(!s->live)return J_OK;
    esp_err_t e=uart_driver_delete((uart_port_t)(UART_NUM_1+bus));
    release_io(s->tx);release_io(s->rx);
    memset(s,0,sizeof(*s));return e==ESP_OK?J_OK:J_HARDWARE;
}
int uart_setup(unsigned bus,unsigned tx,unsigned rx,uint32_t baud,unsigned parity,unsigned stopbits)
{
    if(bus>1 || !io_valid(tx)||!io_valid(rx)||tx==rx||baud<300||
       baud>921600||parity>2||(stopbits!=1&&stopbits!=2))return J_BAD_ARG;
    if(!free_io(tx,5+bus)||!free_io(rx,5+bus))return J_BUSY;
    if((tx<9||rx<9)&&baud>460800)return J_BAD_ARG;
    uart_close(bus);
    uart_port_t p=(uart_port_t)(UART_NUM_1+bus);
    uart_config_t uc={.baud_rate=(int)baud,.data_bits=UART_DATA_8_BITS,
        .parity=parity==0?UART_PARITY_DISABLE:parity==1?UART_PARITY_EVEN:UART_PARITY_ODD,
        .stop_bits=stopbits==1?UART_STOP_BITS_1:UART_STOP_BITS_2,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    if(uart_driver_install(p,4096,2048,0,NULL,0)!=ESP_OK)return J_HARDWARE;
    if(uart_param_config(p,&uc)!=ESP_OK ||
       uart_set_pin(p,BOARD_IO_GPIOS[tx],BOARD_IO_GPIOS[rx],
             UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK) {
        uart_driver_delete(p);
        return J_HARDWARE;
    }
    us[bus]=(uart_ctx){.live=true,.tx=tx,.rx=rx};
    lease[tx]=lease[rx]=5+bus;
    return J_OK;
}
int uart_write_api(unsigned bus,const uint8_t *data,size_t len,int *out)
{
    if(bus>1||!us[bus].live||len>MAX_XFER||!out)return J_BAD_ARG;
    int n=uart_write_bytes((uart_port_t)(UART_NUM_1+bus),data,len);
    if(n<0)return J_HARDWARE;
    *out=n;return J_OK;
}
int uart_read_api(unsigned bus,uint8_t *data,size_t max,unsigned timeout_ms,int *out)
{
    if(bus>1||!us[bus].live||max>MAX_XFER||timeout_ms>500||!out)return J_BAD_ARG;
    int n=uart_read_bytes((uart_port_t)(UART_NUM_1+bus),data,max,pdMS_TO_TICKS(timeout_ms));
    if(n<0)return J_HARDWARE;
    *out=n;return J_OK;
}
void peripherals_safe(void)
{
    for(unsigned i=0;i<2;i++) {i2c_close(i);spi_close(i);uart_close(i);}
    for(unsigned i=0;i<4;i++)io_pwm_stop(i);
    for(unsigned i=0;i<BOARD_IO;i++)release_io(i);
}
