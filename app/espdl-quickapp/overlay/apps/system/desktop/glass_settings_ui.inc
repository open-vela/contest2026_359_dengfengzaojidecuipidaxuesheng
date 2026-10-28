/* SPDX-License-Identifier: Apache-2.0 */
static unsigned glass_language_requested;
static bool glass_language_pending;
static void glass_language_apply_async(void *unused)
{
  LV_UNUSED(unused);glass_language_pending=false;
  int result=glass_i18n_set_language(glass_language_requested);
  glass_language_refresh();
  if(result<0)show_toast(glass_tr("语言已切换，选择未能保存到 SD 卡"));
}
static void glass_language_selected(lv_event_t *event)
{
  glass_language_requested=lv_dropdown_get_selected(lv_event_get_target(event));
  if(glass_language_requested>=GLASS_LANGUAGE_COUNT)return;
  /* Defer page deletion until LVGL finishes dispatching this event. */
  if(!glass_language_pending&&lv_async_call(glass_language_apply_async,NULL)==LV_RESULT_OK)
    glass_language_pending=true;
}
static void glass_language_deleted(lv_event_t *event)
{
  if(g_glass.language_dropdown==lv_event_get_target(event)){
    g_glass.language_dropdown=NULL;
    if(glass_language_pending){lv_async_call_cancel(glass_language_apply_async,NULL);glass_language_pending=false;}
  }
}
static void glass_global_font_control_deleted(lv_event_t *event)
{
  if(g_glass.global_font_dropdown==lv_event_get_target(event))g_glass.global_font_dropdown=NULL;
}

