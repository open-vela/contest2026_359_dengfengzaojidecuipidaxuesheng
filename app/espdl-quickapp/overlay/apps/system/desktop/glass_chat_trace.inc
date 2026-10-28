/* SPDX-License-Identifier: Apache-2.0 */
/* Invocation history, not inferred tool outcomes or model reasoning. */
static void chat_ui_trace_update(const struct chat_turn *turn)
{
  if(!g_chat_ui.trace_root)return;
  char title[128],meta[100],details[240];
  unsigned seconds=turn->state==CHAT_WAITING?(lv_tick_get()-g_chat_ui.began)/1000:
    (turn->activity_ms-turn->began_ms)/1000;
  snprintf(title,sizeof(title),glass_tr("%s · %u 秒"),turn->state==CHAT_DONE?glass_tr("已完成"):turn->reply[0]?glass_tr("正在输出"):glass_tr("正在思考"),seconds);
  if(g_chat_ui.snapshot->phase==CHAT_STOPPING)snprintf(title,sizeof(title),glass_tr("正在停止…"));
  snprintf(meta,sizeof(meta),"%s",turn->state==CHAT_DONE?glass_tr("操作记录"):turn->progress[0]?turn->progress:glass_tr("等待模型输出"));
  chat_ui_label(g_chat_ui.waiting,title);chat_ui_label(g_chat_ui.live_reasoning,meta);
  bool active=g_chat_ui.snapshot->phase==CHAT_REQUESTING||g_chat_ui.snapshot->phase==CHAT_QUEUED;
  unsigned wave=(lv_tick_get()/250)%8;unsigned opacity=wave<4?160+wave*24:160+(7-wave)*24;
  lv_obj_set_style_text_opa(g_chat_ui.waiting,active&&!g_glass.reduced_motion?opacity:LV_OPA_COVER,0);
  char button[48];snprintf(button,sizeof(button),glass_tr("%u 次调用 %s"),turn->tool_total,g_chat_ui.trace_expanded?"-":"›");
  chat_ui_label(lv_obj_get_child(g_chat_ui.trace_button,0),button);
  if(turn->tool_count)lv_obj_remove_state(g_chat_ui.trace_button,LV_STATE_DISABLED);
  else lv_obj_add_state(g_chat_ui.trace_button,LV_STATE_DISABLED);
  size_t used=0;details[0]=0;
  for(unsigned i=0;i<turn->tool_count&&i<CHAT_TOOL_TRACE;i++)
    used+=snprintf(details+used,sizeof(details)-used,"%s%u  %s",i?"\n":"",turn->tool_total-turn->tool_count+i+1,turn->tools[i]);
  chat_ui_label(g_chat_ui.trace_details,details);
  bool expanded=g_chat_ui.trace_expanded&&turn->tool_count;
  if(expanded)lv_obj_remove_flag(g_chat_ui.trace_details,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(g_chat_ui.trace_details,LV_OBJ_FLAG_HIDDEN);
  int height=expanded?72+turn->tool_count*24:64;
  lv_obj_set_height(g_chat_ui.trace_root,height);
  int y=g_chat_ui.trace_y+height+8;
  if(g_chat_ui.cards_root&&g_chat_ui.cards_h){lv_obj_set_y(g_chat_ui.cards_root,y);y+=g_chat_ui.cards_h+8;}
  g_chat_ui.live_y=y;
  if(g_chat_ui.live_bubble)lv_obj_set_y(g_chat_ui.live_bubble,g_chat_ui.live_y);
  g_chat_ui.ball_anchor=turn->reply[0]?g_chat_ui.live_bubble:g_chat_ui.trace_root;
}
static void chat_ui_trace_toggle(lv_event_t *event)
{
  LV_UNUSED(event);struct chat_snapshot *s=g_chat_ui.snapshot;
  if(!s||!s->count||!g_chat_ui.trace_root)return;
  bool follow=lv_obj_get_scroll_bottom(g_chat_ui.messages)<48;
  g_chat_ui.trace_expanded=!g_chat_ui.trace_expanded;
  chat_ui_trace_update(&s->turns[s->count-1]);lv_obj_update_layout(g_chat_ui.messages);
  if(follow)lv_obj_scroll_to_y(g_chat_ui.messages,LV_COORD_MAX,LV_ANIM_OFF);
  chat_ui_ball_position(NULL);
}
static void chat_ui_trace_create(const struct chat_turn *turn,int y)
{
  g_chat_ui.trace_y=y;
  uint32_t surface=g_chat_ui.background;
  g_chat_ui.trace_root=glass_box(g_chat_ui.messages,96,y,700,64,surface,10);
  lv_obj_remove_flag(g_chat_ui.trace_root,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(g_chat_ui.trace_root,LV_OPA_TRANSP,0);
  g_chat_ui.waiting=chat_ui_text(g_chat_ui.trace_root,"",18,4,508,16,g_chat_ui.secondary,false);
  g_chat_ui.live_reasoning=chat_ui_text(g_chat_ui.trace_root,"",18,30,474,20,g_chat_ui.primary,false);
  g_chat_ui.trace_button=chat_ui_button(g_chat_ui.trace_root,glass_tr("0 次调用 ›"),514,26,164,34,false,chat_ui_trace_toggle,NULL);
  lv_obj_set_style_bg_opa(g_chat_ui.trace_button,LV_OPA_TRANSP,0);
  lv_obj_set_style_text_color(lv_obj_get_child(g_chat_ui.trace_button,0),lv_color_hex(g_chat_ui.secondary),0);
  g_chat_ui.trace_details=chat_ui_text(g_chat_ui.trace_root,"",18,68,656,16,g_chat_ui.secondary,true);
  lv_obj_set_style_text_font(g_chat_ui.trace_details,chat_ui_mono(),0);
  lv_obj_set_style_text_line_space(g_chat_ui.trace_details,0,0);
  chat_ui_trace_update(turn);
}
