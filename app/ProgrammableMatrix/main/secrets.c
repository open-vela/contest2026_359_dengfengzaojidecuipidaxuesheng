#include "secrets.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

static uint8_t g_key[32];
static bool g_paired=false;
static const char *TAG="device-secrets";

bool secrets_is_paired(void) {return g_paired;}
const uint8_t *secrets_key(void) {return g_paired ? g_key : NULL;}

void secrets_init(void)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("jbox-sec",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND){g_paired=false;return;}
    ESP_ERROR_CHECK(err);
    size_t n=sizeof g_key;
    err=nvs_get_blob(h,"api-hmac",g_key,&n);
    nvs_close(h);
    g_paired=(err==ESP_OK && n==sizeof g_key);
    if(!g_paired) memset(g_key,0,sizeof g_key);
    ESP_LOGI(TAG,"controller binding: %s",g_paired?"present":"not paired");
}

esp_err_t secrets_claim_key(uint8_t result[32])
{
    if(!result)return ESP_ERR_INVALID_ARG;
    if(!g_paired){
        uint8_t next[32];
        esp_fill_random(next,sizeof next);
        nvs_handle_t h;
        esp_err_t e=nvs_open("jbox-sec",NVS_READWRITE,&h);
        if(e!=ESP_OK){memset(next,0,sizeof next);return e;}
        e=nvs_set_blob(h,"api-hmac",next,sizeof next);
        if(e==ESP_OK)e=nvs_commit(h);
        nvs_close(h);
        if(e!=ESP_OK){memset(next,0,sizeof next);return e;}
        memcpy(g_key,next,sizeof g_key);
        memset(next,0,sizeof next);
        g_paired=true;
    }
    memcpy(result,g_key,sizeof g_key);
    return ESP_OK;
}
