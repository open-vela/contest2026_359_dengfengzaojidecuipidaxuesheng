/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_ble_channel.h"
#include "mynewt-nimble/porting/npl/nuttx/include/nimble/nuttx_callout.h"
#include "ble_register.h"

static int usage(void)
{
  fprintf(stderr, "Usage:\n"
    "  c6ble register\n"
    "  c6ble status\n"
    "  c6ble identity\n"
    "  c6ble central scan [service-uuid128]\n"
    "  c6ble central devices|sessions\n"
    "  c6ble central select <connection-generation>\n"
    "  c6ble central stop <scan-generation>\n"
    "  c6ble central connect <index> <scan-generation>\n"
    "  c6ble central disconnect <connection-generation>\n"
    "  c6ble central services|pair|mtu|notification <connection-generation>\n"
    "  c6ble central characteristics <generation> <start> <end>\n"
    "  c6ble central read <generation> <handle>\n"
    "  c6ble timers\n"
    "  c6ble channel status\n"
    "  c6ble channel receive <hid-epoch>\n"
    "  c6ble channel send <hid-epoch> <hex-bytes>\n"
    "  c6ble mouse start|status|release|stop|diagnostics\n"
    "  c6ble mouse input begin <epoch> | end|keepalive <session>\n"
    "  c6ble mouse input report <session> <buttons> <x> <y> <wheel>\n"
    "  c6ble mouse move <x> <y> [wheel] [buttons]\n"
    "  c6ble mouse click left|right|middle\n"
    "  c6ble mouse scroll <steps>\n"
    "Arguments are decimal; x/y/wheel: -127..127, buttons: 0..7.\n"
    "Start opens a 120-second pairing window; it sends no input.\n");
  return 2;
}

static int number(const char *text, int minimum, int maximum, int *value)
{
  char *end;
  long parsed;
  if (!text || !*text) return -EINVAL;
  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno || end == text || *end || parsed < minimum || parsed > maximum)
    return -EINVAL;
  *value = (int)parsed;
  return 0;
}

static int result(const char *command, int ret)
{
  if (ret)
    {
      fprintf(stderr, "c6ble mouse %s: %d (%s)\n", command, ret,
              ret < 0 ? strerror(-ret) : "backend error");
      return 1;
    }
  printf("c6ble mouse %s: ok\n", command);
  return 0;
}

static int service_uuid(const char *text,uint8_t uuid[16])
{
  static const unsigned positions[16]={0,2,4,6,9,11,14,16,19,21,24,26,28,30,32,34};
  if(!text||strlen(text)!=36||text[8]!='-'||text[13]!='-'||text[18]!='-'||text[23]!='-')return -EINVAL;
  for(unsigned i=0;i<16;i++){
    unsigned value=0;
    for(unsigned j=0;j<2;j++){
      unsigned c=(unsigned char)text[positions[i]+j];
      if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;
      else if(c>='A'&&c<='F')c=c-'A'+10;else return -EINVAL;
      value=value*16+c;
    }
    uuid[15-i]=value;
  }
  return 0;
}

