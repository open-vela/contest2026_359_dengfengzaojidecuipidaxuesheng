/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble_central.h"

enum {BLE_LIST_SCAN=1,BLE_LIST_CONNECT,BLE_LIST_CANCEL,BLE_LIST_DISCONNECT,BLE_LIST_MOUSE,BLE_LIST_PAIR,BLE_LIST_SCAN_STOP,BLE_LIST_SELECT};
struct ble_list_job {
  unsigned operation,index;
  uint32_t scan_generation,generation;
  bool peer_valid;
  struct glass_ble_device peer;
};
/* No page/object pointers cross this mailbox. The desktop worker only calls
 * the public BLE boundary; GAP/GATT remain on the persistent host task group. */
static pthread_mutex_t g_ble_list_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ble_list_wake=PTHREAD_COND_INITIALIZER;
static atomic_bool g_ble_list_pair_cancel;
static struct {
  bool started,busy,pending,cancel;
  unsigned operation;
  int result;
  struct ble_list_job job;
} g_ble_list_jobs;

struct ble_list_row {
  lv_obj_t *root,*name,*address,*signal,*action,*arrow;
  struct ble_list_job binding;
};
struct ble_list_ui {
  lv_obj_t *root,*status,*detail,*scan,*control,*list,*count,*empty,*broadcasts;
  struct ble_list_row rows[GLASS_BLE_LIMIT];
  struct glass_ble_state state,previous;
  struct glass_ble_hid_state hid;
  struct ble_list_job pressed;
  lv_obj_t *pressed_row;
  lv_timer_t *timer;
  unsigned action,previous_operation;
  int previous_result;
  bool valid,previous_busy,previous_hid,expanded;
};
static struct ble_list_ui *g_ble_list;
static void glass_ble_list_refresh(lv_timer_t *timer);

