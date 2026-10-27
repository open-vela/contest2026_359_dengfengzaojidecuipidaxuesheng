/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble_hid_link.h"
#include "host/ble_hs.h"
#include <string.h>

static int refresh(struct glass_ble_hid_link *s)
{
  struct ble_gap_conn_desc desc;
  int ret=ble_gap_conn_find(s->connection,&desc);
  if(!ret){s->interval=desc.conn_itvl;s->latency=desc.conn_latency;
    s->timeout=desc.supervision_timeout;}
  return ret;
}
void glass_ble_hid_link_state(struct glass_ble_hid_link *s,uint16_t handle,
                              uint32_t epoch,bool connected,bool secure,uint64_t now)
{
  if(!connected || !s->connected || s->epoch!=epoch || s->connection!=handle){
    memset(s,0,sizeof(*s));s->connection=handle;s->epoch=epoch;
  }
  if(secure&&!s->secure)s->secured_at=now;
  s->connected=connected;s->secure=connected&&secure;
}
void glass_ble_hid_link_poll(struct glass_ble_hid_link *s,uint64_t now)
{
  (void)now;
  if(!s->connected||!s->secure||s->sampled)return;
  /* A shorter retry delay is not evidence of an interop fix. Keep connection
   * establishment free of an additional peripheral parameter procedure. */
  s->sampled=true;
  s->result=refresh(s);
}
void glass_ble_hid_link_changed(struct glass_ble_hid_link *s,uint16_t handle,int status)
{
  if(!s->connected||s->connection!=handle)return;
  s->changed++;
  s->status=status;
  if(!status)(void)refresh(s);
}