static int central_command(int argc,char **argv)
{
  const char *op=argv[2];int ret=-EINVAL,a=0,b=0,c=0;
  if(!strcmp(op,"scan")&&(argc==3||argc==4)){
    uint8_t uuid[16];
    ret=argc==3?glass_ble_central_scan():service_uuid(argv[3],uuid)?-EINVAL:glass_ble_central_scan_service(uuid);
  }else if(!strcmp(op,"stop")&&argc==4&&!number(argv[3],1,INT_MAX,&a))
    ret=glass_ble_central_scan_stop(a);
  else if(!strcmp(op,"connect")&&argc==5&&!number(argv[3],0,GLASS_BLE_LIMIT-1,&a)&&!number(argv[4],1,INT_MAX,&b))
    {uint32_t generation=0;ret=glass_ble_central_connect_session(a,b,&generation);
      if(!ret)printf("BLE_SESSION generation=%" PRIu32 "\n",generation);}
  else if((!strcmp(op,"devices")||!strcmp(op,"sessions"))&&argc==3){
    struct glass_ble_state *s=calloc(1,sizeof(*s));if(!s)return 1;
    glass_ble_read(s);
    printf("BLE_SCAN generation=%" PRIu32 " scanning=%u count=%u connected=%u connection_generation=%" PRIu32 "\n",
      s->scan_generation,(unsigned)s->scanning,s->count,(unsigned)s->connected,s->generation);
    for(unsigned i=0;i<s->count&&i<GLASS_BLE_LIMIT;i++){
      const struct glass_ble_device *p=&s->devices[i];
      printf("BLE_PEER index=%u type=%u address=%02x:%02x:%02x:%02x:%02x:%02x connectable=%u service128=%u\n",
        i,p->type,p->address[5],p->address[4],p->address[3],p->address[2],p->address[1],p->address[0],
        (unsigned)p->connectable,(unsigned)p->service128);
    }
    printf("BLE_SESSIONS active=%u connected=%u capacity=%u selected=%" PRIu32 "\n",
      s->session_count,s->connection_count,s->central_capacity,s->generation);
    for(unsigned i=0;i<s->session_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++){
      const struct glass_ble_session *v=&s->sessions[i];
      printf("BLE_LINK generation=%" PRIu32 " connecting=%u connected=%u encrypted=%u bonded=%u closing=%u mtu=%u error=%d mouse_paused=%u\n",
        v->generation,(unsigned)v->connecting,(unsigned)v->connected,(unsigned)v->encrypted,(unsigned)v->bonded,(unsigned)v->disconnecting,v->mtu,v->error,(unsigned)v->mouse_paused);
    }
    printf("BLE_RESTORES restoring=%u count=%u\n",(unsigned)s->restoring,s->restore_count);
    for(unsigned i=0;i<s->restore_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++){
      const struct glass_ble_restore_result *r=&s->restores[i];
      printf("BLE_RESTORE generation=%" PRIu32 " attempted=%u connected=%u error=%d\n",
        r->generation,(unsigned)r->attempted,(unsigned)r->connected,r->error);
    }
    free(s);ret=0;
  }else if(argc>=4&&!number(argv[3],1,INT_MAX,&a)){
    if(!strcmp(op,"select")&&argc==4){ret=glass_ble_central_select(a);}
    else if(!strcmp(op,"disconnect")&&argc==4){
      ret=glass_ble_central_disconnect_generation(a);
    }else{
      struct glass_gatt_request *request=calloc(1,sizeof(*request));
      struct glass_gatt_result *reply=calloc(1,sizeof(*reply));
      if(!request||!reply){free(request);free(reply);return 1;}
      request->generation=a;
      if(!strcmp(op,"services")&&argc==4)request->operation=GLASS_GATT_SERVICES;
      else if(!strcmp(op,"pair")&&argc==4)request->operation=GLASS_GATT_PAIR;
      else if(!strcmp(op,"mtu")&&argc==4)request->operation=GLASS_GATT_MTU;
      else if(!strcmp(op,"characteristics")&&argc==6&&!number(argv[4],1,65535,&b)&&!number(argv[5],b,65535,&c)){
        request->operation=GLASS_GATT_CHARACTERISTICS;request->start=b;request->end=c;
      }else if(!strcmp(op,"read")&&argc==5&&!number(argv[4],1,65535,&b)){
        request->operation=GLASS_GATT_READ;request->handle=b;
      }else if(strcmp(op,"notification")||argc!=4)goto done;
      ret=!strcmp(op,"notification")?glass_ble_notification(a,reply):glass_ble_gatt(request,reply,NULL);
      if(!ret){
        printf("BLE_GATT generation=%" PRIu32 " mtu=%u count=%u bytes=%u truncated=%u\n",
          reply->generation,reply->mtu,reply->count,(unsigned)reply->length,(unsigned)reply->truncated);
        for(unsigned i=0;i<reply->count&&i<GLASS_GATT_ITEMS;i++){
          const struct glass_gatt_item *item=&reply->items[i];
          printf("BLE_ATTR uuid=%s start=%u end=%u value=%u properties=%u\n",
            item->uuid,item->start,item->end,item->value,item->properties);
        }
        if(reply->length){
          uint32_t hash=2166136261u;
          for(size_t i=0;i<reply->length&&i<GLASS_GATT_BYTES;i++)hash=(hash^reply->data[i])*16777619u;
          printf("BLE_DATA_FNV32=%08" PRIx32 "\n",hash);
        }
      }
done:free(request);free(reply);
    }
  }
  printf("c6ble central %s result=%d\n",op,ret);
  return ret?1:0;
}

