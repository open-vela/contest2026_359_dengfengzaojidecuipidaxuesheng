/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "glass_ble_central.h"
#include "qpk_hardware.h"
#include <nuttx/usb/board_usb.h>
static unsigned io_calls,gatt_calls;static bool removed;static unsigned long mount_flags;
static int device_open(const char *p,int flags,...)
{if(!strcmp(p,"/dev/boardusb"))return 600;mode_t mode=0600;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}return open(p,flags,mode);}
static int device_close(int fd){return fd==600?0:close(fd);}
static int device_ioctl(int fd,unsigned long cmd,...)
{
  assert(fd==600);io_calls++;va_list ap;va_start(ap,cmd);void *arg=(void *)va_arg(ap,unsigned long);va_end(ap);
  if(cmd==BOARDUSBIOC_TRANSFER){struct board_usb_transfer *r=arg;
    if(removed){errno=ENODEV;return -1;}if(r->generation!=2){errno=ESTALE;return -1;}
    r->info=(struct board_usb_info){.id=0,.generation=2,.kind=BOARD_USB_MSC,.connected=true};strcpy(r->info.path,"/dev/usbblk0");return 0;}
  if(cmd==BOARDUSBIOC_MODE)return 0;
  if(cmd==BOARDUSBIOC_LIST){struct board_usb_list *l=arg;memset(l,0,sizeof(*l));l->version=1;return 0;}
  errno=ENOTTY;return -1;
}
static int device_mount(const char *dev,const char *root,const char *type,unsigned long flags,const void *data)
{(void)dev;(void)root;(void)type;(void)data;mount_flags=flags;return 0;}
static int device_umount(const char *path){(void)path;return 0;}
#define open device_open
#define close device_close
#define ioctl device_ioctl
#define mount device_mount
#define umount device_umount
#include "qpk_usb.c"
#undef open
#undef close
#undef ioctl
#undef mount
#undef umount
#include "qpk_ble_gatt.c"
#include "qpk_ble_channel.c"
int glass_ble_gatt(const struct glass_gatt_request *r,struct glass_gatt_result *o,atomic_bool *cancel)
{assert(!atomic_load(cancel));gatt_calls++;if(r->generation!=3)return -ESTALE;memset(o,0,sizeof(*o));o->generation=3;o->mtu=23;return 0;}
int glass_ble_notification(uint32_t gen,struct glass_gatt_result *out){(void)gen;(void)out;return -EAGAIN;}
static unsigned channel_calls;
void glass_ble_channel_read(struct glass_ble_channel_state *s)
{memset(s,0,sizeof(*s));s->ready=s->connected=s->encrypted=s->subscribed=true;s->epoch=4;s->payload=20;}
int glass_ble_channel_send(uint32_t epoch,const uint8_t *data,size_t length,atomic_bool *cancel)
{channel_calls++;assert(!atomic_load(cancel));if(epoch!=4)return -ESTALE;assert(length==2&&data[0]==1&&data[1]==2);return 0;}
int glass_ble_channel_receive(uint32_t epoch,uint8_t *data,size_t *length)
{channel_calls++;if(epoch!=4)return -ESTALE;assert(*length>=2);data[0]=1;data[1]=2;*length=2;return 0;}
static struct qpk_hw_progress progress;
static void *session;
static int usb(const char *operation,const char *json)
{cJSON *args=cJSON_Parse(json),*out=cJSON_CreateObject();assert(args&&out);int ret=qpk_usb_execute(&session,operation,args,out,&progress);cJSON_Delete(args);cJSON_Delete(out);return ret;}
static int ble(const char *operation,const char *json)
{cJSON *args=cJSON_Parse(json),*out=cJSON_CreateObject();assert(args&&out);int ret=qpk_ble_gatt_execute(operation,args,out,&progress);cJSON_Delete(args);cJSON_Delete(out);return ret;}
static int channel(const char *operation,const char *json)
{cJSON *args=cJSON_Parse(json),*out=cJSON_CreateObject();assert(args&&out);int ret=qpk_ble_channel_execute(operation,args,out,&progress);
 if(!ret&&!strcmp(operation,"channelStatus"))assert(cJSON_IsTrue(cJSON_GetObjectItem(out,"sharedWithHid"))&&cJSON_GetObjectItem(out,"epoch")->valueint==4);
 if(!ret&&!strcmp(operation,"channelReceive"))assert(cJSON_GetArraySize(cJSON_GetObjectItem(out,"data"))==2);
 cJSON_Delete(args);cJSON_Delete(out);return ret;}
