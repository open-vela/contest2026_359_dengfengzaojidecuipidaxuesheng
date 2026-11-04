#include "button.h"
#include "board.h"
#include "wifi_ble.h"
#include "api.h"
#include "driver/gpio.h"
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

static void button_task(void *unused)
{
    (void)unused;
    bool stable=false;
    bool raw_prev=false;
    unsigned same_ticks=0;
    unsigned held_ticks=0;
    while(1){
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
    ESP_LOGI(TAG,"GPIO10 active-low: tap to pair, hold 6s Wi-Fi reset, 12s factory reset");
}