static int channel_command(int argc,char **argv)
{
  const char *op=argv[2];int epoch=0,ret=-EINVAL;
  if(argc==3&&!strcmp(op,"status")){
    struct glass_ble_channel_state s;glass_ble_channel_read(&s);
    printf("BLE_CHANNEL ready=%u connected=%u encrypted=%u notify=%u epoch=%" PRIu32 " connection=%u mtu=%u payload=%u queued=%u received=%" PRIu32 " sent=%" PRIu32 " rejected=%" PRIu32 " error=%d\n",
      (unsigned)s.ready,(unsigned)s.connected,(unsigned)s.encrypted,(unsigned)s.subscribed,s.epoch,s.connection,s.mtu,s.payload,s.queued,s.received,s.sent,s.rejected,s.error);
    printf("BLE_CHANNEL_UUID service=%s rx=%s tx=%s info=%s\n",GLASS_BLE_CHANNEL_SERVICE,GLASS_BLE_CHANNEL_RX,GLASS_BLE_CHANNEL_TX,GLASS_BLE_CHANNEL_INFO);ret=0;
  }else if(argc>=4&&!number(argv[3],1,INT_MAX,&epoch)){
    uint8_t data[GLASS_BLE_CHANNEL_BYTES];size_t length=sizeof(data);
    if(argc==4&&!strcmp(op,"receive")){
      ret=glass_ble_channel_receive(epoch,data,&length);
      if(!ret){uint32_t hash=2166136261u;for(size_t i=0;i<length;i++)hash=(hash^data[i])*16777619u;
        printf("BLE_CHANNEL_DATA bytes=%u fnv32=%08" PRIx32 "\n",(unsigned)length,hash);}
    }else if(argc==5&&!strcmp(op,"send")){
      size_t n=strlen(argv[4]);if(!n||(n%2)||n>sizeof(data)*2)goto done;
      for(size_t i=0;i<n/2;i++){
        unsigned value=0;
        for(unsigned j=0;j<2;j++){
          unsigned c=(unsigned char)argv[4][i*2+j];
          if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;
          else if(c>='A'&&c<='F')c=c-'A'+10;else goto done;
          value=value*16+c;
        }
        data[i]=value;
      }
      ret=glass_ble_channel_send(epoch,data,n/2,NULL);
    }
  }
done:printf("c6ble channel %s result=%d\n",op,ret);return ret?1:0;
}

static int input_command(int argc,char **argv)
{
  int value,ret=-EINVAL;
  if(argc==5&&!strcmp(argv[3],"begin")&&!number(argv[4],1,INT_MAX,&value)){
    uint32_t session=0;ret=glass_ble_hid_input_begin(value,&session);
    if(!ret)printf("HID_INPUT_SESSION id=%" PRIu32 " epoch=%d\n",session,value);
  }else if(argc==5&&!number(argv[4],1,INT_MAX,&value)){
    if(!strcmp(argv[3],"end"))ret=glass_ble_hid_input_end(value);
    else if(!strcmp(argv[3],"keepalive"))ret=glass_ble_hid_input_keepalive(value);
  }else if(argc==9&&!strcmp(argv[3],"report")){
    int buttons,x,y,wheel;
    if(!number(argv[4],1,INT_MAX,&value)&&!number(argv[5],0,7,&buttons)&&
       !number(argv[6],-1024,1024,&x)&&!number(argv[7],-1024,1024,&y)&&
       !number(argv[8],-1024,1024,&wheel))ret=glass_ble_hid_input_report(value,buttons,x,y,wheel);
  }
  printf("HID_INPUT result=%d scope=queued_not_peer_receipt\n",ret);return ret?1:0;
}

