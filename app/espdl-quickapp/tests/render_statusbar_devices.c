/* All registered categories, using production QuickJS/LVGL and offline I/O. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
#include "all_device_preview.h"
extern double qpk_review_number(const char *source);
extern void qpk_review_root_area(lv_area_t *area);

static FILE *bounds_file;
static unsigned bounds_count;
static unsigned icon_checks;
static unsigned check_visible_icons(lv_obj_t *object,uint32_t expected)
{
  if(!object||!lv_obj_is_visible(object))return 0;
  unsigned found=0;
  if(lv_obj_has_class(object,&lv_label_class))
    {
      const unsigned char *text=(const unsigned char *)lv_label_get_text(object);
      const lv_font_t *font=lv_obj_get_style_text_font(object,LV_PART_MAIN);
      while(*text)
        {
          uint32_t code=*text++;
          if(code>=0xc0&&code<0xe0){code=(code&31)<<6;assert(*text);code|=*text++&63;}
          else if(code>=0xe0&&code<0xf0){code=(code&15)<<12;assert(text[0]&&text[1]);code|=(text[0]&63)<<6;code|=text[1]&63;text+=2;}
          else if(code>=0xf0){assert(text[0]&&text[1]&&text[2]);text+=3;continue;}
          if(code>=0xe000&&code<=0xf8ff)
            {
              lv_font_glyph_dsc_t glyph={0};
              if(!lv_font_get_glyph_dsc(font,&glyph,code,0)||glyph.is_placeholder){fprintf(stderr,"Missing icon U+%04X\n",code);assert(false);}
              if(code==expected)assert(lv_obj_get_content_height(object)>=font->line_height);
              icon_checks++;if(code==expected)found++;
            }
        }
    }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)found+=check_visible_icons(lv_obj_get_child(object,i),expected);
  return found;
}
static void save_card_bounds(const char *type,const char *view)
{
  lv_area_t root;qpk_review_root_area(&root);
  int x=root.x1+(int)qpk_review_number("__row.x");
  int y=root.y1+(int)qpk_review_number("__row.y");
  int w=(int)qpk_review_number("__row.w"),h=(int)qpk_review_number("__row.h");
  assert(x>=0&&y>=0&&x+w<=1024&&y+h<=600);
  fprintf(bounds_file,"%s{\"type\":\"%s\",\"theme\":\"%s\",\"view\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
    bounds_count++?",\n":"",type,g_desktop.light_theme?"light":"dark",view,x,y,w,h);
}

#ifndef UNIFIED_DEVICE_NO_MAIN
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font);assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  snprintf(path,sizeof(path),"%s/card-bounds.json",review_out);bounds_file=fopen(path,"w");assert(bounds_file);fputs("[\n",bounds_file);
  for(unsigned theme=0;theme<2;theme++)for(unsigned i=0;i<sizeof(all_device_cases)/sizeof(all_device_cases[0]);i++)
    {
      const struct all_device_case_s *sample=&all_device_cases[i];
      qpk_review_hass_fixture(true);panel_hide(NULL);g_desktop.light_theme=theme==0;glass_rebuild();glass_homeassistant(NULL);
      char js[14000];
      int written=snprintf(js,sizeof(js),"globalThis.__originalGet=system.homeAssistantService.get;"
        "system.homeAssistantService.callService=system.homeAssistantService.control=()=>{throw Error('preview must never send commands')};"
        "globalThis.__haServices=%s;"
        "system.homeAssistantService.get=r=>{if(r!=='services')return __originalGet(r);const id=__haId++;__haPending.push({requestId:id,busy:false,done:true,status:200,error:0,body:JSON.stringify(__haServices)});return id;};"
        "__haStates=%s;ESPHomeHA.refresh();for(let i=0;i<70;i++)__reviewTick();"
        "globalThis.__row=ESPHomeHA.snapshot().layoutPages[0][0];"
        "if(ESPHomeHA.snapshot().deviceCards.length!==1||!ESPHomeHA.snapshot().deviceCards[0].commonProfile)throw Error('preview profile mismatch');",
        all_device_services,sample->states);
      assert(written>0&&written<(int)sizeof(js));assert(qpk_review_eval(js)==0);
      snprintf(path,sizeof(path),"device-%s-small",sample->type);review_capture(display,path,sample->name);save_card_bounds(sample->type,"small");
      check_visible_icons(g_desktop.screen,0);
      int x=(int)qpk_review_number("__row.x+__row.w/2"),y=(int)qpk_review_number("__row.y+22");
      qpk_review_click_at(x,y); /* The card body deliberately has no hit target. */
      assert(qpk_review_eval("for(let i=0;i<15;i++)__reviewTick();if(ESPHomeHA.snapshot().detailOpen)throw Error('body opens detail')")==0);
      y=(int)qpk_review_number("__row.y+__row.h-24");
      assert(qpk_review_click_at(x,y)==0);
      assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailAdvanced)throw Error('detail missing')")==0);
      snprintf(path,sizeof(path),"device-%s-detail",sample->type);review_capture(display,path,sample->name);
      unsigned cameras=check_visible_icons(g_desktop.screen,0xe002);
      if(!strcmp(sample->type,"camera")||!strcmp(sample->type,"video-doorbell"))assert(cameras>0);
      panel_hide(NULL);
    }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  fputs("\n]\n",bounds_file);fclose(bounds_file);
  printf("UNIFIED_DEVICES_NATIVE_OK: %u captures, 59 categories\n",review_count);return 0;
}
#endif
