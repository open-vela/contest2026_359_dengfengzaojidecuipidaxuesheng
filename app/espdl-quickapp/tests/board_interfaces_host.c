/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_hardware.h"
#include "qpk_board_pins.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned probes;
bool qpk_board_endpoint(const char *path,bool directory)
{
  probes++;
  if(directory)return !strcmp(path,"/data"); /* SD absent */
  return strcmp(path,"net:eth0")&&strcmp(path,"/dev/ttyS0");
}
static cJSON *get(cJSON *o,const char *key){cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);assert(v);return v;}
static void boolean(cJSON *o,const char *key,bool yes){assert(yes?cJSON_IsTrue(get(o,key)):cJSON_IsFalse(get(o,key)));}
int main(void)
{
  char *text=qpk_hw_capabilities();assert(text&&strlen(text)<16384);
  cJSON *r=cJSON_Parse(text);free(text);assert(r);assert(get(r,"version")->valueint==3);
  cJSON *pins=get(r,"gpioPins");assert(cJSON_GetArraySize(pins)==5);
  for(int i=0;i<5;i++)assert(cJSON_GetArrayItem(pins,i)->valueint==i+2);
  assert(cJSON_GetArraySize(get(r,"reservedPins"))==2);
  cJSON *interfaces=get(get(r,"boardInterfaces"),"interfaces");assert(cJSON_GetArraySize(interfaces)==21);
  unsigned compiled=0;cJSON *item;
  cJSON_ArrayForEach(item,interfaces){
    boolean(item,"documented",true);const char *id=get(item,"id")->valuestring;
    for(cJSON *next=item->next;next;next=next->next)assert(strcmp(id,get(next,"id")->valuestring));
    if(cJSON_IsTrue(get(item,"compiled"))){compiled++;if(!cJSON_GetObjectItem(item,"endpoint")&&!cJSON_GetObjectItem(item,"blockedBy"))assert(cJSON_IsNull(get(item,"available")));}
    else boolean(item,"available",false);

#ifndef FEATURES_OFF
    if(!strcmp(id,"storage_sd")||!strcmp(id,"ethernet")||!strcmp(id,"uart"))boolean(item,"available",false);
    if(!strcmp(id,"camera_usb")){boolean(item,"compiled",true);boolean(item,"available",true);}
#endif
  }
#ifdef FEATURES_OFF
  assert(compiled==1&&probes==0);assert(!cJSON_GetArraySize(get(r,"i2c")));
  boolean(get(r,"audio"),"record",false);boolean(get(r,"bluetooth"),"scan",false);
#else
  assert(compiled==21&&probes==16);boolean(get(r,"audio"),"record",true);
#ifdef LEGACY_PINS
  assert(!cJSON_GetArraySize(get(r,"i2c"))&&!cJSON_GetArraySize(get(r,"spi")));
  assert(cJSON_GetArraySize(get(r,"pwm"))==3);
  assert(QPK_I2C_MASK&~QPK_HEADER_GPIO_MASK);assert(QPK_SPI_MASK&~QPK_HEADER_GPIO_MASK);
#else
  assert(get(cJSON_GetArrayItem(get(r,"i2c"),0),"sda")->valueint==2);
  assert(get(cJSON_GetArrayItem(get(r,"spi"),0),"cs")->valueint==6);
#ifdef BACKLIGHT_PINS
  assert(cJSON_GetArraySize(get(r,"pwm"))==3);
  cJSON_ArrayForEach(item,get(r,"pwm"))assert(get(item,"channel")->valueint!=0&&get(item,"pin")->valueint!=26);
#else
  assert(get(cJSON_GetArrayItem(get(r,"pwm"),0),"pin")->valueint==2);
#endif
  assert(QPK_I2C_MASK==12&&QPK_SPI_MASK==120&&QPK_UART_MASK==48);
#endif
#endif
#ifdef FEATURES_OFF
  assert(get(get(r,"bluetooth"),"maxOrdinaryConnections")->valueint==0);
#elif defined(LEGACY_PINS)
  assert(get(get(r,"bluetooth"),"maxOrdinaryConnections")->valueint==1);
#else
  assert(get(get(r,"bluetooth"),"maxOrdinaryConnections")->valueint==2);
#endif
  assert(QPK_PIN_MASK(63)&~QPK_HEADER_GPIO_MASK);
  cJSON_Delete(r);puts("BOARD_INTERFACES_OK: truthful feature/endpoint separation, unique catalog, reserved pins and resolved routes");
  return 0;
}
