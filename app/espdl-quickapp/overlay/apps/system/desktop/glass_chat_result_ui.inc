/* SPDX-License-Identifier: Apache-2.0 */
/* Result cards consume typed native receipts; no actions or touch targets. */
static const char *chat_result_title(const struct chat_result_snapshot *r)
{
  if(r->state==CHAT_WAITING)return r->progress[0]?r->progress:"正在处理";
  if(r->state==CHAT_FAILED)return r->card_count?glass_tr("回复未完成"):glass_tr("任务未完成");
  if(r->state==CHAT_CANCELLED)return glass_tr("已停止");
  bool action=false;
  for(unsigned i=0;i<r->card_count;i++)if(r->cards[i].action){
    action=true;if(!r->cards[i].verified)return glass_tr("结果待确认");
  }
  return action?glass_tr("执行已确认"):r->card_count?glass_tr("设备结果"):glass_tr("AI 回复");
}
static size_t chat_result_voice(const struct chat_result_snapshot *r,char *out,size_t size)
{
  if(!size)return 0;out[0]=0;
  if(r->state==CHAT_FAILED||r->state==CHAT_CANCELLED){
    return glass_chat_copy_utf8(out,size,r->state==CHAT_CANCELLED?glass_tr("本次任务已停止。"):glass_chat_error_text(r->error));
  }
  /* Describe receipts even if prose claims more. Unverified controls never
   * receive a generic success announcement. Ordinary answers use AI text. */
  if(r->card_count){
    char line[256];size_t used=0;
    for(unsigned i=0;i<r->card_count;i++){
      const struct chat_card *c=&r->cards[i];
      if(c->action&&!c->verified)snprintf(line,sizeof(line),glass_tr("%s，结果还没有确认。"),c->name);
      else if(c->level>=0)snprintf(line,sizeof(line),glass_tr("%s，%s，百分之%d。"),c->name,c->state,c->level);
      else snprintf(line,sizeof(line),"%s，%s。",c->name,c->state);
      size_t n=glass_chat_copy_utf8(out+used,size-used,line);used+=n;
      if(n<strlen(line))break;
    }
    return used;
  }
  return glass_chat_copy_utf8(out,size,r->reply);
}
static lv_obj_t *chat_result_label(lv_obj_t *p,const char *text,int x,int y,int w,int h,int font,uint32_t color)
{
  lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_text_font(o,zh_font(font),0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
  lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,text);return o;
}
static uint32_t chat_result_signature(const struct chat_result_snapshot *r)
{
  /* Exclude transport revision and speech offsets from visual changes. */
  uint32_t h=2166136261u;
  const char *parts[]={chat_result_title(r),r->reply};
  for(unsigned i=0;i<2;i++)for(const unsigned char *p=(const unsigned char *)parts[i];*p;p++)h=(h^*p)*16777619u;
  h=(h^r->turn_id)*16777619u;
  for(unsigned i=0;i<r->card_count;i++){
    const struct chat_card *c=&r->cards[i];const char *fields[]={c->entity,c->kind,c->name,c->state};
    for(unsigned n=0;n<4;n++)for(const unsigned char *p=(const unsigned char *)fields[n];*p;p++)h=(h^*p)*16777619u;
    h=(h^(uint32_t)c->level)*16777619u;h=(h^c->on)*16777619u;
    h=(h^c->action)*16777619u;h=(h^c->verified)*16777619u;
  }
  return h?h:1;
}
static lv_obj_t *chat_result_build(lv_obj_t *parent,int x,int y,int width,const struct chat_result_snapshot *r)
{
  bool light=g_desktop.light_theme;
  uint32_t ink=light?0x202b3d:0xf1f5fb,muted=light?0x526985:0xaebed4;
  int height=118+(int)r->card_count*50;
  lv_obj_t *root=lv_obj_create(parent);lv_obj_set_pos(root,x,y);lv_obj_set_size(root,width,height);
  lv_obj_set_style_bg_color(root,lv_color_hex(light?0xffffff:0x242b38),0);
  lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);lv_obj_set_style_radius(root,18,0);
  lv_obj_set_style_border_width(root,1,0);lv_obj_set_style_border_color(root,lv_color_hex(light?0xdce5f0:0x43516a),0);
  lv_obj_set_style_pad_all(root,12,0);lv_obj_remove_flag(root,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  uint32_t accent=r->state==CHAT_FAILED?0xdf675d:!strcmp(chat_result_title(r),"结果待确认")?0xdba34b:0x5bb59a;
  chat_result_label(root,chat_result_title(r),0,0,width-26,24,16,accent);
  const char *summary=r->state==CHAT_FAILED?glass_chat_error_text(r->error):r->state==CHAT_WAITING?glass_tr("正在执行你的请求…"):r->card_count?glass_tr("设备状态如下。"):r->reply[0]?r->reply:glass_tr("结果已更新");
  chat_result_label(root,summary,0,30,width-26,60,16,ink);
  for(unsigned i=0;i<r->card_count;i++){
    const struct chat_card *c=&r->cards[i];int row=96+(int)i*50;char line[112];
    chat_result_label(root,c->name,0,row,width-26,20,14,ink);
    if(c->action&&!c->verified)snprintf(line,sizeof(line),glass_tr("结果待确认"));
    else if(c->level>=0)snprintf(line,sizeof(line),"%s · %d%%",c->state,c->level);
    else snprintf(line,sizeof(line),"%s",c->state);
    chat_result_label(root,line,0,row+20,width-26,20,14,muted);
  }
  return root;
}
