#include "net.h"
#include "api.h"
#include "secrets.h"
#include "wifi_ble.h"
#include "route_plan.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include "mbedtls/md.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define FRAME_LEN 16
#define TAG_LEN 16
#define VER 1
#define FLAG_RESP 1
#define FLAG_HMAC 2
static const char *TAG="JBOX-API";
static const uint8_t magic[4]={'J','M','X','1'};
static uint32_t g_commands=0;
static portMUX_TYPE g_pair_lock=portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_pair_deadline_s=0;
static uint32_t g_pair_epoch=0;

void net_pair_open(unsigned seconds)
{
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000000LL);
    portENTER_CRITICAL(&g_pair_lock);
    g_pair_deadline_s=now+seconds;
    ++g_pair_epoch; // Invalidate any previously authenticated control session.
    portEXIT_CRITICAL(&g_pair_lock);
}
void net_pair_close(void)
{
    portENTER_CRITICAL(&g_pair_lock);
    g_pair_deadline_s=0;
    portEXIT_CRITICAL(&g_pair_lock);
}
bool net_pair_is_open(void)
{
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000000LL);
    portENTER_CRITICAL(&g_pair_lock);
    bool open=g_pair_deadline_s!=0 && (int32_t)(g_pair_deadline_s-now)>0;
    portEXIT_CRITICAL(&g_pair_lock);
    return open;
}
static uint32_t net_pair_epoch(void)
{
    portENTER_CRITICAL(&g_pair_lock);
    uint32_t v=g_pair_epoch;
    portEXIT_CRITICAL(&g_pair_lock);
    return v;
}
static bool net_pair_claim(void)
{
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000000LL);
    portENTER_CRITICAL(&g_pair_lock);
    bool granted=g_pair_deadline_s!=0 && (int32_t)(g_pair_deadline_s-now)>0;
    if(granted)g_pair_deadline_s=0; // first claimant wins, no second leak
    portEXIT_CRITICAL(&g_pair_lock);
    return granted;
}

