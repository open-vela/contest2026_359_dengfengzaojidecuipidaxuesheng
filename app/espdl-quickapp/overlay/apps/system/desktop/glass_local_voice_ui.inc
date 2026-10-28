/* SPDX-License-Identifier: Apache-2.0 */
/* UI-thread controller. The existing HA service owns all network I/O.
 * Bindings are explicitly chosen for this page session and never inferred. */
enum {LOCAL_IDLE,LOCAL_LIST,LOCAL_PRECHECK,LOCAL_CONTROL,LOCAL_VERIFY};
static void local_voice_picker_close(lv_event_t *event);
static void local_voice_message(const char *text)
{if(g_speech_ui.feedback&&strcmp(lv_label_get_text(g_speech_ui.feedback),text))lv_label_set_text(g_speech_ui.feedback,text);}
static void local_voice_disconnect(void)
{
#ifdef CONFIG_SYSTEM_HASS
  if(g_speech_ui.client)hass_close(g_speech_ui.client);
#endif
  g_speech_ui.client=g_speech_ui.request=0;g_speech_ui.stage=LOCAL_IDLE;
}
static void local_voice_labels(void)
{for(unsigned i=0;i<3;i++)if(g_speech_ui.binding[i])lv_label_set_text(g_speech_ui.binding[i],g_speech_ui.bound[i].name[0]?g_speech_ui.bound[i].name:glass_tr("选择设备"));}
static bool local_voice_config(void)
{
#ifdef CONFIG_SYSTEM_HASS
  uint32_t revision=hass_configuration_revision();
  if(g_speech_ui.configuration&&revision!=g_speech_ui.configuration){
    local_voice_disconnect();local_voice_picker_close(NULL);memset(g_speech_ui.bound,0,sizeof(g_speech_ui.bound));local_voice_labels();
    g_speech_ui.configuration=revision;local_voice_message(glass_tr("家庭配置已改变，请重新绑定设备"));return false;
  }
  g_speech_ui.configuration=revision;
  if(!g_speech_ui.client){int client=hass_open(HASS_READ|HASS_CONTROL);if(client<=0){local_voice_message(glass_tr("家庭服务不可用，请稍后重试"));return false;}g_speech_ui.client=client;}
  return true;
#else
  local_voice_message(glass_tr("当前固件未启用家庭设备服务"));return false;
#endif
}
static bool local_voice_request(int request,int stage)
{
  if(request<=0){local_voice_message(request==-EBUSY?glass_tr("家庭服务正忙，请稍后重试"):request==-ENOTCONN?glass_tr("请先配置 Home Assistant"):glass_tr("请求未发送，请检查连接"));return false;}
  g_speech_ui.request=request;g_speech_ui.stage=stage;g_speech_ui.deadline=lv_tick_get()+12000;return true;
}
static void local_voice_picker_close(lv_event_t *event)
{
  (void)event;if(g_speech_ui.stage==LOCAL_LIST)local_voice_disconnect();
  if(g_speech_ui.picker)lv_obj_delete(g_speech_ui.picker);
  g_speech_ui.picker=g_speech_ui.page_label=NULL;memset(g_speech_ui.rows,0,sizeof(g_speech_ui.rows));
  free(g_speech_ui.choices);g_speech_ui.choices=NULL;g_speech_ui.choice_count=0;
}
static void local_voice_choose(lv_event_t *event)
{
  unsigned index=(uintptr_t)lv_event_get_user_data(event);
  bool clear=!index;
  if(!g_speech_ui.picker||!local_voice_config()||g_speech_ui.stage!=LOCAL_IDLE)return;
  if(index){index--;if(index>=g_speech_ui.choice_count)return;g_speech_ui.bound[g_speech_ui.role]=g_speech_ui.choices[index];}
  else memset(&g_speech_ui.bound[g_speech_ui.role],0,sizeof(g_speech_ui.bound[0]));
  local_voice_labels();local_voice_picker_close(NULL);local_voice_message(clear?glass_tr("已解除绑定"):glass_tr("本次会话绑定已更新"));
}
static void local_voice_picker_render(void)
{
  if(!g_speech_ui.picker)return;
  for(unsigned i=0;i<4;i++){if(g_speech_ui.rows[i])lv_obj_delete(g_speech_ui.rows[i]);g_speech_ui.rows[i]=NULL;}
  for(unsigned i=0;i<4;i++){
    unsigned index=g_speech_ui.page*4+i;if(index>=g_speech_ui.choice_count)break;
    struct local_voice_device *device=&g_speech_ui.choices[index];
    lv_obj_t *button=glass_button(g_speech_ui.picker,device->name,20,72+i*68,904,theme_card(),local_voice_choose,(void *)(uintptr_t)(index+1));
    g_speech_ui.rows[i]=button;
    lv_obj_t *label=lv_obj_get_child(button,0);lv_obj_set_width(label,850);lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
  }
  lv_label_set_text_fmt(g_speech_ui.page_label,"%u / %u%s",g_speech_ui.page+1,(g_speech_ui.choice_count+3)/4? (g_speech_ui.choice_count+3)/4:1,g_speech_ui.truncated?glass_tr(" · 仅前 32 台"):"");
}
static void local_voice_picker_step(lv_event_t *event)
{
  int page=(int)g_speech_ui.page+((uintptr_t)lv_event_get_user_data(event)?1:-1);
  if(page<0||(unsigned)page*4>=g_speech_ui.choice_count)return;
  g_speech_ui.page=page;local_voice_picker_render();
}
static void local_voice_picker_open(lv_event_t *event)
{
  struct speech_snapshot speech;glass_speech_get(&speech);
  if(speech.active||g_speech_ui.stage!=LOCAL_IDLE){show_toast(glass_tr("请先停止聆听或等待当前操作完成"));return;}
  if(g_speech_ui.picker||!local_voice_config())return;
  g_speech_ui.choices=calloc(32,sizeof(*g_speech_ui.choices));if(!g_speech_ui.choices){local_voice_message(glass_tr("内存不足，无法读取设备"));return;}
  g_speech_ui.role=(uintptr_t)lv_event_get_user_data(event);g_speech_ui.page=g_speech_ui.choice_count=0;g_speech_ui.truncated=g_speech_ui.sent=g_speech_ui.list_ok=false;
  const char *titles[]={glass_tr("选择灯光"),glass_tr("选择窗帘"),glass_tr("选择风扇")};
  lv_obj_t *panel=glass_box(g_speech_ui.root,12,64,952,472,theme_surface(),16);g_speech_ui.picker=panel;
  glass_text(panel,titles[g_speech_ui.role],20,18,640,24,theme_primary());
  glass_button(panel,glass_tr("返回"),812,12,120,theme_card(),local_voice_picker_close,NULL);
  g_speech_ui.page_label=glass_text(panel,glass_tr("正在读取设备…"),196,404,340,16,theme_secondary());
  glass_button(panel,glass_tr("解除绑定"),20,398,156,theme_card(),local_voice_choose,NULL);
  glass_button(panel,glass_tr("上一页"),578,398,156,theme_card(),local_voice_picker_step,NULL);
  glass_button(panel,glass_tr("下一页"),750,398,156,theme_card(),local_voice_picker_step,(void *)1);
#ifdef CONFIG_SYSTEM_HASS
  if(!local_voice_request(hass_get(g_speech_ui.client,"states"),LOCAL_LIST))lv_label_set_text(g_speech_ui.page_label,glass_tr("读取未开始，返回后重试"));
#endif
}
static void local_voice_execute(int command)
{
  if(command<1||command>6)return;
  if(g_speech_ui.stage!=LOCAL_IDLE){local_voice_message(glass_tr("上一条设备指令仍在处理中，请稍后重说"));return;}
  int role=(command-1)/2;
  if(!g_speech_ui.bound[role].entity[0]){local_voice_message(glass_tr("该类设备尚未绑定，请先停止聆听并选择设备"));return;}
  if(!local_voice_config())return;
  g_speech_ui.role=role;g_speech_ui.on=command%2;g_speech_ui.sent=false;
  strcpy(g_speech_ui.entity,g_speech_ui.bound[role].entity);
#ifdef CONFIG_SYSTEM_HASS
  if(local_voice_request(hass_get_state(g_speech_ui.client,g_speech_ui.entity),LOCAL_PRECHECK))local_voice_message(glass_tr("正在读取绑定设备状态…"));
#endif
}
static void local_voice_poll(void)
{
#ifdef CONFIG_SYSTEM_HASS
  if(!g_speech_ui.client||!local_voice_config())return;
  uint32_t now=lv_tick_get();
  if(g_speech_ui.stage!=LOCAL_IDLE&&(int32_t)(now-g_speech_ui.deadline)>=0){
    local_voice_message(g_speech_ui.sent?glass_tr("控制结果未确认，请查看设备；不会自动重发"):glass_tr("读取超时，未发送设备控制"));local_voice_disconnect();return;
  }
  if(g_speech_ui.stage==LOCAL_VERIFY&&!g_speech_ui.request&&(int32_t)(now-g_speech_ui.verify_at)>=0){
    if((int32_t)(now-g_speech_ui.verify_deadline)>=0){local_voice_message(glass_tr("控制结果未确认，请查看设备；不会自动重发"));g_speech_ui.stage=LOCAL_IDLE;return;}
    int request=hass_get_state(g_speech_ui.client,g_speech_ui.entity);
    if(request>0)g_speech_ui.request=request;else g_speech_ui.verify_at=now+500;
  }
  if(!g_speech_ui.request)return;
  struct hass_result_s result={0};int ret=hass_poll(g_speech_ui.client,&result);
  if(ret<0){local_voice_message(g_speech_ui.sent?glass_tr("控制结果未确认，请查看设备"):glass_tr("设备状态读取失败，未发送控制"));local_voice_disconnect();return;}
  if(!result.done){hass_result_free(&result);return;}
  if(result.request_id!=g_speech_ui.request){hass_result_free(&result);local_voice_message(glass_tr("设备响应已过期，请重试"));local_voice_disconnect();return;}
  int stage=g_speech_ui.stage;g_speech_ui.request=0;bool ok=!result.error&&result.status>=200&&result.status<300;
  cJSON *json=ok?local_voice_json(result.body):NULL;
  if(stage==LOCAL_LIST){
    if(cJSON_IsArray(json)){
      g_speech_ui.list_ok=true;
      const cJSON *item;cJSON_ArrayForEach(item,json)if(local_voice_allowed(item,g_speech_ui.role,NULL)){
        if(g_speech_ui.choice_count==32){g_speech_ui.truncated=true;continue;}
        struct local_voice_device *device=&g_speech_ui.choices[g_speech_ui.choice_count++];
        strcpy(device->entity,local_voice_string(item,"entity_id"));
        const cJSON *attr=cJSON_GetObjectItemCaseSensitive(item,"attributes");const char *name=local_voice_string(attr,"friendly_name");
        glass_chat_copy_utf8(device->name,sizeof(device->name),name[0]?name:device->entity);
      }
      local_voice_picker_render();if(!g_speech_ui.choice_count)lv_label_set_text(g_speech_ui.page_label,glass_tr("没有可用设备（窗帘须标明窗帘类型）"));
    }else if(g_speech_ui.page_label)lv_label_set_text(g_speech_ui.page_label,glass_tr("读取失败，请检查家庭服务配置"));
    g_speech_ui.stage=LOCAL_IDLE;
  }else if(stage==LOCAL_PRECHECK){
    g_speech_ui.stage=LOCAL_IDLE;
    if(!json||!local_voice_allowed(json,g_speech_ui.role,g_speech_ui.entity))local_voice_message(glass_tr("设备不可用或类型不匹配，未发送控制"));
    else if(local_voice_matches(json,g_speech_ui.role,g_speech_ui.on))local_voice_message(glass_tr("设备已经处于目标状态"));
    else{
      char body[128];snprintf(body,sizeof(body),"{\"entity_id\":\"%s\"}",g_speech_ui.entity);
      int request=g_speech_ui.role==1?hass_call_service_checked(g_speech_ui.client,g_speech_ui.entity,g_speech_ui.on?"open_cover":"close_cover",body,g_speech_ui.configuration):hass_control_checked(g_speech_ui.client,g_speech_ui.entity,g_speech_ui.on,-1,g_speech_ui.configuration);
      if(local_voice_request(request,LOCAL_CONTROL)){g_speech_ui.sent=true;local_voice_message(glass_tr("控制已发送，等待核对…"));}
    }
  }else if(stage==LOCAL_CONTROL){
    /* A lost POST response does not mean the device did nothing. Verify by
     * GET; never retry a physical action automatically. */
    g_speech_ui.stage=LOCAL_VERIFY;g_speech_ui.verify_at=now;g_speech_ui.verify_deadline=now+8000;g_speech_ui.deadline=now+10000;
    local_voice_message(glass_tr("正在核对设备实际状态…"));
  }else if(stage==LOCAL_VERIFY){
    if(json&&local_voice_allowed(json,g_speech_ui.role,g_speech_ui.entity)&&local_voice_matches(json,g_speech_ui.role,g_speech_ui.on)){
      g_speech_ui.stage=LOCAL_IDLE;local_voice_message(glass_tr("设备状态已确认"));
    }else g_speech_ui.verify_at=now+1000;
  }
  cJSON_Delete(json);hass_result_free(&result);
#endif
}
