#include "wifi_ble.h"
#include "secrets.h"
#include "net.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include <stdio.h>

#define GOT_IP_BIT BIT0
#define BLE_PAIR_WINDOW_SECONDS 180
#define LAN_BIND_WINDOW_SECONDS 120

static const char *TAG="BLE+WIFI";
static EventGroupHandle_t wifi_bits;
static volatile bool provisioning_active;
static volatile bool credentials_saved;
static bool did_ble_provision;
static esp_timer_handle_t reconnect_timer;
static esp_timer_handle_t ble_window_timer;
static unsigned reconnect_attempts;

bool wifi_is_ready(void){return wifi_bits && (xEventGroupGetBits(wifi_bits)&GOT_IP_BIT);}
bool wifi_has_credentials(void){return credentials_saved;}

static void ble_window_expired(void *arg)
{
    (void)arg;
    // Controller memory may be permanently released on BLE teardown in the
    // current ESP-IDF boot. Reboot into the quiet unprovisioned state.
    if(provisioning_active){
        ESP_LOGW(TAG,"BLE provisioning timed out; restarting into standby");
        esp_restart();
    }
}
static void reconnect_callback(void *arg)
{
    (void)arg;
    if(!provisioning_active && credentials_saved && !wifi_is_ready())
        esp_wifi_connect();
}
static void stop_ble_deadline(void)
{
    if(ble_window_timer && esp_timer_is_active(ble_window_timer))
        esp_timer_stop(ble_window_timer);
}
static void on_event(void *arg,esp_event_base_t event,int32_t id,void *data)
{
    (void)arg;
    if(event==WIFI_PROV_EVENT){
        switch(id){
        case WIFI_PROV_START:ESP_LOGI(TAG,"button-authorized BLE pairing started (no code)");break;
        case WIFI_PROV_CRED_RECV:ESP_LOGI(TAG,"Wi-Fi credentials received via BLE");break;
        case WIFI_PROV_CRED_SUCCESS:
            credentials_saved=true;
            stop_ble_deadline();
            ESP_LOGI(TAG,"Wi-Fi configured successfully");
            break;
        case WIFI_PROV_CRED_FAIL:
            ESP_LOGW(TAG,"Wi-Fi connection failed; retry via BLE or hold pairing button to reset Wi-Fi");
            (void)wifi_prov_mgr_reset_sm_state_on_failure();
            break;
        case WIFI_PROV_END:
            provisioning_active=false;
            stop_ble_deadline();
            wifi_prov_mgr_deinit();
            ESP_LOGI(TAG,"BLE stopped and memory released");
            break;
        default:break;
        }
    }else if(event==WIFI_EVENT){
        if(id==WIFI_EVENT_STA_START){
            esp_wifi_set_ps(WIFI_PS_NONE);
            if(!provisioning_active)esp_wifi_connect();
        }else if(id==WIFI_EVENT_STA_DISCONNECTED){
            xEventGroupClearBits(wifi_bits,GOT_IP_BIT);
            if(!provisioning_active && credentials_saved){
                unsigned delay_s=1u<<(reconnect_attempts<5?reconnect_attempts:5);
                if(delay_s>30)delay_s=30;
                if(reconnect_attempts<10)reconnect_attempts++;
                if(esp_timer_is_active(reconnect_timer))esp_timer_stop(reconnect_timer);
                esp_timer_start_once(reconnect_timer,(uint64_t)delay_s*1000000ULL);
            }
        }
    }else if(event==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *e=data;
        ESP_LOGI(TAG,"Wi-Fi connected: " IPSTR,IP2STR(&e->ip_info.ip));
        reconnect_attempts=0;
        if(esp_timer_is_active(reconnect_timer))esp_timer_stop(reconnect_timer);
        xEventGroupSetBits(wifi_bits,GOT_IP_BIT);
        // One physical click performs both initial steps: BLE Wi-Fi
        // provisioning and LAN client binding; never requires UART.
        if(did_ble_provision && !secrets_is_paired()){
            did_ble_provision=false;
            net_pair_open(LAN_BIND_WINDOW_SECONDS);
            ESP_LOGI(TAG,"LAN API binding open for %d s",LAN_BIND_WINDOW_SECONDS);
        }
    }
}

void wifi_ble_start(void)
{
    wifi_bits=xEventGroupCreate();
    const esp_timer_create_args_t rargs={.callback=reconnect_callback,.name="wifi-retry"};
    const esp_timer_create_args_t bargs={.callback=ble_window_expired,.name="ble-pair-expiry"};
    ESP_ERROR_CHECK(esp_timer_create(&rargs,&reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&bargs,&ble_window_timer));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT,ESP_EVENT_ANY_ID,&on_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&on_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&on_event,NULL));
    wifi_prov_mgr_config_t prov={
        .scheme=wifi_prov_scheme_ble,
        .scheme_event_handler=WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov));
    bool provisioned=false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    credentials_saved=provisioned;
    if(provisioned){
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }else{
        ESP_LOGI(TAG,"waiting for active-LOW GPIO10 pairing button; no serial or PoP needed");
    }
}

void wifi_ble_button_short_press(void)
{
    if(credentials_saved){
        net_pair_open(LAN_BIND_WINDOW_SECONDS);
        ESP_LOGI(TAG,"physical LAN binding window open for %d seconds",LAN_BIND_WINDOW_SECONDS);
        return;
    }
    if(provisioning_active)return;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    char name[32];
    snprintf(name,sizeof name,"JBOX-%02X%02X%02X",mac[3],mac[4],mac[5]);
    provisioning_active=true;
    did_ble_provision=true;
    // Security 1 negotiates encrypted transport without a PoP. The physical
    // button/time window is the authorization gate; there is no MITM proof.
    esp_err_t e=wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1,NULL,name,NULL);
    if(e==ESP_OK){
        stop_ble_deadline();
        ESP_ERROR_CHECK(esp_timer_start_once(ble_window_timer,
                        (uint64_t)BLE_PAIR_WINDOW_SECONDS*1000000ULL));
        ESP_LOGI(TAG,"BLE visible as %s for up to %d seconds",name,BLE_PAIR_WINDOW_SECONDS);
    }else{
        provisioning_active=false;
        ESP_LOGE(TAG,"could not start BLE provisioning: %s",esp_err_to_name(e));
    }
}

void wifi_ble_forget_wifi(void)
{
    net_pair_close();
    ESP_LOGW(TAG,"physical Wi-Fi reset: retain API binding key and restart");
    // This affects Wi-Fi credential storage, NOT jbox-sec API binding key.
    (void)esp_wifi_restore();
    esp_restart();
}