static bool ble_list_same_peer(const struct glass_ble_device *a,const struct glass_ble_device *b)
{return a->type==b->type&&!memcmp(a->address,b->address,6);}
static bool ble_list_cancelled(void)
{
  pthread_mutex_lock(&g_ble_list_lock);bool cancel=g_ble_list_jobs.cancel;
  pthread_mutex_unlock(&g_ble_list_lock);return cancel;
}
static int ble_list_wait_ready(void)
{
  if(ble_list_cancelled())return -ECANCELED;
  int result=glass_ble_start();if(result)return result;
  for(unsigned i=0;i<1500;i++){
    if(ble_list_cancelled())return -ECANCELED;
    struct glass_ble_state state;glass_ble_read(&state);
    if(state.ready)return 0;
    if(state.error)return state.error;
    usleep(100000);
  }
  return -ETIMEDOUT;
}
static int ble_list_execute(struct ble_list_job *job)
{
  if(job->operation==BLE_LIST_SCAN||job->operation==BLE_LIST_CONNECT){
    int result=ble_list_wait_ready();if(result)return result;
  }
  struct glass_ble_state state;glass_ble_read(&state);
  if(ble_list_cancelled())return -ECANCELED;
  switch(job->operation){
  case BLE_LIST_SCAN:return glass_ble_central_scan_session(NULL,&job->scan_generation);
  case BLE_LIST_SELECT:return glass_ble_central_select(job->generation);
  case BLE_LIST_SCAN_STOP:return glass_ble_central_scan_stop(job->scan_generation);
  case BLE_LIST_CONNECT:
    if(state.scan_generation!=job->scan_generation||job->index>=state.count||
       job->index>=GLASS_BLE_LIMIT||!ble_list_same_peer(&state.devices[job->index],&job->peer))return -ESTALE;
    if(state.scanning){int result=glass_ble_central_scan_stop(job->scan_generation);if(result)return result;}
    if(ble_list_cancelled())return -ECANCELED;
    glass_ble_read(&state);
    if(state.scan_generation!=job->scan_generation||job->index>=state.count||
       job->index>=GLASS_BLE_LIMIT||!ble_list_same_peer(&state.devices[job->index],&job->peer))return -ESTALE;
    /* The host validates the scan generation again after the scan stops. */
    return glass_ble_central_connect_session(job->index,job->scan_generation,&job->generation);
  case BLE_LIST_CANCEL:
    return job->peer_valid&&job->generation?
      glass_ble_central_disconnect_generation(job->generation):
      glass_ble_central_scan_stop(job->scan_generation);
  case BLE_LIST_DISCONNECT:
    return glass_ble_central_disconnect_generation(job->generation);
  case BLE_LIST_PAIR:{
    if(!state.connected||!state.peer_valid||state.disconnecting)return -ENOTCONN;
    if(state.generation!=job->generation||!ble_list_same_peer(&state.peer,&job->peer))return -ESTALE;
    struct glass_gatt_request request={.operation=GLASS_GATT_PAIR,.generation=job->generation};
    struct glass_gatt_result reply;
    int result=glass_ble_gatt(&request,&reply,&g_ble_list_pair_cancel);
    if(result)return result;
    glass_ble_read(&state);
    if(state.generation!=job->generation||!state.peer_valid||
       !ble_list_same_peer(&state.peer,&job->peer))return -ESTALE;
    return state.connected&&state.encrypted?0:-EACCES;
  }
  default:return -EINVAL;
  }
}
static void *glass_ble_list_worker(void *unused)
{
  LV_UNUSED(unused);
  for(;;){
    pthread_mutex_lock(&g_ble_list_lock);
    while(!g_ble_list_jobs.pending)pthread_cond_wait(&g_ble_list_wake,&g_ble_list_lock);
    struct ble_list_job job=g_ble_list_jobs.job;g_ble_list_jobs.pending=false;
    pthread_mutex_unlock(&g_ble_list_lock);
    int result=ble_list_execute(&job);
    pthread_mutex_lock(&g_ble_list_lock);bool cancelled=g_ble_list_jobs.cancel;
    if(!cancelled||result||(job.operation!=BLE_LIST_SCAN&&job.operation!=BLE_LIST_CONNECT)){
      g_ble_list_jobs.result=result;g_ble_list_jobs.busy=false;g_ble_list_jobs.cancel=false;
      pthread_mutex_unlock(&g_ble_list_lock);continue;
    }
    pthread_mutex_unlock(&g_ble_list_lock);
    {
      struct glass_ble_state state;glass_ble_read(&state);
      if(job.operation==BLE_LIST_CONNECT&&job.generation){
        /* The returned session identity belongs to this accepted request,
         * even when another connection has become the UI selection. */
        (void)glass_ble_central_disconnect_generation(job.generation);
      }else if(job.operation==BLE_LIST_SCAN&&state.scanning&&
               state.scan_generation==job.scan_generation)(void)glass_ble_central_scan_stop(state.scan_generation);
      result=-ECANCELED;
    }
    pthread_mutex_lock(&g_ble_list_lock);
    g_ble_list_jobs.result=result;g_ble_list_jobs.busy=false;g_ble_list_jobs.cancel=false;
    pthread_mutex_unlock(&g_ble_list_lock);
  }
  return NULL;
}
static int glass_ble_list_submit(const struct ble_list_job *job)
{
  pthread_mutex_lock(&g_ble_list_lock);
  if(g_ble_list_jobs.busy){
    if(job->operation==BLE_LIST_CANCEL&&
       (g_ble_list_jobs.operation==BLE_LIST_SCAN||g_ble_list_jobs.operation==BLE_LIST_CONNECT||
        g_ble_list_jobs.operation==BLE_LIST_PAIR)){
      if(g_ble_list_jobs.operation==BLE_LIST_PAIR)atomic_store(&g_ble_list_pair_cancel,true);
      g_ble_list_jobs.cancel=true;pthread_mutex_unlock(&g_ble_list_lock);return 0;
    }
    pthread_mutex_unlock(&g_ble_list_lock);return -EBUSY;
  }
  int result=0;
  if(!g_ble_list_jobs.started){
    pthread_attr_t attr;pthread_t thread;result=pthread_attr_init(&attr);
    if(!result){
      size_t stack=16384;
#ifndef __NuttX__
      /* Native ASan and the dynamic loader need more than an embedded stack. */
      stack=131072;
#endif
      result=pthread_attr_setstacksize(&attr,stack);
      if(!result)result=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
      if(!result)result=pthread_create(&thread,&attr,glass_ble_list_worker,NULL);
      pthread_attr_destroy(&attr);
    }
    if(!result)g_ble_list_jobs.started=true;
  }
  if(!result){
    atomic_store(&g_ble_list_pair_cancel,false);
    g_ble_list_jobs.job=*job;g_ble_list_jobs.operation=job->operation;
    g_ble_list_jobs.busy=g_ble_list_jobs.pending=true;g_ble_list_jobs.cancel=false;g_ble_list_jobs.result=0;
    pthread_cond_signal(&g_ble_list_wake);
  }
  pthread_mutex_unlock(&g_ble_list_lock);return result?-result:0;
}
static const char *ble_list_error(int error)
{
  switch(error){
  case -ECANCELED:return glass_tr("操作已取消");
  case -ESTALE:return glass_tr("设备列表已更新，请重新选择设备");
  case -EBUSY:return glass_tr("蓝牙正在被其他操作使用，请稍后重试");
  case -ENOSPC:return glass_tr("蓝牙连接名额已满，请先断开一个连接");
  case -ETIMEDOUT:return glass_tr("操作超时，请靠近设备并重试");
  case -ENOMEM:return glass_tr("内存不足，请稍后重试");
  case -ENOTCONN:return glass_tr("设备已断开，可重新搜索连接");
  case -EACCES:return glass_tr("配对未完成，请确认对方允许配对后重试");
  case -ENOTSUP:return glass_tr("设备不支持此配对方式");
  default:return glass_tr("蓝牙操作失败，请检查设备后重试");
  }
}
static void ble_list_text(lv_obj_t *label,const char *text)
{if(strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);}
static void ble_list_address(const struct glass_ble_device *peer,char *text,size_t size)
{
  snprintf(text,size,"%02X:%02X:%02X:%02X:%02X:%02X · %s",peer->address[5],peer->address[4],
    peer->address[3],peer->address[2],peer->address[1],peer->address[0],peer->type&1?glass_tr("随机地址"):glass_tr("公共地址"));
}
static const char *ble_list_type(const struct glass_ble_device *peer)
{
  /* Appearance is more specific than a service. Both are untrusted broadcast
   * declarations; never derive a manufacturer/model from a random address. */
  if(peer->appearance==0x03c1)return "广播：键盘";
  if(peer->appearance==0x03c2)return glass_tr("广播：鼠标");
  switch(peer->appearance>>6){
  case 1:return glass_tr("广播：手机");case 2:return glass_tr("广播：电脑");
  case 3:return glass_tr("广播：手表");case 6:return glass_tr("广播：遥控器");
  case 12:return glass_tr("广播：温度计");case 13:return glass_tr("广播：心率设备");
  case 15:return glass_tr("广播：输入设备");
  }
  switch(peer->service_hint){
  case 0x1812:return glass_tr("HID 输入服务");case 0x180d:return glass_tr("心率服务");
  case 0x1809:return glass_tr("体温服务");case 0x181a:return glass_tr("环境传感服务");
  case 0x1810:return glass_tr("血压服务");case 0x1816:case 0x1818:return glass_tr("骑行服务");
  case 0x1826:return glass_tr("健身设备服务");
  }
  return peer->service128?glass_tr("其他 BLE 服务"):glass_tr("类型未知");
}
static const struct glass_ble_session *ble_list_peer_session(const struct glass_ble_state *state,
                                                              const struct glass_ble_device *peer)
{
  for(unsigned i=0;i<state->session_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(ble_list_same_peer(peer,&state->sessions[i].peer))return &state->sessions[i];
  return NULL;
}
static unsigned ble_list_connections(const struct glass_ble_state *state)
{return state->session_count?state->connection_count:(unsigned)(state->connected&&state->peer_valid);}
static bool ble_list_connecting(const struct glass_ble_state *state)
{
  if(state->connecting)return true;
  for(unsigned i=0;i<state->session_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(state->sessions[i].connecting)return true;
  return false;
}
static unsigned ble_list_priority(const struct ble_list_ui *view,const struct glass_ble_device *peer)
{return ((ble_list_peer_session(&view->state,peer)||
          (view->state.peer_valid&&ble_list_same_peer(peer,&view->state.peer)))?16:0)+
        (peer->bonded?8:0)+(peer->name[0]?4:0)+(peer->connectable_known&&peer->connectable?2:0);}
static bool ble_list_folded(const struct ble_list_ui *view,const struct glass_ble_device *peer)
{return !peer->name[0]&&!peer->bonded&&!ble_list_peer_session(&view->state,peer)&&
        !(view->state.peer_valid&&ble_list_same_peer(peer,&view->state.peer));}
static void glass_ble_list_toggle(lv_event_t *event)
{
  LV_UNUSED(event);struct ble_list_ui *view=g_ble_list;if(!view)return;
  view->expanded=!view->expanded;view->valid=false;view->pressed_row=NULL;
  glass_ble_list_refresh(NULL);
}
static void glass_ble_list_action(lv_event_t *event)
{
  struct ble_list_ui *view=g_ble_list;if(!view)return;
  unsigned operation=(uintptr_t)lv_event_get_user_data(event);
  if(lv_obj_has_state(lv_event_get_target(event),LV_STATE_DISABLED))return;
  if(!operation)operation=view->action;
  if(operation==BLE_LIST_SCAN&&view->state.scanning)operation=BLE_LIST_SCAN_STOP;
  else if(operation==BLE_LIST_SCAN&&view->state.connected&&!view->state.encrypted)operation=BLE_LIST_PAIR;
  if(!operation)return;
  struct ble_list_job job={.operation=operation,.generation=view->state.generation,
    .scan_generation=view->state.scan_generation,.peer_valid=view->state.peer_valid,.peer=view->state.peer};
  int result=glass_ble_list_submit(&job);if(result)show_toast(ble_list_error(result));
  glass_ble_list_refresh(NULL);
}
static void glass_ble_list_connect(lv_event_t *event)
{
  struct ble_list_ui *view=g_ble_list;if(!view)return;
  struct ble_list_row *row=lv_event_get_user_data(event);
  if(lv_obj_has_state(row->root,LV_STATE_DISABLED))return;
  if(lv_event_get_code(event)==LV_EVENT_PRESSED){view->pressed=row->binding;view->pressed_row=row->root;return;}
  if(lv_event_get_code(event)!=LV_EVENT_CLICKED)return;
  if(view->pressed_row==row->root&&
     (view->pressed.scan_generation!=row->binding.scan_generation||
      !ble_list_same_peer(&view->pressed.peer,&row->binding.peer))){
    view->pressed_row=NULL;show_toast(ble_list_error(-ESTALE));return;
  }
  view->pressed_row=NULL;
  int result=glass_ble_list_submit(&row->binding);if(result)show_toast(ble_list_error(result));
  glass_ble_list_refresh(NULL);
}
static void glass_ble_list_refresh(lv_timer_t *timer)
{
  LV_UNUSED(timer);struct ble_list_ui *view=g_ble_list;
  if(!view||!glass_ui_can_paint(view->root))return;
  memset(&view->state,0,sizeof(view->state));
  glass_ble_read(&view->state);glass_ble_hid_read(&view->hid);
  /* Only a confirmed central peer belongs in this disconnect action. */
  view->state.connected=view->state.connected&&view->state.peer_valid;
  if(view->state.session_count>GLASS_BLE_CENTRAL_LIMIT)view->state.session_count=GLASS_BLE_CENTRAL_LIMIT;
  if(view->state.count>GLASS_BLE_LIMIT)view->state.count=GLASS_BLE_LIMIT;
  for(unsigned i=0;i<view->state.count;i++)view->state.devices[i].name[GLASS_BLE_NAME_MAX]=0;
  view->state.peer.name[GLASS_BLE_NAME_MAX]=0;
  bool hid=view->hid.enabled||view->hid.advertising||view->hid.connected;
  pthread_mutex_lock(&g_ble_list_lock);
  bool busy=g_ble_list_jobs.busy;unsigned operation=g_ble_list_jobs.operation;int result=g_ble_list_jobs.result;
  pthread_mutex_unlock(&g_ble_list_lock);
  if(view->valid&&view->previous.scan_generation!=view->state.scan_generation)
    lv_obj_scroll_to_y(view->list,0,LV_ANIM_OFF);
  if(view->valid&&!memcmp(&view->previous,&view->state,sizeof(view->state))&&
     view->previous_busy==busy&&view->previous_operation==operation&&view->previous_result==result&&view->previous_hid==hid)return;
  view->valid=true;view->previous=view->state;view->previous_busy=busy;
  view->previous_operation=operation;view->previous_result=result;view->previous_hid=hid;
  struct glass_ble_state *state=&view->state;
  bool pairing=busy&&operation==BLE_LIST_PAIR;
  bool pair_failed=!busy&&operation==BLE_LIST_PAIR&&result;
  const char *status=state->restoring?glass_tr("正在逐个恢复普通设备"):state->mouse_paused?glass_tr("普通设备已暂停，等待鼠标退出"):
    state->disconnecting?glass_tr("正在断开连接"):state->store_error?glass_tr("配对保存失败"):pairing?glass_tr("正在配对设备"):
    pair_failed?(result==-ECANCELED?glass_tr("配对已取消"):glass_tr("配对未完成")):
    state->connected?(state->encrypted?(state->bonded?glass_tr("配对成功"):glass_tr("安全连接成功")):glass_tr("连接成功")):
    state->connecting?glass_tr("正在连接设备"):
    busy?(operation==BLE_LIST_SCAN?glass_tr("正在准备搜索"):operation==BLE_LIST_CONNECT?glass_tr("正在准备连接"):glass_tr("正在处理操作")):
    state->scanning?glass_tr("正在搜索附近设备"):hid?glass_tr("触摸板已启用，尚未连接设备"):
    !result&&operation==BLE_LIST_DISCONNECT?glass_tr("蓝牙已断开"):
    !result&&operation==BLE_LIST_CANCEL?glass_tr("操作已取消"):
    state->scan_generation&&!state->count?glass_tr("搜索结束，未发现设备"):glass_tr("尚未连接设备");
  if(state->peer_valid&&(state->connected||state->connecting)){
    char title[144];snprintf(title,sizeof(title),"%s · %s",status,state->peer.name[0]?state->peer.name:glass_tr("未命名设备"));
    ble_list_text(view->status,title);
  }else ble_list_text(view->status,status);
  char detail[180];
  if(state->store_error)snprintf(detail,sizeof(detail),glass_tr("配对信息保存失败（%d），请检查存储并重启后重试"),state->store_error);
  else if(!busy&&(result||state->error))snprintf(detail,sizeof(detail),"%s",ble_list_error(result?result:state->error));
  else if(state->mouse_paused)snprintf(detail,sizeof(detail),glass_tr("鼠标使用期间暂停普通 BLE，退出后自动重连；可取消设备恢复"));
  else if(pairing)snprintf(detail,sizeof(detail),glass_tr("请在对方设备确认配对，完成后自动更新状态"));
  else if(state->peer_valid&&(state->connected||state->connecting)){
    char address[80];ble_list_address(&state->peer,address,sizeof(address));
    snprintf(detail,sizeof(detail),"%s%s",address,state->connected?(state->encrypted&&state->bonded?glass_tr(" · 已配对"):
      state->encrypted?glass_tr(" · 已加密，未保存配对"):glass_tr(" · 未配对，可点击“配对设备”")):"");
  }else snprintf(detail,sizeof(detail),"%s",hid?glass_tr("设备列表只显示外部设备，触摸板状态单独显示"):
    state->scanning?glass_tr("点选设备即可停止搜索并连接"):glass_tr("搜索附近的低功耗蓝牙设备，设备需处于可连接状态"));
  ble_list_text(view->detail,detail);
  bool can_scan=!hid&&!state->mouse_paused&&!busy&&!ble_list_connecting(state)&&!state->disconnecting&&(!state->connected||state->encrypted);
  bool can_pair=!hid&&!state->mouse_paused&&!busy&&state->connected&&state->peer_valid&&!state->encrypted&&!state->disconnecting;
  glass_mouse_button_enabled(view->scan,can_scan||can_pair);
  ble_list_text(lv_obj_get_child(view->scan,0),pairing?glass_tr("配对中…"):state->scanning?glass_tr("停止搜索"):
    state->connected&&!state->encrypted?glass_tr("配对设备"):glass_tr("搜索设备"));
  view->action=state->mouse_paused&&state->peer_valid?BLE_LIST_DISCONNECT:pairing?BLE_LIST_CANCEL:state->connected?BLE_LIST_DISCONNECT:state->connecting||busy?BLE_LIST_CANCEL:state->scanning?BLE_LIST_SCAN_STOP:0;
  const char *control=state->mouse_paused&&state->peer_valid?glass_tr("取消恢复"):pairing?glass_tr("取消配对"):view->action==BLE_LIST_DISCONNECT?glass_tr("断开连接"):state->connecting?glass_tr("取消连接"):
    state->scanning?glass_tr("停止搜索"):busy?glass_tr("取消操作"):glass_tr("暂无连接");
  ble_list_text(lv_obj_get_child(view->control,0),control);
  glass_mouse_button_enabled(view->control,view->action&&!state->disconnecting&&(!busy||operation==BLE_LIST_SCAN||operation==BLE_LIST_CONNECT||pairing));
  unsigned folded=0;
  for(unsigned i=0;i<state->count;i++)if(ble_list_folded(view,&state->devices[i]))folded++;
  char count[160];snprintf(count,sizeof(count),glass_tr("普通连接 %u/%u · 附近设备 %u 条%s · 未命名 %u 条%s"),
    ble_list_connections(state),state->central_capacity?state->central_capacity:1,state->count,
    state->scan_truncated?glass_tr("（已达保留上限）"):"",folded,state->scanning?glass_tr(" · 搜索中"):"");
  ble_list_text(view->count,count);
  if(state->count)lv_obj_add_flag(view->empty,LV_OBJ_FLAG_HIDDEN);
  else{
    lv_obj_remove_flag(view->empty,LV_OBJ_FLAG_HIDDEN);
    ble_list_text(view->empty,state->scanning?glass_tr("正在寻找设备…"):
      busy?glass_tr("正在准备蓝牙，请稍候"):state->scan_generation?
      glass_tr("未发现设备\n请确认设备已开启广播，再重新搜索"):glass_tr("还没有发现设备\n点击“搜索设备”开始查找"));
  }
  /* Stable within each priority class: fluctuating RSSI never moves a row.
   * The original index/address/generation remain bound through sorting. */
  unsigned order[GLASS_BLE_LIMIT],used=0,main_count=0;
  for(unsigned group=0;group<2;group++){
    unsigned begin=used;
    if(group&&!view->expanded)break;
    for(unsigned i=0;i<state->count;i++)if(ble_list_folded(view,&state->devices[i])==(group!=0)){
      unsigned at=used++;
      while(at>begin&&ble_list_priority(view,&state->devices[i])>
            ble_list_priority(view,&state->devices[order[at-1]])){order[at]=order[at-1];at--;}
      order[at]=i;
    }
    if(!group)main_count=used;
  }
  if(folded){
    lv_obj_remove_flag(view->broadcasts,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(view->broadcasts,main_count*80);
    char label[96];snprintf(label,sizeof(label),glass_tr("未命名广播（%u） · %s"),folded,view->expanded?glass_tr("收起"):glass_tr("展开"));
    ble_list_text(lv_obj_get_child(view->broadcasts,0),label);
  }else lv_obj_add_flag(view->broadcasts,LV_OBJ_FLAG_HIDDEN);
  for(unsigned slot=0;slot<GLASS_BLE_LIMIT;slot++){
    struct ble_list_row *row=&view->rows[slot];
    if(slot>=used){lv_obj_add_flag(row->root,LV_OBJ_FLAG_HIDDEN);continue;}
    unsigned index=order[slot];const struct glass_ble_device *peer=&state->devices[index];
    row->binding=(struct ble_list_job){.operation=BLE_LIST_CONNECT,.index=index,
      .scan_generation=state->scan_generation,.generation=state->generation,.peer_valid=true,.peer=*peer};
    const struct glass_ble_session *session=ble_list_peer_session(state,peer);
    bool active=session||(state->peer_valid&&ble_list_same_peer(peer,&state->peer));
    if(session){row->binding.operation=BLE_LIST_SELECT;row->binding.generation=session->generation;}
    lv_obj_remove_flag(row->root,LV_OBJ_FLAG_HIDDEN);lv_obj_set_y(row->root,slot*80+(slot>=main_count?56:0));
    ble_list_text(row->name,peer->name[0]?peer->name:glass_tr("未命名设备"));
    char address[80],hint[192];ble_list_address(peer,address,sizeof(address));
    snprintf(hint,sizeof(hint),"%s · %s · %s",ble_list_type(peer),
      peer->bonded?glass_tr("已配对"):peer->connectable_known?(peer->connectable?glass_tr("可连接"):glass_tr("仅广播")):glass_tr("连接能力未知"),address);
    ble_list_text(row->address,hint);
    bool broadcast_only=peer->connectable_known&&!peer->connectable;
    char signal[48];
    if(peer->rssi<-127||peer->rssi>20)snprintf(signal,sizeof(signal),glass_tr("信号未知"));
    else snprintf(signal,sizeof(signal),"%d dBm · %s",peer->rssi,peer->rssi>=-55?glass_tr("强"):peer->rssi>=-75?glass_tr("良好"):glass_tr("弱"));
    ble_list_text(row->signal,signal);
    ble_list_text(row->action,session?(session->mouse_paused?glass_tr("等待恢复"):session->disconnecting?glass_tr("断开中…"):session->connecting?glass_tr("连接中…"):
      session->generation==state->generation?glass_tr("当前设备"):glass_tr("管理连接")):active?(state->disconnecting?glass_tr("断开中…"):pairing?glass_tr("配对中…"):
      state->connected?(state->encrypted&&state->bonded?glass_tr("已配对"):glass_tr("已连接")):glass_tr("连接中…")):
      broadcast_only?glass_tr("仅广播"):peer->bonded?glass_tr("重新连接"):glass_tr("连接"));
    lv_obj_set_width(row->action,active?116:96);
    if(active||broadcast_only)lv_obj_add_flag(row->arrow,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(row->arrow,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(row->action,lv_color_hex(active?0x4f9b77:detail_accent()),0);
    unsigned capacity=state->central_capacity?state->central_capacity:1;
    bool available=!hid&&!state->mouse_paused&&state->session_count<capacity&&!ble_list_connecting(state);
    glass_mouse_button_enabled(row->root,!busy&&(session?!session->disconnecting:
      !active&&!broadcast_only&&available&&!state->disconnecting));
    lv_obj_set_style_opa(row->root,LV_OPA_COVER,0);
  }
}
static void glass_ble_list_deleted(lv_event_t *event)
{
  struct ble_list_ui *view=lv_event_get_user_data(event);
  if(view->timer)lv_timer_delete(view->timer);
  if(g_ble_list==view)g_ble_list=NULL;
  free(view);
}
static void glass_ble_devices_page(lv_obj_t *card)
{
  struct ble_list_ui *view=calloc(1,sizeof(*view));if(!view){show_toast(glass_tr("内存不足"));return;}
  g_ble_list=view;view->root=card;
  lv_obj_add_event_cb(card,glass_ble_list_deleted,LV_EVENT_DELETE,view);
  pthread_mutex_lock(&g_ble_list_lock);if(!g_ble_list_jobs.busy)g_ble_list_jobs.result=0;
  pthread_mutex_unlock(&g_ble_list_lock);
  detail_page(card);glass_settings_sidebar(card,SETTINGS_BLUETOOTH);
  detail_text(card,glass_tr("蓝牙设备"),216,26,500,28,detail_ink());
  detail_text(card,glass_tr("优先显示已配对和有名称的设备，类型根据广播声明提示"),216,66,760,14,detail_muted());
  lv_obj_t *summary=detail_tile(card,216,104,784,112);
  home_icon(summary,LV_SYMBOL_BLUETOOTH,20,24,24,detail_accent());
  view->status=detail_text(summary,glass_tr("尚未连接设备"),60,22,414,20,detail_ink());
  lv_label_set_long_mode(view->status,LV_LABEL_LONG_DOT);
  view->detail=detail_text(summary,"",20,74,744,14,detail_muted());lv_label_set_long_mode(view->detail,LV_LABEL_LONG_DOT);
  view->scan=detail_button(summary,glass_tr("搜索设备"),496,16,124,glass_ble_list_action,(void *)(uintptr_t)BLE_LIST_SCAN);
  view->control=detail_button(summary,glass_tr("暂无连接"),632,16,132,glass_ble_list_action,NULL);
  view->count=detail_text(card,glass_tr("附近设备 · 0"),216,236,700,16,detail_muted());
  view->list=glass_box(card,216,272,784,278,detail_paper(),0);
  lv_obj_set_style_bg_opa(view->list,LV_OPA_TRANSP,0);
  lv_obj_add_flag(view->list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(view->list,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(view->list,LV_SCROLLBAR_MODE_AUTO);
  for(unsigned i=0;i<GLASS_BLE_LIMIT;i++){
    struct ble_list_row *row=&view->rows[i];
    row->root=glass_box(view->list,0,i*80,768,72,detail_paper(),14);
    lv_obj_add_flag(row->root,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(row->root,LV_OBJ_FLAG_SCROLLABLE);
    glass_input_guard_install(row->root);
    lv_obj_set_style_border_width(row->root,1,0);lv_obj_set_style_border_color(row->root,lv_color_hex(detail_line()),0);
    home_icon(row->root,LV_SYMBOL_BLUETOOTH,18,24,22,detail_accent());
    row->name=detail_text(row->root,"",60,12,366,18,detail_ink());lv_label_set_long_mode(row->name,LV_LABEL_LONG_DOT);
    row->address=detail_text(row->root,"",60,43,690,12,detail_muted());lv_label_set_long_mode(row->address,LV_LABEL_LONG_DOT);
    row->signal=detail_text(row->root,"",458,14,170,14,detail_muted());
    row->action=detail_text(row->root,glass_tr("连接"),632,27,96,14,detail_accent());
    row->arrow=home_icon(row->root,LV_SYMBOL_RIGHT,742,28,14,detail_accent());
    lv_obj_set_style_text_align(row->action,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_add_event_cb(row->root,glass_ble_list_connect,LV_EVENT_PRESSED,row);
    lv_obj_add_event_cb(row->root,glass_ble_list_connect,LV_EVENT_CLICKED,row);
    lv_obj_add_flag(row->root,LV_OBJ_FLAG_HIDDEN);
  }
  view->broadcasts=detail_button(view->list,glass_tr("未命名广播"),0,0,752,glass_ble_list_toggle,NULL);
  lv_obj_add_flag(view->broadcasts,LV_OBJ_FLAG_HIDDEN);
  view->empty=detail_text(view->list,"",24,80,720,20,detail_muted());
  lv_obj_set_style_text_align(view->empty,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_add_flag(view->empty,LV_OBJ_FLAG_IGNORE_LAYOUT);
  /* Initialize actions against laid-out geometry before the first pointer
   * event, including a direct return from the touchpad page. */
  lv_obj_update_layout(card);
  view->timer=lv_timer_create(glass_ble_list_refresh,200,NULL);glass_ble_list_refresh(NULL);
  if(!view->timer){
    ble_list_text(view->status,glass_tr("状态刷新启动失败，请重新打开蓝牙"));
    glass_mouse_button_enabled(view->scan,false);glass_mouse_button_enabled(view->control,false);
    for(unsigned i=0;i<GLASS_BLE_LIMIT;i++)glass_mouse_button_enabled(view->rows[i].root,false);
    return;
  }
  pthread_mutex_lock(&g_ble_list_lock);bool busy=g_ble_list_jobs.busy;
  pthread_mutex_unlock(&g_ble_list_lock);
  if(!busy&&!view->state.scanning&&!view->state.connecting&&!view->state.connected&&
     !view->state.disconnecting&&!view->state.mouse_paused&&
     !view->hid.enabled&&!view->hid.advertising&&!view->hid.connected){
    struct ble_list_job job={.operation=BLE_LIST_SCAN};
    int result=glass_ble_list_submit(&job);if(result)show_toast(ble_list_error(result));
    glass_ble_list_refresh(NULL);
  }
}
static void glass_ble_devices_open(lv_event_t *event)
{LV_UNUSED(event);lv_obj_t *card=panel_card(glass_tr("蓝牙设备"));if(card)glass_ble_devices_page(card);}