static void glass_settings(lv_event_t *e)
{
  LV_UNUSED(e);glass_preferences_update();
  glass_navigation_capture_settings();
  lv_obj_t *card=panel_card(glass_tr("控制面板"));if(!card)return;
  g_desktop.settings_page=true;g_desktop.page_back=NULL;
  detail_page(card);g_control_card=card;
  lv_obj_add_event_cb(card,glass_control_deleted,LV_EVENT_DELETE,NULL);
  glass_settings_sidebar(card,SETTINGS_PANEL);
  detail_text(card,glass_tr("控制面板"),216,26,300,28,detail_ink());
  detail_text(card,glass_tr("显示、声音与个性化"),216,66,400,14,detail_muted());
  g_preferences_status=detail_text(card,glass_preferences_status_text(),626,36,374,14,detail_muted());
  g_preferences_shown=glass_preferences_display_state();
  lv_obj_add_event_cb(g_preferences_status,glass_preferences_status_deleted,LV_EVENT_DELETE,NULL);

  lv_obj_t *display=detail_tile(card,216,104,384,154);
  home_icon(display,GLASS_ICON_SUN,24,22,24,detail_accent());
  detail_text(display,glass_tr("屏幕亮度"),60,22,300,20,detail_ink());
  lv_obj_t *value=detail_text(display,"100%",24,74,130,32,detail_ink());
  lv_obj_t *brightness=lv_slider_create(display);g_glass.brightness_slider=brightness;
  lv_obj_set_pos(brightness,136,94);lv_obj_set_size(brightness,210,6);
  lv_obj_set_ext_click_area(brightness,18);
  lv_obj_set_style_radius(brightness,LV_RADIUS_CIRCLE,LV_PART_MAIN);
  lv_obj_set_style_radius(brightness,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightness,lv_color_hex(detail_soft()),LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightness,lv_color_hex(detail_accent()),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightness,lv_color_hex(detail_paper()),LV_PART_KNOB);
  lv_obj_set_style_pad_all(brightness,7,LV_PART_KNOB);
  lv_obj_set_style_radius(brightness,LV_RADIUS_CIRCLE,LV_PART_KNOB);
  lv_obj_set_style_border_width(brightness,2,LV_PART_KNOB);
  lv_obj_set_style_border_color(brightness,lv_color_hex(detail_accent()),LV_PART_KNOB);
  lv_obj_set_style_shadow_width(brightness,0,LV_PART_KNOB);
  lv_slider_set_range(brightness,10,100);
  lv_slider_set_value(brightness,g_screen_brightness,LV_ANIM_OFF);
  lv_obj_add_event_cb(brightness,glass_brightness_preview,LV_EVENT_VALUE_CHANGED,value);
  lv_obj_add_event_cb(brightness,glass_brightness_preview,LV_EVENT_PRESS_LOST,value);
#ifdef GLASS_PWM_BACKLIGHT
  lv_label_set_text_fmt(value,"%d%%",g_screen_brightness);
  lv_obj_add_event_cb(brightness,glass_brightness_changed,LV_EVENT_VALUE_CHANGED,value);
  lv_obj_add_event_cb(brightness,glass_brightness_changed,LV_EVENT_RELEASED,value);
  detail_text(display,glass_tr("拖动滑块调整屏幕亮度"),24,126,336,12,detail_muted());
#else
  LV_UNUSED(value);
  lv_obj_add_state(brightness,LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(brightness,lv_color_hex(g_desktop.light_theme?0xb8bac8:0x626878),LV_PART_INDICATOR|LV_STATE_DISABLED);
  detail_text(display,glass_tr("当前屏幕使用固定亮度"),24,126,336,12,detail_muted());
#endif

  lv_obj_t *audio=detail_tile(card,616,104,384,154);
  home_icon(audio,LV_SYMBOL_VOLUME_MAX,24,22,24,detail_accent());
  detail_text(audio,glass_tr("扬声器音量"),60,22,280,20,detail_ink());
  value=detail_text(audio,"",24,74,108,32,detail_ink());
  lv_label_set_text_fmt(value,"%u%%",glass_speaker_volume());
  lv_obj_t *volume=lv_slider_create(audio);
  g_glass.speaker_slider=volume;g_glass.speaker_value=value;
  lv_obj_add_event_cb(volume,glass_speaker_control_deleted,LV_EVENT_DELETE,NULL);
  lv_obj_set_pos(volume,136,94);lv_obj_set_size(volume,210,6);lv_obj_set_ext_click_area(volume,18);
  lv_slider_set_range(volume,0,100);lv_slider_set_value(volume,(int)glass_speaker_volume(),LV_ANIM_OFF);
  lv_obj_set_style_bg_color(volume,lv_color_hex(detail_soft()),LV_PART_MAIN);
  lv_obj_set_style_bg_color(volume,lv_color_hex(detail_accent()),LV_PART_INDICATOR);
  lv_obj_set_style_radius(volume,LV_RADIUS_CIRCLE,LV_PART_MAIN);
  lv_obj_set_style_radius(volume,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volume,lv_color_hex(detail_paper()),LV_PART_KNOB);
  lv_obj_set_style_pad_all(volume,7,LV_PART_KNOB);
  lv_obj_set_style_radius(volume,LV_RADIUS_CIRCLE,LV_PART_KNOB);
  lv_obj_set_style_border_width(volume,2,LV_PART_KNOB);
  lv_obj_set_style_border_color(volume,lv_color_hex(detail_accent()),LV_PART_KNOB);
  lv_obj_set_style_shadow_width(volume,0,LV_PART_KNOB);
  lv_obj_add_event_cb(volume,glass_speaker_changed,LV_EVENT_VALUE_CHANGED,value);
  detail_text(audio,glass_tr("统一扬声器输出 · 0% 静音"),24,126,336,12,detail_muted());

  lv_obj_t *appearance=detail_tile(card,216,274,784,292);
  home_icon(appearance,GLASS_ICON_SPARKLES,24,22,24,detail_accent());
  detail_text(appearance,glass_tr("个性化"),60,22,280,20,detail_ink());
  detail_text(appearance,glass_tr("浅色外观"),24,84,260,16,detail_ink());
  lv_obj_t *sw=lv_switch_create(appearance);g_glass.settings_switch=sw;
  glass_control_switch_style(sw);
  lv_obj_set_pos(sw,308,80);lv_obj_set_size(sw,52,28);
  lv_obj_set_ext_click_area(sw,8);
  lv_obj_set_style_bg_opa(sw,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw,lv_color_hex(detail_accent()),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw,LV_OPA_COVER,LV_PART_INDICATOR|LV_STATE_CHECKED);
  if(g_desktop.light_theme)lv_obj_add_state(sw,LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw,theme_changed,LV_EVENT_VALUE_CHANGED,NULL);
  detail_text(appearance,glass_tr("减少动效"),24,136,260,16,detail_ink());
  sw=lv_switch_create(appearance);g_glass.motion_switch=sw;
  glass_control_switch_style(sw);
  lv_obj_set_pos(sw,308,132);lv_obj_set_size(sw,52,28);
  lv_obj_set_ext_click_area(sw,8);
  lv_obj_set_style_bg_opa(sw,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw,lv_color_hex(detail_accent()),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw,LV_OPA_COVER,LV_PART_INDICATOR|LV_STATE_CHECKED);
  if(g_glass.reduced_motion)lv_obj_add_state(sw,LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw,glass_motion_changed,LV_EVENT_VALUE_CHANGED,NULL);
  glass_box(appearance,396,72,1,180,detail_line(),0);
  detail_text(appearance,glass_tr("主视觉配色"),24,196,120,14,detail_muted());
  const uint32_t colors[]={0x77658f,0xb77860,0x498d82};
  for(unsigned i=0;i<3;i++){
    lv_obj_t *choice=glass_button(appearance,"",150+i*62,188,52,colors[i],glass_palette_select,(void *)(uintptr_t)i);
    lv_obj_set_size(choice,52,32);lv_obj_set_style_radius(choice,10,0);
    lv_obj_set_style_border_width(choice,g_glass.palette==i?2:0,0);
    lv_obj_set_style_border_color(choice,lv_color_hex(g_desktop.light_theme?0xffffff:0xe0d7ee),0);
    lv_obj_set_style_border_opa(choice,LV_OPA_COVER,0);
    if(g_glass.palette==i){lv_obj_t *check=home_icon(choice,LV_SYMBOL_OK,0,0,20,0xffffff);lv_obj_center(check);}
  }
  detail_text(appearance,glass_tr("桌面小组件"),420,72,300,14,detail_muted());
  lv_obj_t *dropdown=lv_dropdown_create(appearance);g_glass.widget_dropdown=dropdown;
  lv_dropdown_set_options(dropdown,glass_tr("空气质量\n环境湿度\n系统运行"));
  lv_dropdown_set_selected(dropdown,g_glass.widget);
  lv_obj_set_pos(dropdown,420,100);lv_obj_set_size(dropdown,340,36);
  glass_input_style(dropdown);
  lv_obj_set_style_text_font(dropdown,home_font(14),LV_PART_MAIN);
  lv_obj_set_style_text_color(dropdown,lv_color_hex(detail_ink()),LV_PART_MAIN);
  lv_obj_set_style_bg_color(dropdown,lv_color_hex(detail_soft()),0);
  lv_obj_set_style_border_color(dropdown,lv_color_hex(detail_line()),0);
  lv_obj_set_style_border_opa(dropdown,LV_OPA_COVER,0);
  lv_obj_set_style_text_font(dropdown,glass_icon_font(16),LV_PART_INDICATOR);
  lv_obj_t *list=lv_dropdown_get_list(dropdown);
  lv_obj_set_style_text_font(list,home_font(16),0);
  lv_obj_set_style_text_color(list,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_bg_color(list,lv_color_hex(detail_paper()),0);
  lv_obj_set_style_radius(list,12,0);
  lv_obj_add_event_cb(dropdown,glass_widget_select,LV_EVENT_VALUE_CHANGED,NULL);

  detail_text(appearance,glass_tr("全局字体"),420,164,300,14,detail_muted());
  dropdown=lv_dropdown_create(appearance);g_glass.global_font_dropdown=dropdown;
  lv_dropdown_clear_options(dropdown);
  for(unsigned i=0;i<glass_lyric_font_count();i++)lv_dropdown_add_option(dropdown,glass_lyric_font_name(i),LV_DROPDOWN_POS_LAST);
  lv_dropdown_set_selected(dropdown,global_font_requested);
  lv_dropdown_set_text(dropdown,global_font_requested!=global_font_selected?glass_tr("字体加载中…"):glass_lyric_font_name(global_font_selected));
  lv_obj_set_pos(dropdown,420,192);lv_obj_set_size(dropdown,340,36);
  lv_obj_set_ext_click_area(dropdown,4);glass_input_style(dropdown);
  lv_obj_set_style_text_font(dropdown,home_font(14),LV_PART_MAIN);
  lv_obj_set_style_text_color(dropdown,lv_color_hex(detail_ink()),LV_PART_MAIN);
  lv_obj_set_style_bg_color(dropdown,lv_color_hex(detail_soft()),0);
  lv_obj_set_style_border_color(dropdown,lv_color_hex(detail_line()),0);
  lv_obj_set_style_border_opa(dropdown,LV_OPA_COVER,0);
  lv_obj_set_style_text_font(dropdown,glass_icon_font(16),LV_PART_INDICATOR);
  list=lv_dropdown_get_list(dropdown);
  lv_obj_set_style_text_font(list,home_font(16),0);
  lv_obj_set_style_text_color(list,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_bg_color(list,lv_color_hex(detail_paper()),0);
  lv_obj_set_style_radius(list,12,0);lv_obj_set_style_max_height(list,320,0);
  lv_obj_set_style_bg_color(list,lv_color_hex(detail_accent()),LV_PART_SELECTED|LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(list,LV_OPA_20,LV_PART_SELECTED|LV_STATE_CHECKED);
  lv_obj_set_style_text_color(list,lv_color_hex(detail_ink()),LV_PART_SELECTED|LV_STATE_CHECKED);
  lv_obj_add_event_cb(dropdown,glass_global_font_selected,LV_EVENT_VALUE_CHANGED,NULL);
  lv_obj_add_event_cb(dropdown,glass_global_font_control_deleted,LV_EVENT_DELETE,NULL);
  detail_text(appearance,glass_tr("桌面、应用与歌词统一使用"),420,240,340,12,detail_muted());

  detail_text(appearance,glass_tr("界面语言"),24,244,104,14,detail_muted());
  dropdown=lv_dropdown_create(appearance);g_glass.language_dropdown=dropdown;
  lv_dropdown_set_options(dropdown,"简体中文\nEnglish");
  lv_dropdown_set_selected(dropdown,glass_i18n_language());
  lv_obj_set_pos(dropdown,136,236);lv_obj_set_size(dropdown,224,36);
  glass_input_style(dropdown);
  lv_obj_set_style_text_font(dropdown,home_font(14),LV_PART_MAIN);
  lv_obj_set_style_text_color(dropdown,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_text_font(dropdown,glass_icon_font(16),LV_PART_INDICATOR);
  list=lv_dropdown_get_list(dropdown);
  lv_obj_set_style_text_font(list,home_font(16),0);
  lv_obj_set_style_bg_color(list,lv_color_hex(detail_paper()),0);
  lv_obj_set_style_text_color(list,lv_color_hex(detail_ink()),0);
  lv_obj_add_event_cb(dropdown,glass_language_selected,LV_EVENT_VALUE_CHANGED,NULL);
  lv_obj_add_event_cb(dropdown,glass_language_deleted,LV_EVENT_DELETE,NULL);

}
