#include "matrix.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>
static const char *TAG="CH446Q";
static routes_t active;

// Datasheet: serial input 7 address bits AY2..AY0 AX3..AX0 (MSB first),
// then DAT on/off value, STB pulse latches the value. CK rising edge shifts.
// This uses GPIO as a deterministic control interface; do not write these
// seven GPIOs in any other task. Hardware clock/data tuning after bench test.
static inline void shift_set(unsigned chip,unsigned x,unsigned y,bool on)
{
    uint8_t addr=(uint8_t)((y<<4)|x);
    gpio_set_level(MATRIX_STB[chip],0);
    for (int k=6;k>=0;k--) {
        gpio_set_level(MATRIX_CLK,0);
        gpio_set_level(MATRIX_DAT, (addr>>k)&1);
        esp_rom_delay_us(1);
        gpio_set_level(MATRIX_CLK,1);
        esp_rom_delay_us(1);
    }
    gpio_set_level(MATRIX_CLK,0);
    gpio_set_level(MATRIX_DAT,on?1:0);
    esp_rom_delay_us(1);
    gpio_set_level(MATRIX_STB[chip],1);
    esp_rom_delay_us(1);
    gpio_set_level(MATRIX_STB[chip],0);
}
static void drive_reset(void)
{
    for(unsigned i=0;i<BOARD_CHIPS;i++) gpio_set_level(MATRIX_STB[i],0);
    gpio_set_level(MATRIX_CLK,0);
    gpio_set_level(MATRIX_DAT,0);
    gpio_set_level(MATRIX_RST,1); // reset HIGH clears all 128 latch bits
    esp_rom_delay_us(5);
    gpio_set_level(MATRIX_RST,0);
    esp_rom_delay_us(2);
}
void matrix_init(void)
{
    // Ensure STB is low before enabling matrix control, then pulse RST.
    memset(&active,0,sizeof active);
    for(unsigned c=0;c<BOARD_CHIPS;c++) {
        gpio_set_level(MATRIX_STB[c],0);
        gpio_reset_pin(MATRIX_STB[c]);
        gpio_set_direction(MATRIX_STB[c],GPIO_MODE_OUTPUT);
        gpio_set_level(MATRIX_STB[c],0);
    }
    for(int i=0;i<3;i++) {
        int pin=(int[]){MATRIX_DAT,MATRIX_CLK,MATRIX_RST}[i];
        gpio_reset_pin(pin);
        gpio_set_level(pin,pin==MATRIX_RST?1:0);
        gpio_set_direction(pin,GPIO_MODE_OUTPUT);
    }
    drive_reset();
    ESP_LOGI(TAG,"four CH446Q all disconnected, 8 shared buses");
}
int matrix_reset(void)
{
    drive_reset();
    memset(&active,0,sizeof active);
    return J_OK;
}
const routes_t *matrix_get(void) {return &active;}

int matrix_replace(const routes_t *new_routes)
{
    int rc=routes_validate(new_routes);
    if(rc!=J_OK) return rc;
    // Phase 1: disconnect all changed X→Y edges across ALL chips,
    // so no transient joins of formerly separate nets are possible.
    for(unsigned n=0;n<BOARD_NODES;n++) {
        int a=routes_net_of(&active,n), b=routes_net_of(new_routes,n);
        if(a>=0 && a!=b) shift_set(mapped_chip(n),mapped_x(n),(unsigned)a,false);
    }
    // Phase 2: make requested connections.
    for(unsigned n=0;n<BOARD_NODES;n++) {
        int a=routes_net_of(&active,n), b=routes_net_of(new_routes,n);
        if(b>=0 && a!=b) shift_set(mapped_chip(n),mapped_x(n),(unsigned)b,true);
    }
    active=*new_routes;
    return J_OK;
}
