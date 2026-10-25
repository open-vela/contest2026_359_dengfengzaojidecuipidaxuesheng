/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_ble.h"
#include <stddef.h>
#include <string.h>

/* Advertising strings are untrusted, length-delimited UTF-8. Keep complete
 * codepoints, replace malformed bytes and flatten control characters so a
 * nearby transmitter cannot insert extra rows into the device list.
 */
static inline size_t glass_ble_name_copy(char *out, const uint8_t *data,
                                         size_t length)
{
  size_t used = 0;
  for (size_t i = 0; i < length && used < GLASS_BLE_NAME_MAX;)
    {
      uint8_t c = data[i];
      size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                 c >= 0xe0 && c <= 0xef ? 3 :
                 c >= 0xf0 && c <= 0xf4 ? 4 : 0;
      bool valid = n && n <= length - i;
      for (size_t j = 1; valid && j < n; j++)
        valid = (data[i + j] & 0xc0) == 0x80;
      if (valid && n >= 3)
        valid = !((c == 0xe0 && data[i + 1] < 0xa0) ||
                  (c == 0xed && data[i + 1] >= 0xa0) ||
                  (c == 0xf0 && data[i + 1] < 0x90) ||
                  (c == 0xf4 && data[i + 1] >= 0x90));
      if (!valid)
        {
          out[used++] = '?';
          i++;
          continue;
        }
      if (n > GLASS_BLE_NAME_MAX - used) break;
      if (n == 1 && (c < 0x20 || c == 0x7f))
        out[used++] = ' ';
      else
        {
          memcpy(out + used, data + i, n);
          used += n;
        }
      i += n;
    }
  while (used && out[used - 1] == ' ') used--;
  out[used] = '\0';
  return used;
}

/* Merge advertisement and scan-response names for the same address. A
 * complete local name (0x09) takes priority over a shortened name (0x08).
 * A later packet without a name must not erase one already discovered.
 */
static inline void glass_ble_update_name(struct glass_ble_device *device,
                                         const uint8_t *data, size_t length)
{
  if (!data) return;
  while (length)
    {
      size_t field = data[0];
      if (!field || field >= length) break;
      if ((data[1] == 0x08 || data[1] == 0x09) && field > 1)
        {
          char name[GLASS_BLE_NAME_MAX + 1] = {0};
          size_t used = glass_ble_name_copy(name, data + 2, field - 1);
          bool complete = data[1] == 0x09;
          if (used && (complete || (!device->name_complete &&
                                   used > strlen(device->name))))
            {
              memcpy(device->name, name, sizeof(device->name));
              device->name_complete = complete;
            }
        }
      data += field + 1;
      length -= field + 1;
    }
}

/* These are advertising hints, never authenticated device identities.
 * Bluetooth SIG Assigned Numbers: Appearance, Service UUID, AD type tables.
 * Only retain specific, recognized services; Battery/Device Information alone
 * do not establish a physical device type. Merge scan responses without
 * erasing the connectability observed in the original advertising PDU. */
static inline void glass_ble_update_advertising(struct glass_ble_device *device,
                                                unsigned event_type,
                                                const uint8_t *data,size_t length)
{
  if(event_type<=3){
    device->connectable_known=true;
    device->connectable=event_type==0||event_type==1;
  }
  glass_ble_update_name(device,data,length);
  if(!data)return;
  while(length){
    size_t field=data[0];if(!field||field>=length)break;
    unsigned type=data[1];const uint8_t *value=data+2;size_t bytes=field-1;
    if(type==0x19&&bytes==2)device->appearance=value[0]|((uint16_t)value[1]<<8);
    if(((type==2||type==3)&&bytes>=2&&!(bytes%2))||(type==0x16&&bytes>=2)){
      size_t end=type==0x16?2:bytes;
      for(size_t i=0;i<end;i+=2){
        unsigned uuid=value[i]|((unsigned)value[i+1]<<8);
        switch(uuid){
        case 0x1812:case 0x180d:case 0x1809:case 0x181a:case 0x1810:
        case 0x1816:case 0x1818:case 0x1826:
          if(!device->service_hint||uuid<device->service_hint)device->service_hint=uuid;
          break;
        }
      }
    }
    if(((type==6||type==7)&&bytes>=16&&!(bytes%16))||(type==0x21&&bytes>=16))
      device->service128=true;
    data+=field+1;length-=field+1;
  }
}

static inline unsigned glass_ble_device_priority(const struct glass_ble_device *device)
{return (device->bonded?8:0)+(device->name[0]?4:0)+
        (device->connectable_known&&device->connectable?2:0);}
