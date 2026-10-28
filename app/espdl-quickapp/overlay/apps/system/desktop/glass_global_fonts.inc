/* SPDX-License-Identifier: Apache-2.0 */
/* Keep each published font address stable: existing labels, textareas, spans,
 * IME and QuickJS widgets all retain these pointers. TinyTTF owns the source
 * descriptors/caches for the desktop lifetime; these views never destroy them.
 * The saved embedded view also provides a cycle-free missing-glyph fallback. */
struct glass_global_font_view {
  lv_font_t *target;
  lv_font_t embedded;
  lv_font_t coverage;
  unsigned pixels;
};
#define GLOBAL_FONT_VIEW_COUNT 14
static struct glass_global_font_view *global_font_views;
static unsigned global_font_view_count;
static bool global_fonts_initialized;
static lv_timer_t *global_font_timer;
static bool global_font_restore_checked,global_font_save_pending;

static void glass_global_font_bind(lv_font_t *font,unsigned pixels)
{
  if(!font)return;
  if(!global_font_views){
    global_font_views=calloc(GLOBAL_FONT_VIEW_COUNT,sizeof(*global_font_views));
    if(!global_font_views)return;
  }
  /* Host fixtures can share the 72 px clock and lyric font. */
  for(unsigned i=0;i<global_font_view_count;i++)
    if(global_font_views[i].target==font)return;
  if(global_font_view_count>=GLOBAL_FONT_VIEW_COUNT)return;
  unsigned i=global_font_view_count++;
  global_font_views[i].target=font;
  global_font_views[i].embedded=*font;
  global_font_views[i].pixels=pixels;
  lv_font_t *chosen=glass_lyric_font_get(global_font_selected,pixels);
  if(chosen){*font=*chosen;font->fallback=&global_font_views[i].embedded;}
}

static void glass_global_fonts_init(void)
{
  if(global_fonts_initialized)return;
  if(!global_font_views){
    global_font_views=calloc(GLOBAL_FONT_VIEW_COUNT,sizeof(*global_font_views));
    if(!global_font_views)return;
  }
  global_fonts_initialized=true;
  glass_global_font_bind(g_desktop.font16,16);
  glass_global_font_bind(g_desktop.font20,20);
  glass_global_font_bind(g_desktop.font28,28);
  glass_global_font_bind(g_home_font12,12);
  glass_global_font_bind(g_home_font14,14);
  glass_global_font_bind(g_home_font32,32);
  glass_global_font_bind(g_home_font34,34);
  glass_global_font_bind(g_home_font40,40);
  glass_global_font_bind(g_home_font64,64);
  glass_global_font_bind(g_glass.font48,48);
  glass_global_font_bind(g_glass.music_font72,72);
  glass_global_font_bind(g_glass.font72,128);
  if(g_desktop.font20)glass_global_font_bind(&g_hass_font20,20);
}

static void glass_global_fonts_apply(void)
{
  glass_global_fonts_init();
  if(!global_fonts_initialized){
    global_font_selected=global_font_requested=0;global_font_save_pending=false;
    show_toast(glass_tr("内存不足，暂时无法切换字体"));return;
  }
  glass_lyric_font_set_global(global_font_selected);
  for(unsigned i=0;i<global_font_view_count;i++){
    lv_font_t *base=&global_font_views[i].embedded;
    unsigned pixels=global_font_views[i].pixels;
    /* HA uses a text degree mark with its normal typographic advance. */
    const lv_font_t *icons=global_font_views[i].target==&g_hass_font20?
      &lv_font_montserrat_24:glass_icon_font(pixels==28?24:pixels==40||pixels==34?32:pixels);
    lv_font_t *coverage=glass_sd_font_get(pixels);
    if(coverage){
      global_font_views[i].coverage=*coverage;
      global_font_views[i].coverage.fallback=icons;
    }
    base->fallback=coverage?&global_font_views[i].coverage:icons;
    if(global_font_views[i].target==chat_bold_font)base->fallback=zh_font(20);
    lv_font_t *chosen=glass_lyric_font_get(global_font_selected,pixels);
    *global_font_views[i].target=chosen?*chosen:*base;
    if(chosen)global_font_views[i].target->fallback=base;
  }
  /* Font metrics changed without changing their public addresses. Clear
   * layout/text measurements throughout active, hidden and system layers. */
  lv_obj_report_style_change(NULL);
  music_full_fonts_changed();
  lv_obj_invalidate(lv_screen_active());
}

static void glass_global_fonts_poll(void)
{
  bool changed=glass_sd_font_poll()!=0;
  unsigned saved;
  if(!global_font_restore_checked&&glass_font_choice_load(&saved)){
    global_font_restore_checked=true;
    if(saved&&glass_lyric_font_request(saved)==0)global_font_requested=saved;
  }
  changed=glass_lyric_font_poll()||changed;
  if(global_font_requested!=global_font_selected){
    int state=glass_lyric_font_state(global_font_requested);
    if(state==3){global_font_selected=global_font_requested;changed=true;}
    else if(state==4){
      global_font_requested=global_font_selected;global_font_save_pending=false;changed=true;
      show_toast(glass_tr("未找到可用字体，请先安装字体包"));
    }
  }
  if(global_font_selected&&glass_lyric_font_state(global_font_selected)==4){
    global_font_selected=global_font_requested=0;global_font_save_pending=false;changed=true;
    show_toast(glass_tr("字体读取失败，已恢复默认字体"));
  }
  if(changed)glass_global_fonts_apply();
  if(global_font_save_pending&&global_font_requested==global_font_selected){
    global_font_save_pending=false;
    if(glass_font_choice_save(global_font_selected)<0)
      show_toast(glass_tr("字体已切换，选择未能保存到 SD 卡"));
  }
  if(glass_font_choice_save_status()<0)show_toast(glass_tr("字体已切换，选择未能保存到 SD 卡"));
  if(g_glass.global_font_dropdown){
    lv_dropdown_set_selected(g_glass.global_font_dropdown,global_font_requested);
    lv_dropdown_set_text(g_glass.global_font_dropdown,global_font_requested!=global_font_selected?
      glass_tr("字体加载中…"):glass_lyric_font_name(global_font_selected));
  }
  if(global_font_requested==global_font_selected&&global_font_timer){
    lv_timer_delete(global_font_timer);global_font_timer=NULL;
  }
}

static void glass_global_font_check(lv_timer_t *timer)
{LV_UNUSED(timer);glass_global_fonts_poll();}

static void glass_global_font_selected(lv_event_t *event)
{
  lv_obj_t *select=lv_event_get_target(event);
  unsigned index=lv_dropdown_get_selected(select);
  if(index>=glass_lyric_font_count())return;
  global_font_restore_checked=true;
  if(index==global_font_requested)return;
  if(glass_lyric_font_request(index)<0){
    lv_dropdown_set_selected(select,global_font_requested);
    show_toast(glass_tr("字体暂时不可用"));return;
  }
  global_font_save_pending=true;global_font_requested=index;
  glass_global_fonts_poll();
  if(global_font_requested!=global_font_selected&&!global_font_timer){
    global_font_timer=lv_timer_create(glass_global_font_check,100,NULL);
    if(!global_font_timer){
      global_font_save_pending=false;global_font_requested=global_font_selected;glass_global_fonts_poll();
      show_toast(glass_tr("内存不足，暂时无法切换字体"));
    }
  }
}
