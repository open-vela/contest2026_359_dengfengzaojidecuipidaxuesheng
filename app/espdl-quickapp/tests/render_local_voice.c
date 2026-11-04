/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
extern void review_local_fixture(int,const char *);
extern void review_local_woke(bool);
extern void review_chat_dialog_fixture(bool);
extern unsigned review_voice_start_count(void);
extern void review_local_elapsed_fixture(unsigned);
extern void review_local_error_fixture(int);
extern int review_local_displayed_error(void);
extern void review_local_config_error(int);
extern void review_local_asr_fixture(bool,unsigned,const char *);
extern void review_local_asr_error(int);
extern unsigned review_local_listens(void),review_local_finishes(void),review_local_stops(void);
extern void review_local_progress_fixture(unsigned,unsigned);
extern void review_local_cold_fixture(void);
extern enum glv_mode review_local_sent_mode(void);
extern const char *review_local_sent_text(void);
static void local_keyboard_bounds(void)
{
  lv_obj_update_layout(g_local_ui->card);
  lv_obj_t *keyboard=g_local_ui->keyboard;
  lv_obj_t *bar=lv_obj_get_child(g_local_ui->card,lv_obj_get_index(keyboard)+1);
  lv_area_t keys,candidates,input;
  lv_obj_get_coords(keyboard,&keys);lv_obj_get_coords(bar,&candidates);
  lv_obj_get_coords(g_local_ui->input,&input);
  assert(keys.x1>=0&&keys.x2<1024&&keys.y1>=GLASS_STATUS_HEIGHT&&keys.y2<600);
  assert(keys.x1==input.x1&&keys.x2==input.x2);
  assert(candidates.x1==keys.x1&&candidates.x2==keys.x2);
  assert(input.y2<candidates.y1&&candidates.y2<keys.y1);
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];FILE *f=fopen(argv[1],"rb");assert(f);
  fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);void *font=malloc(size);assert(font);
  assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  glass_home_fonts_init(font,size);g_glass.reduced_motion=true;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  for(unsigned light=0;light<2;light++){
    g_desktop.light_theme=light;review_local_cold_fixture();glass_local_voice_open(NULL);glass_status_tick();
    assert(lv_dropdown_get_selected(g_local_ui->thinking)==1);
    assert(lv_obj_has_state(g_local_ui->thinking,LV_STATE_DISABLED));
    review_click("小助手");assert(g_local_ui->mode==GLV_CONTROL);
    lv_obj_update_layout(g_local_ui->card);
    for(unsigned nav=0;nav<3;nav++){
      lv_area_t area;lv_obj_get_coords(g_local_ui->navigation[nav],&area);
      assert(area.x1==GLASS_SIDEBAR_PAD&&area.x2+1==GLASS_SIDEBAR_WIDTH-GLASS_SIDEBAR_PAD);
      assert(lv_obj_get_y(g_local_ui->navigation[nav])==96+(int)nav*(GLASS_SIDEBAR_ITEM_HEIGHT+GLASS_SIDEBAR_GAP));
      assert(lv_color_eq(lv_obj_get_style_bg_color(g_local_ui->navigation[nav],0),lv_color_hex(nav==GLV_CONTROL?detail_soft():detail_paper())));
    }
    for(unsigned level=0;level<3;level++){
      lv_dropdown_set_selected(g_local_ui->thinking,level);lv_obj_send_event(g_local_ui->thinking,LV_EVENT_VALUE_CHANGED,NULL);
      struct glv_snapshot thinking;glass_local_voice_get(&thinking,NULL,0);assert(thinking.thinking_rounds==level+4);
    }
    lv_dropdown_set_selected(g_local_ui->thinking,1);lv_obj_send_event(g_local_ui->thinking,LV_EVENT_VALUE_CHANGED,NULL);
    review_capture(display,"local-assistant-medium","双AI · 小助手 · Low4 Medium5 High6");
    review_local_fixture(GLV_GENERATING,"");local_ui_poll(NULL);review_click("普通对话");assert(g_local_ui->mode==GLV_CONTROL);
    review_local_fixture(GLV_IDLE,"");local_ui_poll(NULL);review_click("普通对话");assert(g_local_ui->mode==GLV_DIALOGUE);
    assert(g_local_ui&&!lv_obj_has_state(g_local_ui->send,LV_STATE_DISABLED)&&!lv_obj_has_state(g_local_ui->microphone,LV_STATE_DISABLED));
    assert(lv_obj_has_state(g_local_ui->speak,LV_STATE_DISABLED));review_capture(display,"local-dialogue","本地对话 · 默认文字 · 语音按需加载");
    review_local_fixture(GLV_SPEAKING,"你好，很高兴见到你。\n所有模型从 SD 卡读取，回答只保留在本次会话中。");local_ui_poll(NULL);
    assert(lv_obj_has_state(g_local_ui->send,LV_STATE_DISABLED));review_capture(display,"local-answer","本地回答 · 播报中可以触摸停止");
    review_local_elapsed_fixture(2000);local_ui_poll(NULL);assert(strstr(lv_label_get_text(g_local_ui->metrics),"2.0 秒"));
    review_local_error_fixture(-EIO);local_ui_poll(NULL);assert(review_local_displayed_error()==-EIO);
    assert(strstr(lv_label_get_text(g_local_ui->status),"请检查 SD 模型后重试"));
    review_local_fixture(GLV_IDLE,"");local_ui_poll(NULL);
    review_local_asr_error(-EBADMSG);local_ui_poll(NULL);assert(review_local_displayed_error()==-EBADMSG);
    assert(strstr(lv_label_get_text(g_local_ui->status),"离线转写：")&&strstr(lv_label_get_text(g_local_ui->status),"文字功能可用"));
    assert(!lv_obj_has_state(g_local_ui->send,LV_STATE_DISABLED)&&!lv_obj_has_state(g_local_ui->microphone,LV_STATE_DISABLED));
    review_local_fixture(GLV_CHECKING_ASR,"");local_ui_poll(NULL);assert(strstr(lv_label_get_text(g_local_ui->status),"检查转写模型"));
    review_local_fixture(GLV_PREFILL,"");review_local_progress_fixture(12,24);local_ui_poll(NULL);
    assert(strstr(lv_label_get_text(g_local_ui->status),"50%"));review_capture(display,"local-prefill","本地推理 · 输入处理进度");
    review_click("停止");struct glv_snapshot observed;glass_local_voice_get(&observed,NULL,0);assert(observed.cancel_source==GLV_CANCEL_UI_STOP);
    review_click("新对话");glass_local_voice_get(&observed,NULL,0);assert(observed.cancel_source==GLV_CANCEL_UI_NEW);
    assert(!strstr(lv_label_get_text(g_local_ui->status),"文字功能可用")&&lv_obj_has_state(g_local_ui->send,LV_STATE_DISABLED));
    review_local_fixture(GLV_IDLE,"");local_ui_poll(NULL);
    review_capture(display,"local-asr-fault","转写模型异常 · 文字功能保留");review_local_asr_fixture(false,0,"");local_ui_poll(NULL);
    lv_obj_send_event(g_local_ui->input,LV_EVENT_CLICKED,NULL);assert(g_local_ui->keyboard);
    local_keyboard_bounds();
    assert(!glass_ime_start_voice(g_local_ui->keyboard));review_capture(display,"local-keyboard","中文输入 · 网络麦克风关闭");
    lv_obj_send_event(g_local_ui->keyboard,LV_EVENT_CANCEL,NULL);assert(!g_local_ui->keyboard);
    for(unsigned reopen=0;reopen<3;reopen++){
      lv_obj_send_event(g_local_ui->input,LV_EVENT_CLICKED,NULL);local_keyboard_bounds();
      lv_obj_send_event(g_local_ui->keyboard,LV_EVENT_READY,NULL);
      lv_obj_update_layout(g_local_ui->card);
      assert(!g_local_ui->keyboard&&lv_obj_get_y(g_local_ui->input)==354);
      assert(!lv_obj_has_flag(g_local_ui->result,LV_OBJ_FLAG_HIDDEN));
    }
    assert(!lv_obj_has_state(g_local_ui->microphone,LV_STATE_DISABLED));
    review_local_asr_fixture(true,0,"");local_ui_poll(NULL);unsigned listened=review_local_listens(),finished=review_local_finishes();
    lv_obj_send_event(g_local_ui->microphone,LV_EVENT_CLICKED,NULL);assert(review_local_listens()==listened+1);
    review_local_asr_fixture(true,16384,"");local_ui_poll(NULL);assert(lv_bar_get_value(g_local_ui->level)==50);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_local_ui->microphone,0)),"结束录音"));
    review_capture(display,"local-listening","本地录音 · 输入音量 · 结束录音");
    lv_obj_send_event(g_local_ui->microphone,LV_EVENT_CLICKED,NULL);assert(review_local_finishes()==finished+1);
    review_local_asr_fixture(true,0,"你好，今天天气怎么样？");local_ui_poll(NULL);
    assert(!strcmp(lv_textarea_get_text(g_local_ui->input),"你好，今天天气怎么样？"));
    lv_textarea_set_text(g_local_ui->input,"保留编辑后的草稿");review_local_fixture(GLV_SPEAKING,"回答");local_ui_poll(NULL);
    assert(!strcmp(lv_textarea_get_text(g_local_ui->input),"保留编辑后的草稿"));
    review_local_fixture(GLV_IDLE,"回答");local_ui_poll(NULL);
    assert(!lv_obj_has_state(g_local_ui->speak,LV_STATE_DISABLED));review_click("朗读回答");
    assert(review_local_sent_mode()==GLV_READ&&!strcmp(review_local_sent_text(),"回答"));
    assert(!strcmp(lv_textarea_get_text(g_local_ui->input),"保留编辑后的草稿"));
    review_local_fixture(GLV_IDLE,"回答");local_ui_poll(NULL);
    lv_obj_send_event(g_local_ui->check,LV_EVENT_CLICKED,NULL);assert(!strcmp(lv_textarea_get_text(g_local_ui->input),"保留编辑后的草稿"));
    review_click("保存会话");assert(!strcmp(lv_textarea_get_text(g_local_ui->input),"保留编辑后的草稿"));
    review_local_fixture(GLV_SPEAKING,"旧回答");local_ui_poll(NULL);review_click("新对话");
    assert(!strstr(lv_label_get_text(g_local_ui->output),"旧回答"));
    unsigned stops=review_local_stops();panel_hide(NULL);assert(!g_local_ui&&review_local_stops()==stops);
    review_local_fixture(GLV_IDLE,"");review_local_asr_fixture(true,0,"");glass_local_voice_open(NULL);
    lv_obj_send_event(g_local_ui->microphone,LV_EVENT_CLICKED,NULL);panel_hide(NULL);assert(review_local_stops()==stops+1);
    review_local_fixture(GLV_IDLE,"");review_local_asr_fixture(false,0,"");glass_local_voice_open(NULL);
    g_local_ui->mode=GLV_READ;lv_label_set_text(g_local_ui->heading,"文字朗读");lv_label_set_text(g_local_ui->subtitle,"V2 声音 · 舒缓 · 24 kHz");
    lv_label_set_text(lv_obj_get_child(g_local_ui->send,0),"朗读");g_local_ui->revision=UINT32_MAX;local_ui_poll(NULL);
    review_capture(display,"local-reading","V2 文字朗读");
    lv_area_t area;lv_obj_update_layout(g_local_ui->card);lv_obj_get_coords(g_local_ui->metrics,&area);assert(area.y2<600);
    lv_obj_get_coords(g_local_ui->footer,&area);assert(area.y2<600);
    panel_hide(NULL);assert(!g_local_ui);
    for(unsigned i=0;i<20;i++){glass_local_voice_open(NULL);panel_hide(NULL);assert(!g_local_ui);}
    review_local_fixture(GLV_IDLE,"");glass_local_voice_enable_wake(true);glass_local_voice_set_wake(true);
    glass_settings(NULL);review_click("本地语音");assert(g_system_page_pending);glass_system_page_async(NULL);
    assert(g_local_settings&&g_system_page_kind==SETTINGS_LOCAL_VOICE);
    lv_obj_update_layout(g_local_settings->card);lv_obj_t *back_label=review_label_find(g_local_settings->card,"返回设置");assert(back_label);
    lv_area_t back_area;lv_obj_get_coords(back_label,&back_area);assert(back_area.x1>=12&&back_area.x2<GLASS_SIDEBAR_WIDTH);
    review_capture(display,"local-settings","系统设置 · 本地唤醒和原助手切换");
    review_local_asr_error(-ENOENT);local_settings_poll(NULL);assert(review_local_displayed_error()==-ENOENT&&strstr(lv_label_get_text(g_local_settings->status),"离线转写："));
    review_local_asr_fixture(false,0,"");local_settings_poll(NULL);
    lv_dropdown_set_selected(g_local_settings->target,1);lv_obj_send_event(g_local_settings->target,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!glass_local_voice_wake_selected());assert(strstr(lv_label_get_text(g_local_settings->phrase),"原助手"));
    lv_obj_remove_state(g_local_settings->wake,LV_STATE_CHECKED);lv_obj_send_event(g_local_settings->wake,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!glass_local_voice_wake_enabled());
    review_local_config_error(-EIO);lv_dropdown_set_selected(g_local_settings->target,0);lv_obj_send_event(g_local_settings->target,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!glass_local_voice_wake_selected()&&lv_dropdown_get_selected(g_local_settings->target)==1);review_local_config_error(0);
    review_local_fixture(GLV_SPEAKING,"");local_settings_poll(NULL);assert(lv_obj_has_state(g_local_settings->target,LV_STATE_DISABLED));
    lv_dropdown_set_selected(g_local_settings->target,0);lv_obj_send_event(g_local_settings->target,LV_EVENT_VALUE_CHANGED,NULL);assert(!glass_local_voice_wake_selected());
    review_local_fixture(GLV_IDLE,"");glass_local_voice_set_wake(true);glass_local_voice_enable_wake(true);local_settings_poll(NULL);
    review_local_error_fixture(-EIO);local_settings_poll(NULL);assert(review_local_displayed_error()==-EIO);
    glass_settings(NULL);assert(!g_local_settings);glass_local_voice_settings(NULL);panel_hide(NULL);assert(!g_local_settings);
  }
  review_chat_dialog_fixture(true);review_local_woke(false);wake_mini_service();
  assert(g_local_ui&&review_voice_start_count()==0);panel_hide(NULL);
  review_local_woke(true);wake_mini_service();assert(!g_local_ui&&review_voice_start_count()==0);
  glass_local_voice_enable_wake(false);review_local_woke(false);wake_mini_service();assert(!g_local_ui&&review_voice_start_count()==0);
  glass_local_voice_enable_wake(true);
  review_local_fixture(GLV_IDLE,"");review_local_asr_fixture(true,0,"");unsigned listened=review_local_listens();
  review_local_woke(false);wake_mini_service();assert(g_local_ui&&review_local_listens()==listened+1&&review_voice_start_count()==0);
  wake_mini_service();assert(review_local_listens()==listened+1);panel_hide(NULL);
  fputs("\n]\n",review_manifest);fclose(review_manifest);puts("LOCAL_VOICE_UI_OK themes/keyboard-no-cloud/local-mic/finish/level/transcript/page-lifecycle/wake-once/40-reopen");return 0;
}