static uint32_t le32(const uint8_t *p){return (uint32_t)p[0] |((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t *p,uint32_t x){for(int i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static void put16(uint8_t *p,uint16_t x){p[0]=x;p[1]=x>>8;}
static int secure_eq(const uint8_t *a,const uint8_t *b,size_t n){
    unsigned v=0;for(size_t i=0;i<n;i++)v|=a[i]^b[i];return v==0;
}
static int hmac2(const uint8_t *key,size_t nk,const uint8_t *a,size_t na,
                 const uint8_t *b,size_t nb,uint8_t out[32])
{
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;mbedtls_md_init(&ctx);
    if(mbedtls_md_setup(&ctx,md,1)!=0){mbedtls_md_free(&ctx);return -1;}
    int ok=mbedtls_md_hmac_starts(&ctx,key,nk);
    if(!ok)ok=mbedtls_md_hmac_update(&ctx,a,na);
    if(!ok && nb)ok=mbedtls_md_hmac_update(&ctx,b,nb);
    if(!ok)ok=mbedtls_md_hmac_finish(&ctx,out);
    mbedtls_md_free(&ctx);
    return ok;
}
static int recv_full(int fd,uint8_t *buf,size_t n,int32_t timeout_ms,uint32_t epoch)
{
    size_t done=0;
    int64_t started=esp_timer_get_time();
    while(done<n){
        if(net_pair_epoch()!=epoch)return -1;
        int ret=recv(fd,buf+done,n-done,0);
        if(ret>0){done+=ret;continue;}
        if(ret==0)return -1;
        if(errno==EINTR)continue;
        if(errno==EAGAIN || errno==EWOULDBLOCK) {
            if(wifi_is_ready() && (timeout_ms==0 ||
                esp_timer_get_time()-started<timeout_ms*1000LL))continue;
        }
        return -1;
    }
    return 0;
}
static int send_full(int fd,const uint8_t *buf,size_t n)
{
    size_t done=0;
    while(done<n){
        int rc=send(fd,buf+done,n-done,0);
        if(rc>0){done+=rc;continue;}
        if(rc<0 && errno==EINTR)continue;
        return -1;
    }
    return 0;
}
static int send_resp(int fd,uint8_t cmd,uint32_t seq,int32_t status,
                     const uint8_t *out,size_t outn,bool secured,
                     const uint8_t key[32],uint8_t *frame)
{
    if(outn+4>API_MAX_PAYLOAD)return -1;
    memcpy(frame,magic,4);
    frame[4]=VER;frame[5]=cmd;
    put16(frame+6,FLAG_RESP|(secured?FLAG_HMAC:0));
    put32(frame+8,seq);put32(frame+12,(uint32_t)(4+outn));
    put32(frame+FRAME_LEN,(uint32_t)status);
    if(outn)memcpy(frame+FRAME_LEN+4,out,outn);
    size_t n=FRAME_LEN+4+outn;
    if(secured) {
        uint8_t tag[32];
        if(hmac2(key,32,frame,n,NULL,0,tag)!=0)return -1;
        memcpy(frame+n,tag,TAG_LEN);n+=TAG_LEN;
    }
    // Single contiguous write avoids 3-4 tiny TCP packets per response.
    return send_full(fd,frame,n);
}
static int check_mac(const uint8_t key[32],const uint8_t h[16],
                     const uint8_t *p,size_t n,const uint8_t mac[16])
{
    uint8_t sum[32];
    if(hmac2(key,32,h,16,p,n,sum)!=0)return 0;
    return secure_eq(sum,mac,TAG_LEN);
}
static void client_session(int fd)
{
    uint8_t *payload=malloc(API_MAX_PAYLOAD);
    uint8_t *output=malloc(API_MAX_PAYLOAD);
    uint8_t *reply=malloc(API_MAX_PAYLOAD+FRAME_LEN+TAG_LEN);
    if(!payload || !output || !reply){free(payload);free(output);free(reply);return;}
    uint8_t nonce[16]={0},session[32]={0};
    bool have_hello=false,authenticated=false;
    uint32_t last_seq=0;
    uint32_t pair_epoch=net_pair_epoch();
    struct timeval rcvto={.tv_sec=2,.tv_usec=0};
    struct timeval sndto={.tv_sec=3,.tv_usec=0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&rcvto,sizeof rcvto);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&sndto,sizeof sndto);
    while(wifi_is_ready() && net_pair_epoch()==pair_epoch) {
        uint8_t h[FRAME_LEN],mac[TAG_LEN];
        if(recv_full(fd,h,sizeof h,authenticated?0:10000,pair_epoch)!=0)break;
        if(memcmp(h,magic,4)!=0 || h[4]!=VER)break;
        uint8_t cmd=h[5];uint16_t flags=(uint16_t)h[6]|(uint16_t)h[7]<<8;
        uint32_t seq=le32(h+8),len=le32(h+12);
        if(flags & ~(uint16_t)FLAG_HMAC)break;
        if(len>API_MAX_PAYLOAD)break;
        if(recv_full(fd,payload,len,authenticated?0:10000,pair_epoch)!=0)break;
        if(flags&FLAG_HMAC) if(recv_full(fd,mac,TAG_LEN,authenticated?0:10000,pair_epoch)!=0)break;
        if(!authenticated){
            if(flags!=0)break;
            if(cmd==CMD_PAIR && len==0){
                if(!net_pair_claim()){
                    (void)send_resp(fd,cmd,seq,J_AUTH,NULL,0,false,NULL,reply);
                    break;
                }
                // The device never prints a secret. During a physically
                // authorized window, give it only to the first LAN claimant.
                // For adversarial LANs, use an encrypted/pinned transport.
                uint8_t key_material[32];
                esp_err_t err=secrets_claim_key(key_material);
                if(err==ESP_OK){
                    (void)send_resp(fd,cmd,seq,J_OK,key_material,32,false,NULL,reply);
                    ESP_LOGI(TAG,"first LAN controller claimed physical pairing window");
                }else{
                    (void)send_resp(fd,cmd,seq,J_INTERNAL,NULL,0,false,NULL,reply);
                }
                memset(key_material,0,sizeof key_material);
                break; // reconnect for HELLO/AUTH
            }
            if(cmd==CMD_HELLO && len==0) {
                // Physical pair mode exclusively accepts PAIR, preventing a
                // known controller from occupying the single TCP slot.
                if(net_pair_is_open()){
                    (void)send_resp(fd,cmd,seq,J_AUTH,NULL,0,false,NULL,reply);
                    break;
                }
                esp_fill_random(nonce,sizeof nonce);
                have_hello=true;
                if(send_resp(fd,cmd,seq,J_OK,nonce,sizeof nonce,false,NULL,reply)!=0)break;
                continue;
            }
            if(cmd!=CMD_AUTH || len!=32 || !have_hello || !secrets_is_paired())break;
            static const uint8_t auth_suf[]="JMX-AUTH-v1";
            static const uint8_t sess_suf[]="JMX-SESSION-v1";
            uint8_t expected[32];
            if(hmac2(secrets_key(),32,nonce,sizeof nonce,auth_suf,sizeof(auth_suf)-1,expected)!=0)break;
            if(!secure_eq(expected,payload,32)) {
                send_resp(fd,cmd,seq,J_AUTH,NULL,0,false,NULL,reply);
                ESP_LOGW(TAG,"invalid HMAC authentication");break;
            }
            if(hmac2(secrets_key(),32,nonce,sizeof nonce,sess_suf,sizeof(sess_suf)-1,session)!=0)break;
            authenticated=true;last_seq=seq;
            if(send_resp(fd,cmd,seq,J_OK,NULL,0,true,session,reply)!=0)break;
            ESP_LOGI(TAG,"LAN controller authenticated");
            continue;
        }
        if(flags!=FLAG_HMAC || seq<=last_seq)break;
        if(!check_mac(session,h,payload,len,mac))break;
        last_seq=seq;
        size_t outlen=0;
        int rc=api_dispatch(cmd,payload,len,output,&outlen);
        if(send_resp(fd,cmd,seq,rc,output,outlen,true,session,reply)!=0)break;
        ++g_commands;
        if(cmd==CMD_FACTORY_RESET && rc==J_OK) {
            ESP_LOGW(TAG,"authenticated factory reset");
            api_safe();
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
    memset(session,0,sizeof session);
    free(payload);free(output);free(reply);
    // Safe on control session exit: drop routes, high-Z all IO, release buses.
    api_safe();
    ESP_LOGI(TAG,"session closed; safe hardware reset (commands=%lu)",(unsigned long)g_commands);
}
static void listener(void *arg)
{
    (void)arg;
    while(true){
        while(!wifi_is_ready())vTaskDelay(pdMS_TO_TICKS(250));
        int server=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);
        if(server<0){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
        int enable=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&enable,sizeof enable);
        struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(API_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
        if(bind(server,(struct sockaddr*)&addr,sizeof addr)<0 || listen(server,1)<0){
            ESP_LOGE(TAG,"listen failed: %d",errno);close(server);
            vTaskDelay(pdMS_TO_TICKS(2000));continue;
        }
        ESP_LOGI(TAG,"binary API listening on TCP %d",API_PORT);
        while(wifi_is_ready()) {
            fd_set r;FD_ZERO(&r);FD_SET(server,&r);
            struct timeval tv={.tv_sec=1,.tv_usec=0};
            int rc=select(server+1,&r,NULL,NULL,&tv);
            if(rc<=0)continue;
            struct sockaddr_in peer;socklen_t addrlen=sizeof peer;
            int fd=accept(server,(struct sockaddr*)&peer,&addrlen);
            if(fd<0)continue;
            int yes=1;
            setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
            setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&yes,sizeof yes);
            int idle=30,interval=5,count=3;
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof idle);
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&interval,sizeof interval);
            setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&count,sizeof count);
            client_session(fd);
            shutdown(fd,SHUT_RDWR);close(fd);
        }
        close(server);
    }
}
// Read-only LAN broadcast discovery. No secrets or GPIO operations exposed.
#define DISCOVERY_PORT 4748
static void discover_task(void *arg)
{
    (void)arg;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    char identity[32];
    snprintf(identity,sizeof identity,"%02X%02X%02X%02X%02X%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    while(true){
        int sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(sock<0){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
        struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(DISCOVERY_PORT),
                                 .sin_addr.s_addr=htonl(INADDR_ANY)};
        int reuse=1;
        setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
        if(bind(sock,(struct sockaddr*)&addr,sizeof addr)<0){
            close(sock);vTaskDelay(pdMS_TO_TICKS(1000));continue;
        }
        struct timeval tv={.tv_sec=1,.tv_usec=0};
        setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
        while(true){
            char data[64];
            struct sockaddr_in peer;socklen_t n=sizeof peer;
            int got=recvfrom(sock,data,sizeof data,0,(struct sockaddr*)&peer,&n);
            if(got!=13 || memcmp(data,"JBOX-DISCOVER",13)!=0 || !wifi_is_ready())continue;
            char replybuf[52];
            int written=snprintf(replybuf,sizeof replybuf,"JBOX-HERE|%s|%d|%d",
                         identity,secrets_is_paired()?1:0,net_pair_is_open()?1:0);
            if(written>0 && written<(int)sizeof replybuf)
                (void)sendto(sock,replybuf,written,0,(struct sockaddr*)&peer,n);
        }
    }
}
void net_start(void)
{
    // Stack includes TCP + mbedTLS HMAC contexts; RX/TX buffers are heap based.
    xTaskCreatePinnedToCore(listener,"lan-control",12288,NULL,6,NULL,1);
    xTaskCreatePinnedToCore(discover_task,"lan-discover",4096,NULL,4,NULL,1);
}
