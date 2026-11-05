/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
extern void review_pinyin_fixture(unsigned);
extern void review_pinyin_loading_fixture(bool);
extern unsigned review_pinyin_prepare_count(void);
extern void review_chat_dialog_fixture(bool),review_voice_text_fixture(const char *);
extern double qpk_review_number(const char *);
static unsigned failures;
#define CHECK(condition,message) do {if(!(condition)){fprintf(stderr,"IME_BUG: %s\n",message);failures++;}}while(0)
static void ime_test_key(lv_obj_t *keyboard,const char *text)
{
  for(unsigned i=0;i<128;i++){
    const char *key=lv_buttonmatrix_get_button_text(keyboard,i);if(!key)break;
    if(!strcmp(key,text)){
      lv_buttonmatrix_set_selected_button(keyboard,i);
      lv_obj_send_event(keyboard,LV_EVENT_VALUE_CHANGED,NULL);return;
    }}
  assert(!"key missing");
}
static void ime_test_type(lv_obj_t *keyboard,const char *text)
{for(;*text;text++){char key[2]={*text,0};ime_test_key(keyboard,key);}}
static lv_point_t ime_pointer;
static lv_indev_state_t ime_pressed;
static void ime_test_read(lv_indev_t *input,lv_indev_data_t *data)
{LV_UNUSED(input);data->point=ime_pointer;data->state=ime_pressed;}
static void ime_test_tap(lv_indev_t *input,lv_obj_t *button)
{
  lv_obj_update_layout(g_desktop.screen);lv_area_t area;lv_obj_get_coords(button,&area);
  ime_pointer=(lv_point_t){(area.x1+area.x2)/2,(area.y1+area.y2)/2};
  ime_pressed=LV_INDEV_STATE_PRESSED;lv_indev_read(input);lv_tick_inc(25);
  ime_pressed=LV_INDEV_STATE_RELEASED;lv_indev_read(input);lv_tick_inc(25);
}
static lv_obj_t *ime_test_keyboard(lv_obj_t *root)
{
  if(lv_obj_check_type(root,&lv_keyboard_class))return root;
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
    lv_obj_t *found=ime_test_keyboard(lv_obj_get_child(root,i));if(found)return found;
  }
  return NULL;
}
static int ime_test_luma(lv_color_t color)
{uint32_t rgb=lv_color_to_u32(color);return (((rgb>>16)&255)*77+((rgb>>8)&255)*150+(rgb&255)*29)>>8;}
static void ime_test_contrast(lv_obj_t *keyboard)
{
  const lv_state_t states[]={LV_STATE_DEFAULT,LV_STATE_CHECKED,LV_STATE_PRESSED};
  for(unsigned i=0;i<3;i++){
    lv_obj_add_state(keyboard,states[i]);
    int text=ime_test_luma(lv_obj_get_style_text_color(keyboard,LV_PART_ITEMS));
    int background=ime_test_luma(lv_obj_get_style_bg_color(keyboard,LV_PART_ITEMS));
    CHECK(abs(text-background)>100,"normal, function and pressed key labels must all remain readable");
    lv_obj_remove_state(keyboard,states[i]);
  }
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];FILE *file=fopen(argv[1],"rb");assert(file);
  fseek(file,0,SEEK_END);size_t size=ftell(file);rewind(file);void *font=malloc(size);assert(font&&fread(font,1,size,file)==size);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);g_glass.reduced_motion=true;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  lv_indev_t *pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_display(pointer,display);lv_indev_set_read_cb(pointer,ime_test_read);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    panel_hide(NULL);g_desktop.light_theme=theme==0;glass_rebuild();
    lv_obj_t *card=qapp_page_prepare("输入法检查");assert(card);qapp_page_commit(card);
    lv_obj_t *first=lv_textarea_create(card),*second=lv_textarea_create(card);
    unsigned prepares=review_pinyin_prepare_count();
    lv_obj_set_pos(first,32,40);lv_obj_set_size(first,460,60);glass_input_style(first);
    CHECK(review_pinyin_prepare_count()==prepares+1,"input field must prepare the dictionary before a keyboard exists");
    lv_obj_t *dropdown=lv_dropdown_create(card);glass_input_style(dropdown);
    CHECK(review_pinyin_prepare_count()==prepares+1,"styling a dropdown must not preload the dictionary");
    lv_obj_delete(dropdown);
    lv_obj_set_pos(second,516,40);lv_obj_set_size(second,460,60);glass_input_style(second);lv_textarea_set_password_mode(second,true);
    lv_obj_t *keyboard=lv_keyboard_create(card);glass_keyboard_style(keyboard);lv_obj_set_size(keyboard,824,240);lv_obj_align(keyboard,LV_ALIGN_TOP_LEFT,100,260);
    lv_keyboard_set_textarea(keyboard,first);glass_ime_attach(keyboard,zh_font(20),true,theme_primary(),theme_surface(),theme_accent());
    lv_obj_t *bar=lv_obj_get_child(card,-1);lv_obj_update_layout(card);review_pinyin_fixture(40);
    review_pinyin_loading_fixture(true);ime_test_type(keyboard,"ni");ime_test_key(keyboard," ");
    CHECK(!lv_textarea_get_text(first)[0],"loading must preserve composition on space instead of committing raw Pinyin");
    review_pinyin_loading_fixture(false);lv_tick_inc(100);lv_timer_handler();
    CHECK(review_find(bar,"候选1"),"loaded decoder must refresh candidates without another keystroke");
    glass_ime_commit(keyboard);lv_textarea_set_text(first,"");ime_test_type(keyboard,"ni");
    ime_test_contrast(keyboard);
    review_capture(display,"ime-chinese","输入法 · 中文候选");
    lv_obj_t *candidate=review_find(bar,"候选1");assert(candidate);
    lv_keyboard_set_textarea(keyboard,second);lv_obj_send_event(candidate,LV_EVENT_CLICKED,NULL);
    CHECK(!lv_textarea_get_text(second)[0],"old Chinese candidate entered the new password field");
    ime_test_key(keyboard,"a");
    CHECK(!strcmp(lv_textarea_get_text(second),"a"),"password must accept raw Latin keys only");
    CHECK(review_find(bar,"EN")&&!review_find(bar,"候选1"),"password bar must show EN without Chinese candidates");
    review_capture(display,"ime-password","输入法 · 密码输入");
    CHECK(!glass_ime_start_voice(keyboard),"password field cannot start voice recognition");
    lv_obj_set_width(keyboard,640);lv_obj_set_pos(keyboard,170,250);lv_obj_update_layout(card);
    CHECK(lv_obj_get_width(bar)==640,"candidate bar did not resize with keyboard");
    CHECK(lv_obj_get_x(bar)==lv_obj_get_x(keyboard)&&lv_obj_get_y(bar)==lv_obj_get_y(keyboard)-48,"candidate bar did not follow keyboard position");
    review_capture(display,"ime-resized","输入法 · 键盘移动与缩放");
    glass_ime_set_target(keyboard,first);ime_test_type(keyboard,"ni");
    candidate=review_find(bar,"候选1");assert(candidate);
    lv_obj_add_state(keyboard,LV_STATE_DISABLED);ime_test_tap(pointer,candidate);
    CHECK(!lv_textarea_get_text(first)[0],"disabled keyboard must block candidate selection");
    lv_obj_remove_state(keyboard,LV_STATE_DISABLED);
    candidate=review_find(bar,"候选1");assert(candidate);ime_test_tap(pointer,candidate);
    CHECK(!strcmp(lv_textarea_get_text(first),"候选1"),"candidate above keyboard must remain touchable");
    const unsigned widths[]={320,640,744,824,1024};
    for(unsigned w=0;w<5;w++){
      lv_obj_set_width(keyboard,widths[w]);lv_obj_set_x(keyboard,0);lv_obj_update_layout(card);
      ime_test_type(keyboard,"ni");lv_obj_update_layout(card);
      for(unsigned i=0;i<lv_obj_get_child_count(bar);i++){
        lv_obj_t *child=lv_obj_get_child(bar,i);if(!lv_obj_is_visible(child))continue;
        lv_area_t a,b;lv_obj_get_coords(child,&a);lv_obj_get_coords(bar,&b);
        CHECK(a.x1>=b.x1&&a.x2<=b.x2&&a.y1>=b.y1&&a.y2<=b.y2,"candidate controls must fit the resized bar");
      }
      glass_ime_commit(keyboard);
    }
    lv_obj_set_width(keyboard,824);lv_obj_set_x(keyboard,100);lv_obj_update_layout(card);
    review_chat_dialog_fixture(true);lv_textarea_set_text(first,"保留草稿");
    CHECK(glass_ime_start_voice(keyboard),"ordinary field must start voice recognition");
    glass_ime_set_target(keyboard,second);
    review_voice_text_fixture("不应串入密码");lv_tick_inc(100);lv_timer_handler();
    CHECK(!strcmp(lv_textarea_get_text(first),"保留草稿")&&!strcmp(lv_textarea_get_text(second),"a"),"retargeted voice result must not overwrite either field");
    lv_obj_t *transient=lv_textarea_create(card);lv_textarea_set_text(transient,"临时输入");
    glass_ime_set_target(keyboard,transient);CHECK(glass_ime_start_voice(keyboard),"voice starts before target deletion");
    lv_obj_delete(transient);CHECK(!lv_keyboard_get_textarea(keyboard),"deleting the input field must detach the keyboard");
    ime_test_key(keyboard,"a");review_voice_text_fixture("已删除输入框");lv_tick_inc(100);lv_timer_handler();
    glass_ime_set_target(keyboard,first);ime_test_type(keyboard,"ni");
    review_capture(display,"ime-restored","输入法 · 切换与语音结束后恢复");
    panel_hide(NULL);
    for(unsigned i=0;i<20;i++){
      lv_obj_t *root=lv_obj_create(g_desktop.screen),*target=lv_textarea_create(root),*keys=lv_keyboard_create(root);
      lv_keyboard_set_textarea(keys,target);glass_ime_attach(keys,zh_font(20),true,theme_primary(),theme_surface(),theme_accent());
      lv_obj_delete(root);
    }
    review_chat_dialog_fixture(false);
    card=qapp_page_prepare("快应用输入");assert(card);lv_obj_set_style_bg_color(card,lv_color_hex(theme_card()),0);
    const char *script="globalThis.__inputResult=null;prompt.input({title:'输入文字',placeholder:'中文 / English',value:''},value=>__inputResult=value);";
    assert(!qpk_runtime_launch(card,"输入法检查","review.ime","1","builtin:/ime.js",script,strlen(script),zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive));qapp_page_commit(card);
    keyboard=ime_test_keyboard(card);assert(keyboard);lv_obj_update_layout(card);
    CHECK(abs(ime_test_luma(lv_obj_get_style_text_color(keyboard,LV_PART_ITEMS))-ime_test_luma(lv_obj_get_style_bg_color(keyboard,LV_PART_ITEMS)))>128,"quickapp keyboard keys need contrasting text and background");
    ime_test_contrast(keyboard);
    ime_test_type(keyboard,"ni");review_capture(display,"ime-quickapp","输入法 · 快应用弹窗对比度");
    ime_test_key(keyboard,LV_SYMBOL_OK);CHECK(lv_obj_is_valid(keyboard),"first confirmation commits composition without closing dialog");
    ime_test_key(keyboard,LV_SYMBOL_OK);
    CHECK(qpk_review_number("__inputResult==='候选1'"),"second confirmation submits the committed Chinese text");
    CHECK(!ime_test_keyboard(card),"submit must remove keyboard and candidate bar together");panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  if(failures){fprintf(stderr,"IME_CHECK_FAILED: %u\n",failures);return 1;}
  lv_indev_delete(pointer);
  puts("IME_UI_OK: both themes; candidate isolation, literal passwords, disabled guard, actual pointer selection, 5 widths, target deletion, voice retarget, 40 create/delete cycles; QuickJS prompt contrast and two-step confirmation");return 0;
}
