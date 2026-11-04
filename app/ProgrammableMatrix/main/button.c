#include "button.h"
#include "board.h"
#include "wifi_ble.h"
#include "api.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include <string.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// GPIO10 (input): physical switch to GND, 10k external pull-up to 3.3V,
// and 100nF capacitor to GND. Low level means pressed.
// Tap: BLE Wi-Fi provisioning when unconfigured, LAN API binding otherwise.
// Hold 6-12 sec: clear Wi-Fi only; hold >=12 sec: factory reset credentials
// AND control key. Never prints or requests a pairing code on UART.
#define POLL_MS 20
#define DEBOUNCE_TICKS 3
#define WIFI_RESET_TICKS 300
#define FULL_RESET_TICKS 600
static const char *TAG="pair-button";

// Wired UART0 is a local authorization surface, never a network command.
// Parse bounded complete lines; discard oversized/binary lines until newline.
static void serial_pair_poll(void)
{
    static char line[32];
    static unsigned used;
    static bool discard;
    unsigned char bytes[32];
    int count=uart_read_bytes(UART_NUM_0,bytes,sizeof bytes,0);
    for(int i=0;i<count;i++){
        unsigned char ch=bytes[i];
        if(ch=='\r' || ch=='\n'){
            line[used]=0;
            if(!discard && used){
                if(strcmp(line,"jbox pair")==0){
                    ESP_LOGI(TAG,"local serial pairing request");
                    wifi_ble_button_short_press();
                }else if(strcmp(line,"jbox provision")==0){
                    ESP_LOGI(TAG,"local serial Wi-Fi provisioning request");
                    wifi_ble_serial_provision();
                }else if(strcmp(line,"help")==0){
                    ESP_LOGI(TAG,"UART commands: jbox pair, jbox provision, help");
                }else ESP_LOGW(TAG,"unknown UART command");
            }
            used=0;discard=false;
        }else if(ch<32 || ch>126 || used>=sizeof(line)-1){
            discard=true;
        }else if(!discard)line[used++]=(char)ch;
    }
}

static void button_task(void *unused)
{
    (void)unused;
    bool stable=false;
    bool raw_prev=false;
    unsigned same_ticks=0;
    unsigned held_ticks=0;
    while(1){
        serial_pair_poll();
        bool raw=gpio_get_level(BOARD_PAIR_BUTTON)==0;
        if(raw==raw_prev){if(same_ticks<DEBOUNCE_TICKS)same_ticks++;}
        else{raw_prev=raw;same_ticks=0;}
        if(same_ticks==DEBOUNCE_TICKS && raw!=stable){
            stable=raw;
            if(stable) held_ticks=0;
            else{
                if(held_ticks>=FULL_RESET_TICKS){
                    ESP_LOGW(TAG,"physical factory reset");
                    api_safe();
                    ESP_ERROR_CHECK(nvs_flash_erase());
                    esp_restart();
                }else if(held_ticks>=WIFI_RESET_TICKS){
                    api_safe();
                    wifi_ble_forget_wifi();
                }else if(held_ticks>=DEBOUNCE_TICKS){
                    wifi_ble_button_short_press();
                }
                held_ticks=0;
            }
        }
        if(stable && held_ticks<0xffffu)held_ticks++;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
void button_start(void)
{
    // Console TX remains polling; only UART0 RX gets a bounded driver buffer.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,256,0,0,NULL,0));
    gpio_reset_pin(BOARD_PAIR_BUTTON);
    gpio_config_t config={
        .pin_bit_mask=(1ULL<<BOARD_PAIR_BUTTON),
        .mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    xTaskCreatePinnedToCore(button_task,"pair-button",3072,NULL,5,NULL,0);
    ESP_LOGI(TAG,"GPIO10 active-low: tap to pair, hold 6s Wi-Fi reset, 12s factory reset; UART: jbox pair");
}