int main(int argc, char **argv)
{
  int x, y, wheel = 0, buttons = 0;
  const char *cmd;
  if(argc>=4&&!strcmp(argv[1],"mouse")&&!strcmp(argv[2],"input"))return input_command(argc,argv);
  if(argc==3&&!strcmp(argv[1],"mouse")&&!strcmp(argv[2],"diagnostics")){
    struct glass_ble_hid_input_state s;glass_ble_hid_input_read(&s);
    printf("HID_INPUT session=%" PRIu32 " epoch=%" PRIu32 " queued=%u releasing=%u accepted=%" PRIu32 " submitted=%" PRIu32 " merged=%" PRIu32 " retries=%" PRIu32 " discarded=%" PRIu32 "\n",
      s.session,s.epoch,s.queued,(unsigned)s.releasing,s.accepted,s.submitted,s.merged,s.retries,s.discarded);
    printf("HID_LINK interval=%u latency=%u timeout=%u request=%d status=%d\n",
      s.interval,s.latency,s.supervision_timeout,s.parameter_result,s.parameter_status);
    printf("HID_LINK_WORK attempts=%u updates=%u\n",s.parameter_attempts,s.parameter_updates);
    printf("HID_DISCONNECT total=%" PRIu32 " unexpected=%" PRIu32 " reason=%d epoch=%" PRIu32 " attempts=%u updates=%u interval=%u secure_age_ms=%" PRIu64 "\n",
      s.disconnects,s.unexpected_disconnects,s.last_disconnect_reason,s.last_disconnect_epoch,
      s.last_parameter_attempts,s.last_parameter_updates,s.last_interval,s.last_secure_age_ms);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "timers"))
    {
      struct ble_npl_nuttx_callout_stats stats;
      ble_npl_nuttx_callout_read(&stats);
      printf("NPL_CALLOUT expirations=%" PRIu32 " dispatched=%" PRIu32
             " coalesced=%" PRIu32 " pending=%" PRIu32 " highwater=%" PRIu32 "\n",
             stats.expirations, stats.dispatched, stats.coalesced,
             stats.pending, stats.highwater);
      return 0;
    }
  if(argc>=3&&!strcmp(argv[1],"channel"))return channel_command(argc,argv);
  if(argc>=3&&!strcmp(argv[1],"central"))return central_command(argc,argv);
  if (argc == 2 && !strcmp(argv[1], "register"))
    return result("register", c6_ble_register("/dev/ttyHCI0"));
  if (argc == 2 && !strcmp(argv[1], "identity"))
    {
      printf("type=static-random address=%s name=%s\n",
             glass_ble_identity_address(), glass_ble_identity_name());
      return 0;
    }
  if (argc == 2 && !strcmp(argv[1], "status"))
    {
      struct glass_ble_state device;
      struct glass_ble_hid_state mouse;
      struct glass_ble_capacity_state capacity;

      glass_ble_read(&device);
      glass_ble_hid_read(&mouse);
      glass_ble_capacity_read(&capacity);
      printf("configured_connections=%u effective_connections=%u "
             "host_ready=%u device_connected=%u mouse_connected=%u store_error=%d\n",
             (unsigned)CONFIG_NIMBLE_BLE_MAX_CONN, glass_ble_capacity(),
             (unsigned)device.ready,
             (unsigned)(device.connected && device.peer_valid),
             (unsigned)mouse.connected, device.store_error);
      printf("reserved_connections=%u central_reserved=%u hid_reserved=%u central_reservations=%u device_connections=%u "
             "mixed_role_capacity=%u mouse_paused=%u\n", capacity.reserved,
             (unsigned)capacity.central_reserved, (unsigned)capacity.hid_reserved,
             capacity.central_reservations,device.connection_count,
             0u,(unsigned)device.mouse_paused);
      return 0;
    }
  if (argc < 3 || (strcmp(argv[1], "mouse") && strcmp(argv[1], "hid")))
    return usage();
  cmd = argv[2];
  if (argc == 3)
    {
      if (!strcmp(cmd, "start")) return result(cmd, glass_ble_hid_start());
      if (!strcmp(cmd, "stop")) return result(cmd, glass_ble_hid_stop());
      if (!strcmp(cmd, "release")) return result(cmd, glass_ble_hid_release());
      if (!strcmp(cmd, "status"))
        {
          struct glass_ble_hid_state s;
          glass_ble_hid_read(&s);
          printf("host_ready=%d enabled=%d advertising=%d connected=%d "
                 "encrypted=%d notify=%d suspended=%d boot=%d buttons=%u "
                 "epoch=%lu error=%d ble_error=%d store_error=%d\n",
                 s.host_ready, s.enabled, s.advertising, s.connected,
                 s.encrypted, s.mouse_notify, s.suspended, s.boot_mode,
                 (unsigned)s.buttons, (unsigned long)s.epoch, s.error, s.ble_error,
                 s.store_error);
          return 0;
        }
    }
  if (!strcmp(cmd, "click") && argc == 4)
    {
      if (!strcmp(argv[3], "left")) buttons = 1;
      else if (!strcmp(argv[3], "right")) buttons = 2;
      else if (!strcmp(argv[3], "middle")) buttons = 4;
      else return usage();
      return result(cmd, glass_ble_hid_click((unsigned)buttons));
    }
  if (!strcmp(cmd, "scroll") && argc == 4)
    {
      if (number(argv[3], -127, 127, &wheel)) return usage();
      return result(cmd, glass_ble_hid_mouse(0, 0, 0, wheel));
    }
  if (!strcmp(cmd, "move") && argc >= 5 && argc <= 7)
    {
      if (number(argv[3], -127, 127, &x) || number(argv[4], -127, 127, &y) ||
          (argc >= 6 && number(argv[5], -127, 127, &wheel)) ||
          (argc == 7 && number(argv[6], 0, 7, &buttons))) return usage();
      return result(cmd, glass_ble_hid_mouse(buttons, x, y, wheel));
    }
  return usage();
}
