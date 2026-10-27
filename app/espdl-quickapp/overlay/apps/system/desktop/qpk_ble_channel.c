/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_hardware.h"
#include "glass_ble_channel.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

int qpk_ble_channel_execute(const char *op,const cJSON *args,cJSON *out,
                            struct qpk_hw_progress *progress)
{
  if(!strcmp(op,"channelStatus")){
    if(cJSON_GetArraySize(args))return -EINVAL;
    struct glass_ble_channel_state s;glass_ble_channel_read(&s);
    cJSON_AddBoolToObject(out,"ready",s.ready);cJSON_AddBoolToObject(out,"connected",s.connected);
    cJSON_AddBoolToObject(out,"encrypted",s.encrypted);cJSON_AddBoolToObject(out,"notify",s.subscribed);
    cJSON_AddBoolToObject(out,"sharedWithHid",true);cJSON_AddNumberToObject(out,"epoch",s.epoch);
    cJSON_AddNumberToObject(out,"payload",s.payload);cJSON_AddNumberToObject(out,"queued",s.queued);
    cJSON_AddNumberToObject(out,"received",s.received);cJSON_AddNumberToObject(out,"sent",s.sent);
    cJSON_AddNumberToObject(out,"rejected",s.rejected);cJSON_AddNumberToObject(out,"error",s.error);
    cJSON_AddStringToObject(out,"service",GLASS_BLE_CHANNEL_SERVICE);return 0;
  }
  bool send=!strcmp(op,"channelSend"),receive=!strcmp(op,"channelReceive");
  if(!send&&!receive)return -ENOSYS;
  const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(args,"epoch"),*field;
  if(!cJSON_IsNumber(epoch)||epoch->valuedouble<1||epoch->valuedouble>INT_MAX||
     epoch->valuedouble!=epoch->valueint)return -EINVAL;
  cJSON_ArrayForEach(field,args){
    if(!field->string||(strcmp(field->string,"epoch")&&(!send||strcmp(field->string,"data"))))return -EINVAL;
  }
  uint8_t bytes[GLASS_BLE_CHANNEL_BYTES];size_t length=sizeof(bytes);int ret;
  if(send){
    const cJSON *data=cJSON_GetObjectItemCaseSensitive(args,"data");int count=cJSON_GetArraySize(data);
    if(!cJSON_IsArray(data)||count<1||count>GLASS_BLE_CHANNEL_BYTES)return -EINVAL;
    length=0;cJSON_ArrayForEach(field,data){
      if(!cJSON_IsNumber(field)||field->valuedouble<0||field->valuedouble>255||field->valuedouble!=field->valueint)return -EINVAL;
      bytes[length++]=field->valueint;
    }
    ret=glass_ble_channel_send(epoch->valueint,bytes,length,&progress->cancel);
    if(!ret)cJSON_AddStringToObject(out,"scope","nimble_submitted_not_peer_readback");
  }else{
    ret=glass_ble_channel_receive(epoch->valueint,bytes,&length);
    if(!ret){cJSON *data=cJSON_AddArrayToObject(out,"data");if(!data)return -ENOMEM;
      for(size_t i=0;i<length;i++){cJSON *v=cJSON_CreateNumber(bytes[i]);if(!v)return -ENOMEM;cJSON_AddItemToArray(data,v);}}
  }
  if(!ret){cJSON_AddNumberToObject(out,"epoch",epoch->valueint);cJSON_AddNumberToObject(out,"bytes",length);}
  return ret;
}
