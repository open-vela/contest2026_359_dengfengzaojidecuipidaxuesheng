/* SPDX-License-Identifier: Apache-2.0 */
/* Actual LVGL renderer and finite animation/lifetime regression. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glass_icons.c"
#include <lvgl/src/misc/cache/lv_image_cache.h>
static uint8_t framebuffer[1024*600*2];
static uint8_t drawbuffer[1024*80*2];
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
  for(int y=area->y1;y<=area->y2;y++){
    memcpy(framebuffer+(y*1024+area->x1)*2,pixels,(area->x2-area->x1+1)*2);
    pixels+=(area->x2-area->x1+1)*2;
  }
  lv_display_flush_ready(display);
}
static void capture(const char *dir,const char *name)
{
  lv_refr_now(NULL);char path[512];snprintf(path,sizeof(path),"%s/%s.rgb565",dir,name);
  FILE *f=fopen(path,"wb");assert(f);assert(fwrite(framebuffer,1,sizeof(framebuffer),f)==sizeof(framebuffer));fclose(f);
}
static lv_obj_t *symbol(lv_obj_t *parent,unsigned code,int size)
{
  char text[]={(char)(0xe0|(code>>12)),(char)(0x80|((code>>6)&63)),(char)(0x80|(code&63)),0};
  lv_obj_t *obj=glass_icon_create(parent);assert(obj);
  lv_obj_set_style_text_font(obj,glass_icon_font(size),0);lv_label_set_text(obj,text);return obj;
}
static void advance(unsigned ms)
{for(unsigned i=0;i<ms;i+=40){lv_tick_inc(40);lv_timer_handler();}}
static unsigned active(void)
{unsigned n=0;for(unsigned i=0;i<12;i++)n+=icon_active[i]!=NULL;return n;}
static void no_work_color(const struct icon_symbol *spec)
{
  if(spec->code<0xe100||spec->code>=0xe13b)return;
  for(unsigned i=0;i<spec->count;i++)if(icon_layers[spec->first+i].flags&16384)return;
  unsigned color=spec->tint;
  unsigned packed=(((color>>16)&255)>>3)<<11|(((color>>8)&255)>>2)<<5|((color&255)>>3);
  for(unsigned i=0;i<sizeof(framebuffer);i+=2)assert((framebuffer[i]|(unsigned)framebuffer[i+1]<<8)!=packed);
}
int main(int argc,char **argv)
{
  assert(argc==2);lv_init();
  /* Match P4's bounded image-cache configuration. All 59 families exceed
   * this capacity together, exercising real LVGL cache eviction as well. */
  lv_image_cache_resize(262144,true);assert(lv_image_cache_is_enabled());
  lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,drawbuffer,NULL,sizeof(drawbuffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display,flush);lv_obj_t *screen=lv_screen_active();
  lv_obj_set_style_bg_color(screen,lv_color_hex(0xf7f9fb),0);
  assert(sizeof(icon_symbols)/sizeof(icon_symbols[0])==59);
  lv_obj_t *diagnostic=symbol(screen,0xe11f,64);
  char report[256];
  assert(glass_icon_diagnose(diagnostic,report,sizeof(report))>0);
  assert(strstr(report,"failed=0"));
  assert(!strstr(report,"coverage=0\n"));
  lv_obj_delete(diagnostic);
  /* Appliance PUA codepoints belong to the icon widget, even when a global
   * reading font replaces its text face and has no icon fallback. */
  lv_obj_t *foreign_font=symbol(screen,0xe11f,64);
  lv_obj_set_style_text_font(foreign_font,&lv_font_montserrat_24,0);
  lv_obj_set_size(foreign_font,64,68);
  assert(icon_lookup(foreign_font));
  lv_obj_update_layout(foreign_font);
  assert(glass_icon_diagnose(foreign_font,report,sizeof(report))>0);
  assert(strstr(report,"px=64")&&strstr(report,"lookup=1"));
  lv_refr_now(NULL);
  uint8_t *foreign_idle=malloc(sizeof(framebuffer));assert(foreign_idle);
  memcpy(foreign_idle,framebuffer,sizeof(framebuffer));
  glass_icon_set_running(foreign_font,true);advance(400);lv_refr_now(NULL);
  assert(((struct icon_label *)foreign_font)->running);
  assert(memcmp(foreign_idle,framebuffer,sizeof(framebuffer)));
  free(foreign_idle);glass_icon_set_running(foreign_font,false);
  lv_obj_delete(foreign_font);
  puts("HOUSEHOLD_FOREIGN_FONT_OK: visible running light without icon-font fallback");
  /* System icons remain visible font glyphs. Neither explicit API calls nor
   * clicks/value changes may register a timer or change their pixels. */
  const unsigned static_codes[]={0xe002,0xe005,0xe009,0xe00f,0xf013,0xf1eb,0xf0f3};
  for(unsigned i=0;i<sizeof(static_codes)/sizeof(static_codes[0]);i++){
    lv_obj_t *button=lv_button_create(screen);
    lv_obj_set_pos(button,16,16);lv_obj_set_size(button,96,96);
    lv_obj_t *item=symbol(button,static_codes[i],32);
    assert(!icon_lookup(item));lv_refr_now(NULL);
    uint8_t *before=malloc(sizeof(framebuffer));assert(before);
    memcpy(before,framebuffer,sizeof(framebuffer));
    glass_icon_play(item);glass_icon_set_running(item,true);
    lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
    lv_obj_send_event(button,LV_EVENT_VALUE_CHANGED,NULL);
    advance(240);lv_refr_now(NULL);
    assert(active()==0&&!((struct icon_label *)item)->running);
    assert(!memcmp(before,framebuffer,sizeof(framebuffer)));
    for(unsigned slot=0;slot<64;slot++)assert(!icon_running[slot]);
    for(unsigned slot=0;slot<128;slot++)assert(!icon_pending[slot]);
    free(before);lv_obj_delete(button);
  }
  printf("SYSTEM_ICONS_STATIC_OK: no icon motion or running registrations\n");
  lv_obj_t *objects[sizeof(icon_symbols)/sizeof(icon_symbols[0])];
  glass_icon_set_reduced(true);
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++){
    objects[i]=symbol(screen,icon_symbols[i].code,32);
    lv_obj_set_pos(objects[i],22+(i%15)*67,6+(i/15)*49);
    lv_obj_set_style_text_color(objects[i],lv_color_hex(0x20213a),0);
    lv_obj_t *number=lv_label_create(screen);char text[12];snprintf(text,sizeof(text),"%04x",icon_symbols[i].code);
    lv_label_set_text(number,text);lv_obj_set_pos(number,20+(i%15)*67,39+(i/15)*49);
    lv_obj_set_style_text_color(number,lv_color_hex(0x697386),0);
    lv_obj_set_style_text_font(number,&lv_font_montserrat_14,0);
    assert(lv_obj_has_class(objects[i],&lv_label_class));
    assert(icon_lookup(objects[i]));
  }
  lv_obj_update_layout(screen);capture(argv[1],"atlas-light");
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)glass_icon_set_active(objects[i],true);
  capture(argv[1],"atlas-active");
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)glass_icon_set_active(objects[i],false);
  lv_obj_set_style_bg_color(screen,lv_color_hex(0x171b29),0);
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)
    lv_obj_set_style_text_color(objects[i],lv_color_hex(0xe8edf8),0);
  capture(argv[1],"atlas-dark");
  glass_icon_set_reduced(false);
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)glass_icon_play(objects[i]);
  assert(active()==12);advance(200);assert(active()<=12);
  glass_icon_set_reduced(true);assert(active()==0);
  glass_icon_set_reduced(false);
  /* Every glyph gets a turn even when a page exceeds the 12-animation cap. */
  bool seen[sizeof(objects)/sizeof(objects[0])];memset(seen,0,sizeof(seen));
  for(unsigned base=0;base<sizeof(objects)/sizeof(objects[0]);base+=100){
    for(unsigned i=base;i<base+100&&i<sizeof(objects)/sizeof(objects[0]);i++)glass_icon_play(objects[i]);
    for(unsigned tick=0;tick<180;tick++){
    for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)
      if(lv_anim_get(objects[i],icon_step))seen[i]=true;
    assert(active()<=12);advance(40);
    }
  }
  for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);i++)assert(seen[i]);
  assert(active()==0);for(unsigned i=0;i<128;i++)assert(!icon_pending[i]);
  advance(1000);assert(active()==0); /* Finite gestures never restart at idle. */
  lv_obj_clean(screen);assert(active()==0);
  lv_obj_t *panel=lv_obj_create(screen);lv_obj_set_pos(panel,0,0);lv_obj_set_size(panel,1024,600);
  lv_obj_set_style_bg_color(panel,lv_color_hex(0xf7f9fb),0);
  lv_obj_set_style_border_width(panel,0,0);lv_obj_set_style_pad_all(panel,0,0);
  unsigned codes[]={0xe108,0xe12e,0xe11f,0xe10e,0xe10c,0xe137,0xe105,0xe119,0xe101,0xe10a,0xe12a,0xe134};
  const char *names[]={"Camera","Speaker","Desk light","Fan","Curtains","Washer",
                       "Purifier","Humidifier","Air conditioner","Coffee","Pressure cooker","Vacuum"};
  lv_obj_t *demo[12],*captions[12];
  for(unsigned i=0;i<12;i++){
    demo[i]=symbol(panel,codes[i],64);
    lv_obj_set_pos(demo[i],96+(i%4)*240,45+(i/4)*180);
    lv_obj_set_style_text_color(demo[i],lv_color_hex(0x20213a),0);
    lv_obj_t *caption=lv_label_create(panel);lv_label_set_text(caption,names[i]);
    captions[i]=caption;
    lv_obj_set_pos(caption,64+(i%4)*240,125+(i/4)*180);lv_obj_set_width(caption,128);
    lv_obj_set_style_text_font(caption,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(caption,lv_color_hex(0x697386),0);
  }
  lv_obj_update_layout(screen);lv_refr_now(NULL);advance(80);assert(active()==0);
  /* Page creation, hover/focus, selection and a cancelled press are quiet.
   * Only an actual click/value change activates functional icon motion. */
  lv_obj_send_event(panel,LV_EVENT_HOVER_OVER,NULL);
  lv_obj_send_event(panel,LV_EVENT_FOCUSED,NULL);
  lv_obj_send_event(panel,LV_EVENT_PRESSED,NULL);
  lv_obj_send_event(panel,LV_EVENT_PRESS_LOST,NULL);
  glass_icon_set_active(panel,true);advance(80);assert(active()==0);
  glass_icon_set_active(panel,false);
  lv_obj_send_event(panel,LV_EVENT_DEFOCUSED,NULL);
  lv_obj_send_event(panel,LV_EVENT_HOVER_LEAVE,NULL);
  /* All six AnimatedIcon states are reachable through native control events.
   * Reduced motion keeps the selected/pressed endpoint without any timer. */
  glass_icon_set_reduced(true);
  assert(icon_state(demo[0])==ICON_NORMAL);
  lv_obj_send_event(panel,LV_EVENT_HOVER_OVER,NULL);
  assert(icon_state(demo[0])==ICON_POINTER_OVER);
  lv_obj_send_event(panel,LV_EVENT_PRESSED,NULL);
  assert(icon_state(demo[0])==ICON_PRESSED);
  assert(!lv_obj_has_state(panel,LV_STATE_CHECKED));
  glass_icon_set_active(panel,true);
  assert(icon_state(demo[0])==ICON_PRESSED_SELECTED);
  capture(argv[1],"state-pressed-selected");
  lv_obj_send_event(panel,LV_EVENT_RELEASED,NULL);
  assert(icon_state(demo[0])==ICON_POINTER_OVER_SELECTED);
  lv_obj_send_event(panel,LV_EVENT_HOVER_LEAVE,NULL);
  assert(icon_state(demo[0])==ICON_SELECTED);
  capture(argv[1],"state-selected");
  glass_icon_set_active(panel,false);
  assert(icon_state(demo[0])==ICON_NORMAL);assert(active()==0);
  lv_obj_send_event(panel,LV_EVENT_PRESSED,NULL);
  lv_obj_send_event(panel,LV_EVENT_PRESS_LOST,NULL);
  assert(icon_state(demo[0])==ICON_NORMAL);
  glass_icon_set_reduced(false);
  glass_icon_play_children(panel);assert(active()==12);
  advance(240);assert(((struct icon_label *)demo[0])->phase>0);
  capture(argv[1],"motion-240");
  lv_label_set_text(demo[0],GLASS_ICON_HOME_WASHER);assert(icon_lookup(demo[0])->code==0xe137);
  advance(240);capture(argv[1],"motion-480");
  advance(520);assert(active()==0);
  lv_label_set_text(demo[0],GLASS_ICON_HOME_CAMERA);capture(argv[1],"motion-rest");
  lv_obj_send_event(panel,LV_EVENT_CLICKED,NULL);assert(active()==12);
  lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);advance(80);assert(active()==0);
  for(unsigned i=0;i<12;i++)assert(!lv_anim_get(demo[i],icon_step));
  lv_obj_remove_flag(panel,LV_OBJ_FLAG_HIDDEN);
  lv_obj_send_event(panel,LV_EVENT_VALUE_CHANGED,NULL);assert(active()==12);
  glass_icon_set_reduced(true);assert(active()==0);glass_icon_set_reduced(false);
  lv_label_set_text(demo[0],"Icon + text");assert(!icon_lookup(demo[0]));
  lv_label_set_text(demo[0],"");assert(!icon_lookup(demo[0]));
  lv_label_set_text(demo[0],"\xf0\x9f\x8e\xa4");assert(!icon_lookup(demo[0]));
  lv_label_set_text(demo[0],GLASS_ICON_HOME_CAMERA);glass_icon_play_children(panel);
  for(unsigned frame=0;frame<36;frame++){
    char name[40];snprintf(name,sizeof(name),"frame-%02u",frame);
    advance(40);capture(argv[1],name);
  }
  assert(active()==0);
  /* New semantic gestures must visibly change pixels and restore them exactly. */
  const unsigned extra[]={0xe112,0xe12b,0xe10c,0xe10e,0xe100,0xe11d,
                          0xe106,0xe135,0xe119,0xe129,0xe132,0xe130};
  const char *extra_names[]={"Bin lid","Printer","Curtains","Fan","Diffuser","Juicer",
                             "Airer","Doorbell","Humidifier","Plant","Towel rack","Wall switch"};
  for(unsigned i=0;i<12;i++){
    unsigned code=extra[i];
    char text[]={(char)(0xe0|(code>>12)),(char)(0x80|((code>>6)&63)),(char)(0x80|(code&63)),0};
    lv_label_set_text(demo[i],text);lv_label_set_text(captions[i],extra_names[i]);
  }
  uint8_t *rest=malloc(sizeof(framebuffer));assert(rest);
  const unsigned sizes[]={14,16,20,24,32,64};
  for(unsigned size=0;size<6;size++){
    for(unsigned i=0;i<12;i++)lv_obj_set_style_text_font(demo[i],glass_icon_font(sizes[size]),0);
    lv_obj_update_layout(screen);lv_refr_now(NULL);
    memcpy(rest,framebuffer,sizeof(framebuffer));
    for(unsigned i=0;i<12;i++){
      icon_step(demo[i],440);lv_refr_now(NULL);
      if(memcmp(rest,framebuffer,sizeof(framebuffer))==0)
        fprintf(stderr,"No visible motion: %s at %u px\n",extra_names[i],sizes[size]);
      assert(memcmp(rest,framebuffer,sizeof(framebuffer))!=0);
      icon_step(demo[i],1000);lv_refr_now(NULL);
      assert(memcmp(rest,framebuffer,sizeof(framebuffer))==0);
    }
  }
  free(rest);
  glass_icon_play_children(panel);
  for(unsigned frame=0;frame<24;frame++){
    char name[40];snprintf(name,sizeof(name),"semantic-%02u",frame);
    advance(40);capture(argv[1],name);
  }
  assert(active()==0);
  glass_icon_play_children(panel);
  glass_icon_set_reduced(true);
  const unsigned run_codes[]={0xe11f,0xe137,0xe10e,0xe105,0xe119,0xe101,0xe12e,0xe108,0xe100,0xe11e,0xe10b,0xe12a};
  const char *run_names[]={"Desk light","Washer drum","Fan blades","Air purifier","Humidifier","Air conditioner","Speaker","Camera","Diffuser","Kettle","Cooker","Pressure cooker"};
  for(unsigned i=0;i<12;i++){
    unsigned code=run_codes[i];
    char text[]={(char)(0xe0|(code>>12)),(char)(0x80|((code>>6)&63)),(char)(0x80|(code&63)),0};
    lv_label_set_text(demo[i],text);lv_label_set_text(captions[i],run_names[i]);
    lv_obj_set_style_text_font(demo[i],glass_icon_font(64),0);
  }
  lv_obj_update_layout(screen);capture(argv[1],"running-off");
  glass_icon_set_reduced(false);
  for(unsigned i=0;i<12;i++)glass_icon_set_running(demo[i],true);
  for(unsigned frame=0;frame<64;frame++){
    char name[40];snprintf(name,sizeof(name),"running-%02u",frame);
    advance(40);capture(argv[1],name);
  }
  assert(!icon_run_idle);
  /* Rotating a washer drum must leave its shell and control buttons fixed. */
  for(unsigned i=0;i<12;i++)if(i!=1)glass_icon_set_running(demo[i],false);
  advance(800);lv_refr_now(NULL);
  rest=malloc(sizeof(framebuffer));assert(rest);memcpy(rest,framebuffer,sizeof(framebuffer));
  advance(320);lv_refr_now(NULL);lv_area_t wa;lv_obj_get_content_coords(demo[1],&wa);
  unsigned changed=0;
  for(int py=0;py<600;py++)for(int px=0;px<1024;px++){
    unsigned offset=(py*1024+px)*2;
    if(memcmp(rest+offset,framebuffer+offset,2)){
      changed++;
      assert(px>=wa.x1+16&&px<=wa.x1+48);
      assert(py>=wa.y1+2+21&&py<=wa.y1+2+54);
    }
  }
  assert(changed>0);free(rest);
  glass_icon_set_reduced(true);assert(icon_run_idle);
  assert(((struct icon_label *)demo[1])->running);
  glass_icon_set_reduced(false);advance(80);assert(!icon_run_idle);
  lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);advance(80);assert(icon_run_idle);
  lv_obj_remove_flag(panel,LV_OBJ_FLAG_HIDDEN);lv_refr_now(NULL);advance(80);assert(!icon_run_idle);
  glass_icon_set_running(demo[1],false);advance(80);assert(icon_run_idle);
  glass_icon_play_children(panel);
  /* Deleting a page also removes queued icons, never leaving stale pointers. */
  for(unsigned i=0;i<20;i++)symbol(panel,0xe108,24);
  lv_refr_now(NULL);
  glass_icon_play_children(panel);
  unsigned pending=0;for(unsigned i=0;i<128;i++)pending+=icon_pending[i]!=NULL;
  assert(pending>=20);
  lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);advance(80);assert(active()==0);
  for(unsigned i=0;i<128;i++)assert(!icon_pending[i]);
  lv_obj_remove_flag(panel,LV_OBJ_FLAG_HIDDEN);glass_icon_play_children(panel);
  lv_obj_delete(panel);assert(active()==0);advance(1000);
  for(unsigned i=0;i<64;i++)assert(!icon_running[i]);
  for(unsigned i=0;i<128;i++)assert(!icon_pending[i]);
  /* Every independent symbol has an operating mechanism. Verify pixels at
   * every supported size, stop restoration, and a stable reduced endpoint. */
  lv_obj_clean(screen);lv_obj_set_style_bg_color(screen,lv_color_hex(0xf7f9fb),0);
  lv_obj_t *probe=symbol(screen,icon_symbols[0].code,64);
  lv_obj_set_pos(probe,100,100);lv_obj_set_style_text_color(probe,lv_color_hex(0x20213a),0);
  uint8_t *off=malloc(sizeof(framebuffer)),*on=malloc(sizeof(framebuffer));assert(off&&on);
  unsigned family_checks=0;
  for(unsigned index=0;index<sizeof(icon_symbols)/sizeof(icon_symbols[0]);index++){
    const struct icon_symbol *spec=&icon_symbols[index];assert(spec->run_kind);
    unsigned code=spec->code;
    char text[]={(char)(0xe0|(code>>12)),(char)(0x80|((code>>6)&63)),(char)(0x80|(code&63)),0};
    lv_label_set_text(probe,text);
    for(unsigned si=0;si<6;si++){
      lv_obj_set_style_text_font(probe,glass_icon_font(sizes[si]),0);
      if(code>=0xe100&&code<0xe13b){lv_font_glyph_dsc_t actual={0};assert(lv_font_get_glyph_dsc(glass_icon_font(sizes[si]),&actual,code,0)&&!actual.is_placeholder);}
      glass_icon_set_reduced(true);lv_obj_update_layout(screen);lv_refr_now(NULL);
      no_work_color(spec);
      memcpy(off,framebuffer,sizeof(framebuffer));glass_icon_set_reduced(false);
      icon_step(probe,440);lv_refr_now(NULL);
      if(!memcmp(off,framebuffer,sizeof(framebuffer)))fprintf(stderr,"Gesture unchanged: %x size %u\n",code,sizes[si]);
      assert(memcmp(off,framebuffer,sizeof(framebuffer)));
      no_work_color(spec);
      icon_step(probe,1000);glass_icon_set_running(probe,true);advance(800);
      ((struct icon_label *)probe)->run_phase=0;lv_obj_invalidate(probe);lv_refr_now(NULL);
      memcpy(on,framebuffer,sizeof(framebuffer));bool changed=false;
      const unsigned samples[]={125,250,350,750,840};
      for(unsigned sample=0;sample<5;sample++){
        ((struct icon_label *)probe)->run_phase=samples[sample];lv_obj_invalidate(probe);lv_refr_now(NULL);
        changed|=memcmp(on,framebuffer,sizeof(framebuffer))!=0;
      }
      if(!changed)fprintf(stderr,"Operating unchanged: %x size %u\n",code,sizes[si]);
      assert(changed);
      glass_icon_set_reduced(true);lv_refr_now(NULL);memcpy(on,framebuffer,sizeof(framebuffer));
      advance(800);lv_refr_now(NULL);assert(!memcmp(on,framebuffer,sizeof(framebuffer)));
      glass_icon_set_running(probe,false);advance(600);lv_refr_now(NULL);
      assert(!memcmp(off,framebuffer,sizeof(framebuffer)));family_checks++;
      no_work_color(spec);
    }
  }
  free(off);free(on);lv_obj_delete(probe);
  /* Page through the whole family within the 64-registration budget. These
   * are native animated atlases, not an SVG approximation. */
  for(unsigned page=0;page<(sizeof(icon_symbols)/sizeof(icon_symbols[0])+29)/30;page++){
    lv_obj_clean(screen);glass_icon_set_reduced(true);
    for(unsigned cell=0;cell<30;cell++){
      unsigned index=page*30+cell;if(index>=sizeof(icon_symbols)/sizeof(icon_symbols[0]))break;
      lv_obj_t *item=symbol(screen,icon_symbols[index].code,64);
      lv_obj_set_pos(item,52+(cell%6)*168,8+(cell/6)*118);
      lv_obj_set_style_text_color(item,lv_color_hex(0x20213a),0);
      lv_obj_t *caption=lv_label_create(screen);char text[24];snprintf(text,sizeof(text),"U+%04X",icon_symbols[index].code);
      lv_label_set_text(caption,text);lv_obj_set_pos(caption,45+(cell%6)*168,82+(cell/6)*118);
      lv_obj_set_style_text_font(caption,&lv_font_montserrat_14,0);
      glass_icon_set_running(item,true);
    }
    lv_obj_update_layout(screen);lv_refr_now(NULL);glass_icon_set_reduced(false);
    for(unsigned frame=0;frame<40;frame++){
      char name[48];snprintf(name,sizeof(name),"family-%u-%02u",page,frame);
      advance(80);capture(argv[1],name);
    }
  }
  lv_obj_clean(screen);advance(80);
  for(unsigned i=0;i<64;i++)assert(!icon_running[i]);
  printf("ICON_FAMILY_OK: %u symbols x 6 sizes = %u visible gestures, operating states, stop and reduced endpoints\n",
         (unsigned)(sizeof(icon_symbols)/sizeof(icon_symbols[0])),family_checks);
  lv_display_delete(display);lv_deinit();
  printf("ICON_MOTION_OK: %u outlines; semantic pixel changes/restoration, cap, live glyph, reduced, hidden, deletion, text fallback\n",
         (unsigned)(sizeof(icon_symbols)/sizeof(icon_symbols[0])));
  return 0;
}