int main(void)
{
  assert(usb("mode","{\"port\":0,\"role\":1,\"extra\":1}")==-EINVAL&&!io_calls);
  assert(usb("info","{\"id\":0,\"generation\":1}")==-ESTALE);
  assert(usb("info","{\"id\":0.1,\"generation\":2}")==-EINVAL);
  assert(usb("hidReport","{\"id\":0,\"generation\":2,\"reportType\":2,\"data\":[256]}")==-EINVAL);
  assert(!usb("mount","{\"id\":0,\"generation\":2}"));assert(mount_flags&MS_RDONLY);
  assert(usb("writeFile","{\"id\":0,\"generation\":2,\"path\":\"test.bin\",\"data\":[1,2]}")==-EROFS);
  assert(usb("readFile","{\"id\":0,\"generation\":2,\"path\":\"../escape\"}")==-EINVAL);
  assert(!usb("unmount","{\"id\":0,\"generation\":2}"));
  assert(!usb("mount","{\"id\":0,\"generation\":2,\"writable\":true}"));assert(!mount_flags);
  assert(!usb("writeFile","{\"id\":0,\"generation\":2,\"path\":\"test.bin\",\"data\":[1,2]}"));
  assert(usb("writeFile","{\"id\":0,\"generation\":2,\"path\":\"test.bin\",\"data\":[3]}")==-EEXIST);
  assert(!usb("readFile","{\"id\":0,\"generation\":2,\"path\":\"test.bin\"}"));
  char path[256];snprintf(path,sizeof(path),QPK_USB_MOUNT_ROOT "/usb0/link");assert(!symlink("/etc/passwd",path));
  assert(usb("readFile","{\"id\":0,\"generation\":2,\"path\":\"link\"}")==-EINVAL);unlink(path);
  removed=true;assert(!usb("unmount","{\"id\":0,\"generation\":2}"));
  atomic_store(&progress.cancel,true);unsigned before=io_calls;assert(usb("list","{}")==-ECANCELED&&io_calls==before);atomic_store(&progress.cancel,false);
  qpk_usb_destroy(session);snprintf(path,sizeof(path),QPK_USB_MOUNT_ROOT "/usb0/test.bin");unlink(path);
  assert(ble("write","{\"generation\":3,\"handle\":1,\"data\":[-1]}")==-EINVAL&&!gatt_calls);
  assert(ble("read","{\"generation\":3,\"handle\":1,\"extra\":1}")==-EINVAL&&!gatt_calls);
  assert(ble("characteristics","{\"generation\":3,\"start\":5,\"end\":2}")==-EINVAL&&!gatt_calls);
  assert(ble("read","{\"generation\":2,\"handle\":1}")==-ESTALE);
  assert(!ble("read","{\"generation\":3,\"handle\":1}"));
  assert(!channel("channelStatus","{}"));
  assert(channel("channelStatus","{\"epoch\":4}")==-EINVAL&&!channel_calls);
  assert(channel("channelSend","{\"epoch\":4,\"data\":[256]}")==-EINVAL&&!channel_calls);
  assert(channel("channelSend","{\"epoch\":4,\"data\":[1,2],\"extra\":1}")==-EINVAL&&!channel_calls);
  assert(channel("channelSend","{\"epoch\":0,\"data\":[1,2]}")==-EINVAL&&!channel_calls);
  assert(channel("channelSend","{\"epoch\":4,\"data\":[]}")==-EINVAL&&!channel_calls);
  assert(channel("channelReceive","{\"epoch\":3}")==-ESTALE);
  assert(!channel("channelSend","{\"epoch\":4,\"data\":[1,2]}"));
  assert(!channel("channelReceive","{\"epoch\":4}"));
  puts("INTERFACE_PROTOCOLS_OK: strict JSON, generation checks, read-only default, explicit file writes, traversal/symlink rejection, unplug unmount, cancellation and GATT bounds");return 0;
}
