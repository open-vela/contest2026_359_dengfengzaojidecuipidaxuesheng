/* SPDX-License-Identifier: Apache-2.0 */
#define MUSIC_FULL_NO_MAIN
#include "render_music_full.c"
#include <unistd.h>

static void select_font(unsigned index)
{
  if(!g_glass.global_font_dropdown)glass_settings(NULL);
  assert(g_glass.global_font_dropdown&&!g_music_ui.full);
  lv_dropdown_set_selected(g_glass.global_font_dropdown,index);
  lv_obj_send_event(g_glass.global_font_dropdown,LV_EVENT_VALUE_CHANGED,NULL);
}
static void wait_font(unsigned index,bool failure)
{
  for(unsigned i=0;i<6000;i++){
    lv_tick_inc(100);lv_timer_handler();
    int state=glass_lyric_font_state(index);
    if(state==3||state==4){
      music_full_fonts_poll();
      assert(!global_font_timer);
      assert(state==(failure?4:3));
      if(!failure)assert(global_font_selected==index);
      return;
    }
    usleep(10000);
  }
  assert(!"Font validation timed out");
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);size_t size=ftell(file);rewind(file);
  void *base=malloc(size);assert(base&&fread(base,1,size,file)==size);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  const unsigned sizes[]={16,20,28,48,72};lv_font_t *defaults[5];
  for(unsigned i=0;i<5;i++){
    defaults[i]=lv_tiny_ttf_create_data(base,size,sizes[i]);assert(defaults[i]);
    defaults[i]->fallback=glass_icon_font(i==2?24:sizes[i]);
  }
  g_desktop.font16=defaults[0];g_desktop.font20=defaults[1];g_desktop.font28=defaults[2];
  g_glass.font48=defaults[3];g_glass.music_font72=defaults[4];
  g_glass.font72=lv_tiny_ttf_create_data(base,size,128);assert(g_glass.font72);
  glass_home_fonts_init(base,size);
  glass_global_fonts_init();
  const void *embedded[5];for(unsigned i=0;i<5;i++)embedded[i]=defaults[i]->dsc;
  g_glass.reduced_motion=true;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  if(getenv("GLOBAL_FONT_RESTORE_EXPECT")){
    unsigned expected=(unsigned)strtoul(getenv("GLOBAL_FONT_RESTORE_EXPECT"),NULL,10);
    glass_sd_font_start();
    for(unsigned i=0;i<6000;i++){
      glass_global_fonts_poll();unsigned saved;
      if(glass_font_choice_load(&saved)&&global_font_selected==expected){
        assert(saved==expected&&global_font_requested==expected);
        if(expected)assert(g_desktop.font20->dsc==glass_lyric_font_get(expected,20)->dsc);
        puts("GLOBAL_FONT_RESTORE_OK");return 0;
      }
      usleep(10000);
    }
    assert(!"saved global font did not restore");
  }
  lv_obj_t *hidden=lv_obj_create(g_desktop.screen);lv_obj_add_flag(hidden,LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *existing=lv_label_create(hidden);lv_label_set_text(existing,"全局文字 ABC");
  lv_obj_set_style_text_font(existing,home_font(12),0);
  lv_obj_t *input=lv_textarea_create(hidden);lv_textarea_set_text(input,"正在编辑的草稿");
  lv_obj_set_style_text_font(input,zh_font(20),0);
  const lv_font_t *old_small=home_font(12),*old_input=zh_font(20),*old_ha=hass_qpk_font(20);
  const lv_font_t *bold=NULL;
  struct music_now_playing now={.revision=40,.track_id=100,.position_ms=192800,
    .duration_ms=600000,.volume=40,.state=MUSIC_PLAYING,.lyrics_state=MUSIC_LYRICS_READY,
    .can_skip=true,.lyrics_started=true};
  strcpy(now.name,"城市与晚风");strcpy(now.artist,"声动 · 字体试听");
  review_music_fixture(2);review_music_now_fixture(&now);review_lyrics_fixture(192,48);
  review_lyrics_text_fixture("把今天交给温柔的晚风");
  review_lyrics_translation_fixture("Give today to the gentle evening breeze");
  glass_rebuild();
  assert(glass_lyric_font_count()==7&&glass_lyric_font_state(0)==3);
  assert(glass_lyric_font_request(999)==-EINVAL&&!glass_lyric_font_get(999,28));
  glass_music(NULL);music_full_open(NULL);assert(g_music_ui.full);
  if(getenv("GLOBAL_FONT_IO_FAILURE")){
    select_font(1);wait_font(1,false);
    char damaged[1024];snprintf(damaged,sizeof(damaged),"%s/VelaLyricSans-Regular.ttf",getenv("LYRIC_FONT_TEST_ROOT"));
    FILE *file=fopen(damaged,"wb");assert(file);fclose(file);
    for(unsigned cp=0x4e00;cp<0x9000&&global_font_selected;cp++){
      lv_font_glyph_dsc_t glyph={0};lv_font_get_glyph_dsc(zh_font(20),&glyph,cp,0);
      glass_global_fonts_poll();
    }
    assert(global_font_selected==0&&global_font_requested==0);
    assert(g_desktop.font20->dsc==embedded[1]);
    puts("GLOBAL_FONT_IO_FALLBACK_OK");return 0;
  }
  if(getenv("LYRIC_FONT_EXPECT_FAILURE")){
    select_font(1);wait_font(1,true);
    assert(global_font_selected==0&&global_font_requested==0&&!glass_lyric_font_get(1,28));
    assert(music_lyric_font(28)==defaults[2]);
    panel_hide(NULL);assert(!g_glass.global_font_dropdown);
    puts("LYRIC_FONT_REJECTED: unavailable pack retained default");return 0;
  }
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  lv_font_t *retained[6][5]={0};
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;
    glass_rebuild();
    if(theme){panel_hide(NULL);glass_music(NULL);music_full_open(NULL);}
    for(unsigned family=0;family<glass_lyric_font_count();family++){
      select_font(family);wait_font(family,false);
      assert(lv_dropdown_get_selected(g_glass.global_font_dropdown)==family);
      assert(!strcmp(lv_dropdown_get_text(g_glass.global_font_dropdown),glass_lyric_font_name(family)));
      assert(lv_obj_get_style_text_font(existing,0)==old_small&&home_font(12)==old_small);
      assert(lv_obj_get_style_text_font(input,0)==old_input&&zh_font(20)==old_input);
      assert(hass_qpk_font(20)==old_ha);
      /* The chat face may first be created after a custom family is active. */
      if(family&&!bold){
        bold=chat_ui_bold();assert(bold->fallback&&bold->fallback->fallback==zh_font(20));
      }
      if(bold)assert(chat_ui_bold()==bold);
      assert(!strcmp(lv_textarea_get_text(input),"正在编辑的草稿"));
      if(!family){
        review_capture(display,"lyric-font-settings","设置 · 个性化 · 全局字体");
        lv_dropdown_open(g_glass.global_font_dropdown);review_capture(display,"lyric-font-menu","设置 · 七种全局字体");
        lv_dropdown_close(g_glass.global_font_dropdown);
      }
      if(family==1)review_capture(display,"global-font-settings-custom","设置 · 全局字体即时更新");
      panel_hide(NULL);assert(!g_glass.global_font_dropdown);
      glass_music(NULL);music_full_open(NULL);assert(g_music_ui.full);
      struct music_full_ui *full=g_music_ui.full;unsigned slot=48%MUSIC_LYRICS_WINDOW;
      assert(!review_callback_find(full->root,glass_global_font_selected));
      for(unsigned i=0;i<5;i++){
        const lv_font_t *font=music_lyric_font(sizes[i]);assert(font);
        if(family){
          assert(font==defaults[i]&&font->dsc==glass_lyric_font_get(family,sizes[i])->dsc);
          if(!theme)retained[family-1][i]=glass_lyric_font_get(family,sizes[i]);
          else assert(glass_lyric_font_get(family,sizes[i])==retained[family-1][i]);
          lv_font_glyph_dsc_t glyph={0};
          assert(lv_font_get_glyph_dsc(font,&glyph,0x97f3,0)&&!glyph.is_placeholder&&glyph.resolved_font==font);
          assert(font->fallback&&font->fallback->dsc==embedded[i]);
        }else assert(font==defaults[i]&&font->dsc==embedded[i]);
      }
      assert(lv_obj_get_style_text_font(full->labels[slot],0)==music_lyric_font(28));
      assert(lv_obj_get_style_text_font(full->translations[slot],0)==music_lyric_font(16));
      lyric_arc_bounds(full);
      for(unsigned i=0;i<global_font_view_count;i++){
        unsigned pixels=global_font_views[i].pixels;
        lv_font_t *target=global_font_views[i].target;
        if(family)assert(target->dsc==glass_lyric_font_get(family,pixels)->dsc);
        else assert(target->dsc==global_font_views[i].embedded.dsc);
        unsigned depth=0;const lv_font_t *fallback=target;
        while(fallback){assert(++depth<10);fallback=fallback->fallback;}
      }
      char id[80];snprintf(id,sizeof(id),"lyric-font-%u-ordinary",family);
      review_capture(display,id,glass_lyric_font_name(family));
      music_full_mode(NULL);
      for(unsigned style=0;style<3;style++){
        full->pv.style_index=style;full->pv.valid=false;music_pv_refresh(&full->pv);
        assert(full->pv.valid&&full->pv.count>0);
        for(unsigned i=0;i<full->pv.count;i++){
          const lv_font_t *font=lv_obj_get_style_text_font(full->pv.text[i],0);
          bool matched=false;for(unsigned s=0;s<5;s++)if(font==music_lyric_font(sizes[s]))matched=true;
          assert(matched);
        }
        snprintf(id,sizeof(id),"lyric-font-%u-pv-%u",family,style);
        review_capture(display,id,glass_lyric_font_name(family));
      }
      music_full_mode(NULL);
      if(family==1){
        panel_hide(NULL);review_capture(display,"global-font-home","桌面 · 全局字体");
        qpk_review_hass_fixture(true);glass_homeassistant(NULL);
        assert(qpk_review_eval("for(let i=0;i<50;i++)__reviewTick() ")==0);
        review_capture(display,"global-font-ha","家居快应用 · 全局字体");
        glass_chat_open(NULL);review_capture(display,"global-font-chat","工作台 · 全局字体");
        glass_music(NULL);music_full_open(NULL);
      }
    }
    glass_settings(NULL);unsigned objects=lock_object_count(g_desktop.current_card);
    for(unsigned i=0;i<60;i++){
      unsigned family=i%7;select_font(family);
      assert(global_font_selected==family&&lock_object_count(g_desktop.current_card)==objects);
      if(family)for(unsigned s=0;s<5;s++)assert(glass_lyric_font_get(family,sizes[s])==retained[family-1][s]);
    }
    panel_hide(NULL);assert(!g_glass.global_font_dropdown);
    glass_settings(NULL);
    assert(lv_dropdown_get_selected(g_glass.global_font_dropdown)==global_font_selected);
  }
  panel_hide(NULL);review_lyrics_translation_fixture(NULL);
  assert(glass_font_choice_save(global_font_selected)==0);
  for(unsigned i=0;i<1000;i++){
    int result=glass_font_choice_save_status();
    if(result){assert(result==1);break;}
    usleep(10000);assert(i<999);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("GLOBAL_FONTS_OK: settings-only selector; 6 actual SD faces + default; 12 sizes; stable desktop/HA/lyric pointers; source glyphs; fallback chain; both themes; half-arc geometry; 3 PV effects; settings deletion safety; 120 cached switches; reopen retention");
  return 0;
}
