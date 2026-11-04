/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_app_store.h"
#include "glass_ui_visibility.h"
#include <ctype.h>
static struct {
  lv_obj_t *root,*body,*rail,*view,*modal,*query_input,*keyboard;
  lv_timer_t *timer;
  struct glass_store_snapshot *snapshot;
  unsigned painted,section,category,page;
  char query[100],selected[48];
  bool return_apps,cloud_input;
} g_store_ui;
static void glass_store_app(lv_event_t *e);
static void store_ui_draw(void);
static void store_ui_back(lv_event_t *e);
static const char *store_ui_categories[]={"实用工具","专注效率","轻松趣味"};
static const char *store_ui_sections[]={"精选","分类","SD 仓库","已安装","云端商城"};
static unsigned store_ui_kind(const struct glass_store_entry *app)
{return app->icon<4?app->icon:3;}
static uint32_t store_ui_color(const struct glass_store_entry *app)
{static const uint32_t colors[]={0x548cf2,0x46aa91,0xe49a50,0x9680cf};return colors[store_ui_kind(app)];}
static lv_obj_t *store_ui_art(lv_obj_t *parent,const struct glass_store_entry *app,int x,int y,int size)
{
  uint32_t color=store_ui_color(app);lv_obj_t *icon=glass_box(parent,x,y,size,size,color,size/4);
  lv_obj_set_style_bg_grad_color(icon,lv_color_mix(lv_color_hex(color),lv_color_hex(0xffffff),210),0);
  lv_obj_set_style_bg_grad_dir(icon,LV_GRAD_DIR_VER,0);
  unsigned kind=store_ui_kind(app);
  if(kind==1){
    lv_obj_t *ring=glass_box(icon,10,10,size-20,size-20,color,LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(ring,2,0);lv_obj_set_style_border_color(ring,lv_color_white(),0);
    lv_obj_t *hand=glass_box(icon,size/2-1,15,2,size/2-13,0xffffff,1);(void)hand;
    glass_box(icon,size/2-1,size/2-1,size/5,2,0xffffff,1);
  }else if(kind==2){
    for(int b=0;b<3;b++)glass_box(icon,size/4+b*(size/5),size/2+6-b*5,4,size/4-4+b*5,0xffffff,2);
  }else{
    for(unsigned b=0;b<4;b++){
      lv_obj_t *dot=glass_box(icon,size/4+(b%2)*(size/3),size/4+(b/2)*(size/3),size/6,size/6,0xffffff,kind?size/12:2);(void)dot;
    }
  }
  /* Artwork is decorative; every pixel of the icon belongs to its card. */
  lv_obj_remove_flag(icon,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  for(unsigned n=0;n<lv_obj_get_child_count(icon);n++)lv_obj_remove_flag(lv_obj_get_child(icon,n),LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  return icon;
}
static lv_obj_t *store_ui_pill(lv_obj_t *parent,const char *text,int x,int y,int width,lv_event_cb_t cb,uintptr_t data)
{
  lv_obj_t *button=glass_button(parent,text,x,y,width,detail_soft(),cb,(void *)data);
  lv_obj_set_height(button,36);lv_obj_set_style_radius(button,18,0);
  lv_obj_set_style_text_color(lv_obj_get_child(button,0),lv_color_hex(detail_accent()),0);
  lv_obj_set_style_text_font(lv_obj_get_child(button,0),home_font(16),0);
  return button;
}
static const char *store_ui_error(int code)
{
  switch(code){
    case 0:return "操作已完成";
    case -ECANCELED:return "操作已取消";
    case -EBUSY:return "正在处理，请稍后再试";
    case -ENOSPC:return "存储空间不足，未更换安装位置";
    case -ENOENT:case -ENODEV:return "存储设备或应用文件不可用";
    case -EBADMSG:return "文件校验不通过，请检查仓库内容";
    case -ENOEXEC:return "应用脚本语法检查未通过";
    case -EEXIST:return "应用或暂存目录已存在，原文件保留";
    case -EPERM:case -ENOTEMPTY:return "应用包含改动，已保留原文件";
    case -ENOMEM:return "内存不足，请关闭其他任务后重试";
    case -EINVAL:return "地址无效：使用 HTTPS，局域网可使用 HTTP IP 地址";
    case -E2BIG:return "应用目录已满，请减少当前来源的应用数量";
    case -ENETDOWN:case -EHOSTUNREACH:case -ECONNREFUSED:return "云端暂时无法连接，本地应用仍可使用";
    case -ETIMEDOUT:return "连接超时，请检查网络或服务地址";
    case -EACCES:return "无法访问，请检查权限或服务器证书";
    case -EREMOTEIO:return "云端服务返回错误，请稍后刷新";
    case -EAGAIN:return "服务繁忙，请稍后刷新";
    case -EFBIG:return "文件超出设备大小限制";
    default:return "操作未完成，请检查存储设备后重试";
  }
}
static lv_obj_t *store_ui_button(lv_obj_t *parent,const char *text,int x,int y,int w,bool primary,lv_event_cb_t cb,uintptr_t value)
{
  lv_obj_t *b=glass_button(parent,text,x,y,w,primary?detail_accent():detail_soft(),cb,(void *)value);
  lv_obj_set_height(b,44);glass_input_guard_install(b);
  if(primary)lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(glass_app_palette(g_desktop.light_theme).on_accent),0);
  return b;
}
static int store_ui_selected(void)
{
  for(unsigned i=0;i<g_store_ui.snapshot->count;i++)if(!strcmp(g_store_ui.selected,g_store_ui.snapshot->entries[i].package))return (int)i;
  return -1;
}
static void store_ui_dialog_close(lv_event_t *e)
{
  (void)e;lv_obj_t *modal=g_store_ui.modal;g_store_ui.modal=g_store_ui.query_input=g_store_ui.keyboard=NULL;
  if(modal)lv_obj_delete(modal);
  if(g_store_ui.root)store_ui_draw();
}
static void store_ui_search_apply(lv_event_t *e)
{
  (void)e;glass_ime_commit(g_store_ui.keyboard);
  if(g_store_ui.cloud_input){
    int ret=glass_store_configure(lv_textarea_get_text(g_store_ui.query_input));
    if(ret){show_toast(store_ui_error(ret));return;}
    store_ui_dialog_close(NULL);return;
  }
  strlcpy(g_store_ui.query,lv_textarea_get_text(g_store_ui.query_input),sizeof(g_store_ui.query));
  g_store_ui.selected[0]=0;g_store_ui.page=0;store_ui_dialog_close(NULL);
}
static void store_ui_prompt(bool cloud)
{
  if(g_store_ui.modal)return;
  g_store_ui.cloud_input=cloud;
  lv_obj_t *shade=glass_box(g_store_ui.root,0,0,1024,568,detail_canvas(),0);g_store_ui.modal=shade;
  lv_obj_add_flag(shade,LV_OBJ_FLAG_CLICKABLE);
  detail_text(shade,cloud?"云端商店地址":"搜索应用",192,24,640,28,detail_ink());
  lv_obj_t *input=lv_textarea_create(shade);g_store_ui.query_input=input;
  lv_obj_set_pos(input,192,76);lv_obj_set_size(input,640,52);glass_input_style(input);
  lv_textarea_set_one_line(input,true);lv_textarea_set_max_length(input,cloud?GLASS_STORE_URL_CAP-1:24);lv_textarea_set_text(input,cloud?g_store_ui.snapshot->cloud_url:g_store_ui.query);
  lv_textarea_set_placeholder_text(input,cloud?"https://store.example.com（留空关闭云端来源）":"应用名称、简介或包名");
  store_ui_button(shade,"取消",192,146,312,false,store_ui_dialog_close,0);
  store_ui_button(shade,cloud?"保存地址":"搜索",520,146,312,true,store_ui_search_apply,0);
  lv_obj_t *keyboard=lv_keyboard_create(shade);g_store_ui.keyboard=keyboard;
  lv_obj_set_pos(keyboard,164,210);lv_obj_set_size(keyboard,860,358);
  glass_ime_attach(keyboard,zh_font(20),!cloud,detail_ink(),detail_soft(),detail_accent());
  glass_ime_set_voice_enabled(keyboard,false);glass_ime_set_target(keyboard,input);
}
static void store_ui_search(lv_event_t *e){(void)e;store_ui_prompt(false);}
static void store_ui_cloud_settings(lv_event_t *e){(void)e;store_ui_prompt(true);}
static void store_ui_section(lv_event_t *e)
{
  if(g_store_ui.modal)return;
  g_store_ui.section=(unsigned)(uintptr_t)lv_event_get_user_data(e);g_store_ui.page=0;g_store_ui.selected[0]=0;
  store_ui_draw();
}
static void store_ui_category(lv_event_t *e)
{g_store_ui.category=(unsigned)(uintptr_t)lv_event_get_user_data(e);g_store_ui.page=0;store_ui_draw();}
static void store_ui_page(lv_event_t *e)
{g_store_ui.page=(unsigned)(uintptr_t)lv_event_get_user_data(e);store_ui_draw();}
static void store_ui_details(lv_event_t *e)
{
  unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);if(i>=g_store_ui.snapshot->count)return;
  strlcpy(g_store_ui.selected,g_store_ui.snapshot->entries[i].package,sizeof(g_store_ui.selected));store_ui_draw();
}
static void store_ui_refresh(lv_event_t *e)
{
  (void)e;int ret=glass_store_submit(GLASS_STORE_REFRESH,0,QPK_VOLUME_INTERNAL);
  if(ret)show_toast(store_ui_error(ret));
}
static void store_ui_cancel(lv_event_t *e)
{(void)e;glass_store_cancel();show_toast("正在取消，请等待后台收尾");}
static void store_ui_run(lv_event_t *e)
{
  unsigned encoded=(unsigned)(uintptr_t)lv_event_get_user_data(e);int i=store_ui_selected();if(i<0)return;
  enum glass_store_action action=encoded>=2?GLASS_STORE_REMOVE:GLASS_STORE_INSTALL;
  enum qpk_volume volume=(enum qpk_volume)(encoded%2);
  int ret=glass_store_submit(action,(unsigned)i,volume);
  store_ui_dialog_close(NULL);if(ret)show_toast(store_ui_error(ret));
}
static void store_ui_remove_confirm(lv_event_t *e)
{
  if(g_store_ui.modal)return;
  unsigned volume=(unsigned)(uintptr_t)lv_event_get_user_data(e);
  g_store_ui.modal=glass_box(g_store_ui.root,0,0,1024,568,detail_canvas(),0);
  lv_obj_add_flag(g_store_ui.modal,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *box=detail_tile(g_store_ui.modal,224,112,576,320);
  detail_text(box,"卸载这个应用？",28,28,520,28,detail_ink());
  detail_text(box,"应用将从所选存储位置移除。\n已保存的数据保留，重新安装后可继续使用。",28,90,520,20,detail_muted());
  store_ui_button(box,"取消",28,240,250,false,store_ui_dialog_close,0);
  store_ui_button(box,"卸载",294,240,254,true,store_ui_run,2+volume);
}
static void store_ui_open_installed(lv_event_t *e)
{
  enum qpk_volume v=(enum qpk_volume)(uintptr_t)lv_event_get_user_data(e);
  char package[48];strlcpy(package,g_store_ui.selected,sizeof(package));
  qpk_scan();
  for(int i=0;i<g_desktop.nqpk;i++)if(g_desktop.qpk[i].volume==v&&!strcmp(g_desktop.qpk[i].package,package)){
    g_desktop.apps_page=true;launch_external_qapp(i);return;
  }
  show_toast("应用暂时不可用，请刷新仓库");
}
static void store_ui_all_apps(lv_event_t *e){(void)e;glass_apps(NULL);}
static void store_ui_detail(unsigned i)
{
  struct glass_store_entry *app=&g_store_ui.snapshot->entries[i];lv_obj_t *body=g_store_ui.view;
  store_ui_button(body,"返回列表",24,22,124,false,store_ui_back,0);
  store_ui_art(body,app,24,92,80);
  detail_text(body,app->name,124,92,680,28,detail_ink());
  char metadata[160];snprintf(metadata,sizeof(metadata),"%s  ·  v%s  ·  %.1f KiB",store_ui_categories[app->category],app->version,app->bytes/1024.0);
  detail_text(body,metadata,124,142,680,16,detail_muted());
  lv_obj_t *intro=detail_tile(body,24,194,812,142);
  detail_text(intro,"关于这款应用",20,20,768,20,detail_ink());
  lv_obj_t *description=detail_text(intro,app->summary,20,58,768,16,detail_muted());lv_obj_set_height(description,44);
  detail_text(intro,app->cloud?"来源：云端商城 · 安装时校验完整文件":app->local?"来源：SD 本地仓库 · 请仅安装你信任的脚本":"来源：内置精选 · 离线可安装",20,112,768,14,detail_muted());
  if(g_store_ui.snapshot->busy){
    detail_text(body,"正在后台处理，完成后更新结果…",24,366,780,20,detail_ink());
    store_ui_button(body,"取消操作",24,414,220,false,store_ui_cancel,0);
  }else if(!app->installed){
    detail_text(body,"选择安装位置",24,356,780,20,detail_ink());
    for(unsigned v=0;v<2;v++){
      lv_obj_t *b=store_ui_button(body,v?"安装到 SD 卡":"安装到内置存储",24+v*414,398,398,v==1,store_ui_run,v);
      if(!g_store_ui.snapshot->free_bytes[v])lv_obj_add_state(b,LV_STATE_DISABLED);
    }
    char free_text[160];snprintf(free_text,sizeof(free_text),"可用空间：内置 %.1f MiB / SD %.1f MiB",g_store_ui.snapshot->free_bytes[0]/1048576.0,g_store_ui.snapshot->free_bytes[1]/1048576.0);
    detail_text(body,free_text,24,456,800,16,detail_muted());
  }else{
    for(unsigned v=0;v<2;v++)if(app->installed&(1u<<v)){
      store_ui_button(body,v?"打开 SD 卡应用":"打开内置应用",24,364+v*56,398,true,store_ui_open_installed,v);
      if(app->managed&(1u<<v))store_ui_button(body,"卸载（保留数据）",438,364+v*56,398,false,store_ui_remove_confirm,v);
    }
    detail_text(body,"已安装，可打开或管理应用。",24,492,800,16,detail_muted());
  }
  detail_text(body,app->package,24,530,800,16,detail_muted());
}
static bool store_ui_contains(const char *text,const char *query)
{
  /* ASCII case-insensitive package search; UTF-8 names retain exact matching. */
  for(const char *p=text;*p;p++){
    const unsigned char *a=(const unsigned char *)p,*b=(const unsigned char *)query;
    while(*a&&*b&&tolower(*a)==tolower(*b)){a++;b++;}if(!*b)return true;
  }
  return !*query;
}
static bool store_ui_matches(const struct glass_store_entry *app)
{
  if(g_store_ui.section==1&&g_store_ui.category&&app->category+1!=g_store_ui.category)return false;
  if(g_store_ui.section==2&&!app->local)return false;
  if(g_store_ui.section==3&&!app->installed)return false;
  if(g_store_ui.section==4&&!app->cloud)return false;
  return store_ui_contains(app->name,g_store_ui.query)||store_ui_contains(app->summary,g_store_ui.query)||store_ui_contains(app->package,g_store_ui.query);
}
static void store_ui_art_passive(lv_obj_t *object)
{
  lv_obj_remove_flag(object,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)store_ui_art_passive(lv_obj_get_child(object,i));
}
static void store_ui_collection(lv_event_t *e)
{(void)e;g_store_ui.section=1;g_store_ui.category=0;g_store_ui.page=0;store_ui_draw();}
static int store_ui_feature_index(const char *package)
{
  for(unsigned i=0;i<g_store_ui.snapshot->count;i++)if(!strcmp(g_store_ui.snapshot->entries[i].package,package))return (int)i;
  return -1;
}
static void store_ui_poster(lv_obj_t *body,const struct glass_store_feature *feature,unsigned position,unsigned count)
{
  int index=store_ui_feature_index(feature->package);if(index<0)return;
  const struct glass_store_entry *app=&g_store_ui.snapshot->entries[index];
  int x=position?540:24,width=count==1?812:position?296:500,pad=position?22:24;
  bool primary=position==0;
  static const uint32_t bg[]={0x254438,0xe1b38e,0x433b5b},ink[]={0xf2f2df,0x453023,0xf1eaf8},muted[]={0xc3d5b9,0x76513a,0xcabfe0},button[]={0xeff0de,0xf5dfca,0xe4d8f1},button_ink[]={0x2e4937,0x65442e,0x4b3a60},dial_bg[]={0xc9d9ac,0xf5dfca,0xd1c0e6},orbit_bg[]={0x345443,0xce9f78,0x5a506f};
  unsigned tone=feature->tone<3?feature->tone:0;
  lv_obj_t *poster=glass_box(body,x,126,width,224,bg[tone],22);lv_obj_set_style_clip_corner(poster,true,0);
  detail_text(poster,app->name,pad,20,width-2*pad,14,muted[tone]);
  int text_width=primary?width-216:width-2*pad;
  lv_obj_t *title=detail_text(poster,feature->title,pad,primary?48:52,text_width,primary?34:28,ink[tone]);
  lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);lv_obj_set_height(title,primary?100:84);lv_obj_set_style_text_line_space(title,primary?-4:-2,0);
  if(feature->subtitle[0])detail_text(poster,feature->subtitle,pad,primary?148:138,primary?text_width:width-140,14,muted[tone]);
  lv_obj_t *art=NULL;
  if(feature->art==0){
    int diameter=primary?184:100;
    art=glass_box(poster,width-diameter+10,primary?23:116,diameter,diameter,orbit_bg[tone],LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(art,1,0);lv_obj_set_style_border_color(art,lv_color_hex(muted[tone]),0);lv_obj_set_style_border_opa(art,100,0);
    int inset=primary?14:8,inner=diameter-2*inset;
    lv_obj_t *dial=glass_box(art,inset,inset,inner,inner,dial_bg[tone],LV_RADIUS_CIRCLE);
    if(feature->value[0]){
      int font_size=primary&&strlen(feature->value)<=4?64:28;
      lv_obj_t *number=home_text(dial,feature->value,0,primary?10:8,inner,font_size,button_ink[tone]);lv_obj_set_style_text_align(number,LV_TEXT_ALIGN_CENTER,0);
    }
    if(feature->unit[0]){
      lv_obj_t *unit=home_text(dial,feature->unit,0,primary?105:48,inner,12,button_ink[tone]);lv_obj_set_style_text_align(unit,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_letter_space(unit,primary?2:0,0);
    }
  }else if(feature->art==1){
    art=glass_box(poster,width-106,135,108,98,bg[tone],0);
    for(int i=0;i<3;i++){
      lv_obj_t *bar=glass_box(art,8+i*30,64-i*22,17,34+i*22,button_ink[tone],8);
      lv_obj_set_style_bg_opa(bar,130+i*45,0);
    }
  }else art=store_ui_art(poster,app,width-96,126,64);
  if(art)store_ui_art_passive(art);
  lv_obj_t *action=store_ui_pill(poster,primary?"探索应用  →":"了解应用  →",pad,178,144,store_ui_details,(uintptr_t)index);
  lv_obj_set_style_bg_color(action,lv_color_hex(button[tone]),0);lv_obj_set_style_text_color(lv_obj_get_child(action,0),lv_color_hex(button_ink[tone]),0);
  lv_obj_remove_flag(poster,LV_OBJ_FLAG_CLICKABLE);
}
static void store_ui_editorial(void)
{
  lv_obj_t *body=g_store_ui.view;struct glass_store_snapshot *s=g_store_ui.snapshot;
  for(unsigned i=0;i<s->featured_count&&i<2;i++)store_ui_poster(body,&s->featured[i],i,s->featured_count);
  detail_text(body,"随手好工具",24,375,420,20,detail_ink());
  char collection[64];snprintf(collection,sizeof(collection),"全部 %u 个应用  →",s->count);
  lv_obj_t *all=store_ui_pill(body,collection,650,368,186,store_ui_collection,0);lv_obj_set_style_bg_opa(all,LV_OPA_TRANSP,0);
  unsigned tool=s->count;
  for(unsigned i=0;i<s->count;i++){
    bool featured=false;for(unsigned f=0;f<s->featured_count;f++)if(!strcmp(s->entries[i].package,s->featured[f].package))featured=true;
    if(!featured){tool=i;break;}
  }
  if(tool<s->count){
    struct glass_store_entry *a=&s->entries[tool];
    lv_obj_t *row=glass_box(body,24,412,812,92,detail_canvas(),0);store_ui_art(row,a,0,12,60);
    detail_text(row,a->name,80,14,500,20,detail_ink());
    detail_text(row,a->summary,80,48,500,14,detail_muted());
    store_ui_pill(row,a->installed?"管理":"获取",688,28,124,store_ui_details,tool);
    lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);glass_input_guard_install(row);lv_obj_add_event_cb(row,store_ui_details,LV_EVENT_CLICKED,(void *)(uintptr_t)tool);
    lv_obj_set_style_border_side(row,LV_BORDER_SIDE_BOTTOM,0);lv_obj_set_style_border_width(row,1,0);lv_obj_set_style_border_color(row,lv_color_hex(detail_line()),0);
  }else detail_text(body,"更多应用可在完整目录中浏览",24,438,800,20,detail_muted());
  detail_text(body,s->busy?"正在后台处理…":"轻量应用，按需安装 · 内置 / SD / 云端",24,532,812,14,detail_muted());
}
static void store_ui_draw(void)
{
  if(!g_store_ui.root||g_store_ui.modal||!glass_ui_can_paint(g_store_ui.root))return;
  struct glass_store_snapshot *s=g_store_ui.snapshot;g_store_ui.painted=s->revision;
  lv_obj_clean(g_store_ui.view);
  for(unsigned n=0;n<5;n++){
    lv_obj_t *b=lv_obj_get_child(g_store_ui.rail,n+2);
    lv_obj_set_style_bg_color(b,lv_color_hex(n==g_store_ui.section?detail_soft():detail_paper()),0);
    lv_obj_set_style_bg_color(lv_obj_get_child(b,1),lv_color_hex(n==g_store_ui.section?detail_accent():detail_muted()),0);
  }
  int selected=store_ui_selected();if(selected>=0){store_ui_detail((unsigned)selected);return;}g_store_ui.selected[0]=0;
  lv_obj_t *body=g_store_ui.view;
  bool editorial=g_store_ui.section==0&&!g_store_ui.query[0]&&s->featured_count>0;
  if(editorial){
    lv_obj_t *eyebrow=home_text(body,"APP STORE",24,14,400,12,detail_muted());lv_obj_set_style_text_letter_space(eyebrow,2,0);
    detail_text(body,s->headline,24,42,520,32,detail_ink());
  }else detail_text(body,store_ui_sections[g_store_ui.section],24,22,400,32,detail_ink());
  store_ui_pill(body,g_store_ui.query[0]?"修改搜索":"搜索应用",558,26,166,store_ui_search,0);
  lv_obj_t *refresh=store_ui_pill(body,s->busy?"取消":"刷新",738,26,98,s->busy?store_ui_cancel:store_ui_refresh,0);
  (void)refresh;
  char subtitle[140];snprintf(subtitle,sizeof(subtitle),g_store_ui.query[0]?"搜索：%s":"发现值得留在桌面的小应用",g_store_ui.query);
  detail_text(body,editorial?s->tagline:subtitle,24,editorial?90:72,800,14,detail_muted());
  if(editorial){store_ui_editorial();return;}
  int top=114,rows=2;
  if(g_store_ui.section==1){
    const char *labels[]={"全部","实用工具","专注效率","轻松趣味"};
    for(unsigned c=0;c<4;c++)store_ui_button(body,labels[c],24+c*206,114,194,g_store_ui.category==c,store_ui_category,c);
    top=176;
  }else if(g_store_ui.section==2){
    detail_text(body,"将 index.json 与应用目录放入 /sdcard/app-store",24,114,800,16,detail_muted());top=154;
  }else if(g_store_ui.section==4){
    store_ui_button(body,"配置地址",24,114,180,false,store_ui_cloud_settings,0);
    detail_text(body,s->cloud_url[0]?s->cloud_url:"尚未配置云端商店",222,126,614,14,detail_muted());top=176;
  }
  unsigned indices[GLASS_STORE_MAX],count=0;
  for(unsigned i=0;i<s->count;i++)if(store_ui_matches(&s->entries[i]))indices[count++]=i;
  unsigned page_size=rows==1?3:4,pages=(count+page_size-1)/page_size;if(!pages)pages=1;
  if(g_store_ui.page>=pages)g_store_ui.page=pages-1;
  if(!count){
    detail_text(body,s->busy?"正在读取应用目录…":g_store_ui.query[0]?"没有找到匹配的应用":g_store_ui.section==4?(s->cloud_url[0]?"云端目录暂时没有可用应用":"配置服务地址后，即可浏览云端应用"):g_store_ui.section==2?(s->sd_repository?"仓库已读取，尚无额外应用":"还没有可用的 SD 仓库"):"这里还没有应用",24,top+52,800,24,detail_ink());
    if(g_store_ui.section==2&&s->catalog_error&&!s->busy)detail_text(body,store_ui_error(s->catalog_error),24,top+106,800,20,detail_muted());
    if(g_store_ui.section==4&&s->cloud_error&&!s->busy)detail_text(body,store_ui_error(s->cloud_error),24,top+106,800,20,detail_muted());
  }
  for(unsigned n=0;n<page_size&&g_store_ui.page*page_size+n<count;n++){
    unsigned i=indices[g_store_ui.page*page_size+n];struct glass_store_entry *a=&s->entries[i];
    int width=rows==1?260:398,height=rows==1?150:142,x=24+(n%(rows==1?3:2))*(width+16),y=top+(n/(rows==1?3:2))*(height+16);
    lv_obj_t *tile=detail_tile(body,x,y,width,height);
    lv_obj_set_style_border_width(tile,0,0);
    store_ui_art(tile,a,16,16,48);
    detail_text(tile,a->name,76,16,rows==1?width-92:width-200,20,detail_ink());
    detail_text(tile,store_ui_categories[a->category],76,46,rows==1?width-92:width-200,14,detail_muted());
    lv_obj_t *summary=detail_text(tile,a->summary,16,80,width-32,14,detail_muted());
    lv_obj_set_height(summary,rows==1?20:44);lv_label_set_long_mode(summary,LV_LABEL_LONG_DOT);
    char size_text[48];snprintf(size_text,sizeof(size_text),a->installed?"已安装":"%.1f KiB",a->bytes/1024.0);
    if(rows==1)detail_text(tile,size_text,16,118,120,14,detail_muted());
    store_ui_pill(tile,a->installed?"管理":"获取",width-100,rows==1?104:20,84,store_ui_details,i);
    lv_obj_add_flag(tile,LV_OBJ_FLAG_CLICKABLE);glass_input_guard_install(tile);
    lv_obj_add_event_cb(tile,store_ui_details,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
  }
  char footer[128];
  if(s->busy)strlcpy(footer,"正在后台处理…",sizeof(footer));
  else if(pages>1)snprintf(footer,sizeof(footer),"%u 个应用 · 第 %u / %u 页",count,g_store_ui.page+1,pages);
  else snprintf(footer,sizeof(footer),"%u 个应用 · 按需安装，随时可用",count);
  detail_text(body,footer,24,532,480,14,detail_muted());
  if(pages>1){
    lv_obj_t *prev=store_ui_button(body,"上一页",558,510,132,false,store_ui_page,g_store_ui.page?g_store_ui.page-1:0);
    lv_obj_t *next=store_ui_button(body,"下一页",704,510,132,false,store_ui_page,g_store_ui.page+1);
    if(!g_store_ui.page)lv_obj_add_state(prev,LV_STATE_DISABLED);
    if(g_store_ui.page+1>=pages)lv_obj_add_state(next,LV_STATE_DISABLED);
  }else if(g_store_ui.section==3)store_ui_button(body,"全部应用管理",634,510,202,false,store_ui_all_apps,0);
}
static void store_ui_poll(lv_timer_t *timer)
{
  (void)timer;bool busy=g_store_ui.snapshot->busy;unsigned revision=g_store_ui.snapshot->revision;
  glass_store_get(g_store_ui.snapshot);
  if(busy&&!g_store_ui.snapshot->busy&&revision!=g_store_ui.snapshot->revision){
    if(g_store_ui.snapshot->result)show_toast(store_ui_error(g_store_ui.snapshot->result));
    else if(g_store_ui.snapshot->completed_action==GLASS_STORE_INSTALL)show_toast("应用已安装");
    else if(g_store_ui.snapshot->completed_action==GLASS_STORE_REMOVE)show_toast("应用已卸载，保存的数据已保留");
    else if(g_store_ui.snapshot->completed_action==GLASS_STORE_CONFIGURE)show_toast(g_store_ui.snapshot->cloud_error?"地址已保存，云端暂不可用":"云端商店地址已保存");
    else if(g_store_ui.section==2&&g_store_ui.snapshot->catalog_error)show_toast(store_ui_error(g_store_ui.snapshot->catalog_error));
    else if(g_store_ui.section==4&&g_store_ui.snapshot->cloud_error)show_toast(store_ui_error(g_store_ui.snapshot->cloud_error));
  }
  if(g_store_ui.painted!=g_store_ui.snapshot->revision)store_ui_draw();
}
static void store_ui_deleted(lv_event_t *e)
{
  if(lv_event_get_target(e)!=g_store_ui.root)return;
  if(g_store_ui.timer)lv_timer_delete(g_store_ui.timer);
  glass_store_close();free(g_store_ui.snapshot);memset(&g_store_ui,0,sizeof(g_store_ui));
}
static void store_ui_back(lv_event_t *e)
{
  (void)e;if(g_store_ui.modal){store_ui_dialog_close(NULL);return;}
  if(g_store_ui.selected[0]){g_store_ui.selected[0]=0;store_ui_draw();return;}
  if(g_store_ui.return_apps)glass_apps(NULL);else panel_hide(NULL);
}
static void glass_store_app(lv_event_t *e)
{
  (void)e;if(g_store_ui.root)return;
  struct glass_store_snapshot *snapshot=calloc(1,sizeof(*snapshot));
  if(!snapshot){show_toast("内存不足，请稍后再试");return;}
  int ret=glass_store_open();if(ret){free(snapshot);show_toast(store_ui_error(ret));return;}
  bool from_apps=g_desktop.apps_page;
  lv_obj_t *card=panel_card("应用商店");if(!card){glass_store_close();free(snapshot);return;}
  detail_page(card);g_store_ui.root=card;g_store_ui.snapshot=snapshot;g_store_ui.return_apps=from_apps;
  lv_obj_add_event_cb(card,store_ui_deleted,LV_EVENT_DELETE,NULL);
  struct glass_app_frame frame=glass_app_frame_create(card,"应用商店",zh_font(20),store_ui_back,"返回");
  g_store_ui.body=frame.body;g_store_ui.rail=frame.sidebar;glass_app_frame_navigation(frame.body,5);
  for(unsigned i=0;i<5;i++){
    lv_obj_t *nav=store_ui_button(frame.sidebar,store_ui_sections[i],12,88+i*52,140,false,store_ui_section,i);
    lv_obj_t *text=lv_obj_get_child(nav,0);lv_obj_set_width(text,94);lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_LEFT,0);lv_obj_align(text,LV_ALIGN_LEFT_MID,34,0);
    lv_obj_t *mark=glass_box(nav,16,18,8,8,detail_muted(),3);lv_obj_remove_flag(mark,LV_OBJ_FLAG_CLICKABLE);
  }
  detail_text(frame.sidebar,"本机应用",24,GLASS_CONTENT_HEIGHT-96,120,14,detail_muted());
  g_store_ui.view=glass_app_frame_box(frame.body,0,0,860,568,detail_canvas());
  g_store_ui.timer=lv_timer_create(store_ui_poll,150,NULL);
  if(!g_store_ui.timer){panel_hide(NULL);show_toast("内存不足，请稍后再试");return;}
  glass_navigation_register(glass_store_app,from_apps,0);g_desktop.page_back=store_ui_back;
  ret=glass_store_submit(GLASS_STORE_REFRESH,0,QPK_VOLUME_INTERNAL);glass_store_get(snapshot);store_ui_draw();
  if(ret)show_toast(store_ui_error(ret));
}
