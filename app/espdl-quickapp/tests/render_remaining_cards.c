/* Offline LVGL review of the production card and detail layouts. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
extern double qpk_review_number(const char *source);

static unsigned camera_text_lines(lv_obj_t *object,const char *text)
{
  if(!object||!lv_obj_is_visible(object))return 0;
  unsigned found=0;
  if(lv_obj_has_class(object,&lv_label_class)&&!strcmp(lv_label_get_text(object),text))
    {
      const lv_font_t *face=lv_obj_get_style_text_font(object,LV_PART_MAIN);
      int height=lv_obj_get_content_height(object);
      printf("CAMERA_TEXT_LINE %s: content=%d font=%u\n",text,height,face->line_height);
      fflush(stdout);assert(height>=face->line_height);found++;
    }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)
    found+=camera_text_lines(lv_obj_get_child(object,i),text);
  return found;
}

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
  g_desktop.screen=lv_screen_active();lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,600,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  const char *types[]={"air-monitor","washer","camera","smoke-sensor","plant-monitor","water-purifier","treadmill","heater"};
  for(unsigned theme=0;theme<2;theme++)for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++)
    {
      qpk_review_hass_fixture(true);panel_hide(NULL);g_desktop.light_theme=theme==0;glass_rebuild();glass_homeassistant(NULL);
      char js[2800];
      snprintf(js,sizeof(js),"globalThis.__originalGet=system.homeAssistantService.get;system.homeAssistantService.callService=()=>{throw Error('offline render issued a command')};"
        "system.homeAssistantService.get=r=>{if(r!=='services')return __originalGet(r);const id=__haId++;__haPending.push({requestId:id,busy:false,done:true,status:200,error:0,body:JSON.stringify([{domain:'climate',services:{set_temperature:{},set_hvac_mode:{},turn_off:{},turn_on:{}}}])});return id;};"
        "__haStates=[JSON.parse(JSON.stringify(__miExamples.find(s=>s.attributes.device_type==='%s')))];"
        "if('%s'==='washer'){__haStates[0].state='washing';__haStates[0].attributes.miot_properties['washer.status']='washing';__haStates[0].attributes.miot_properties['washer.left-time']='00:20';}"
        "if('%s'==='heater')__haStates=[{entity_id:'climate.heater',state:'heat',attributes:{friendly_name:'客厅取暖器',device_type:'heater',supported_features:385,current_temperature:20.5,temperature:23,min_temp:16,max_temp:30,hvac_modes:['heat','off']}}];"
        "if(__haStates[0].attributes.device_type==='smoke-sensor')__haStates=[{entity_id:'binary_sensor.smoke',state:'on',attributes:{friendly_name:'烟雾报警器',device_type:'smoke-sensor',device_class:'smoke',battery_level:68}}];"
        "ESPHomeHA.refresh();for(let i=0;i<60;i++)__reviewTick();globalThis.__row=ESPHomeHA.snapshot().layoutPages[0][0];"
        "if(!ESPHomeHA.snapshot().deviceCards[0].commonProfile)throw Error('missing profile');",types[i],types[i],types[i]);
      assert(qpk_review_eval(js)==0);
      snprintf(path,sizeof(path),"remaining-%s-small",types[i]);review_capture(display,path,types[i]);
      int x=(int)qpk_review_number("__row.x+__row.w/2"),y=(int)qpk_review_number("__row.y+22");
      assert(qpk_review_click_at(x,y)==0);
      assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailAdvanced)throw Error('detail not open')")==0);
      snprintf(path,sizeof(path),"remaining-%s-detail",types[i]);review_capture(display,path,types[i]);
      assert(qpk_review_click_at(192,30)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
      assert(qpk_review_click_at(900,30)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();__row=ESPHomeHA.snapshot().layoutPages[0][0]")==0);
      y=(int)qpk_review_number("__row.y+__row.h-32");assert(qpk_review_click_at(x,y)==0);
      assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()")==0);
      assert(qpk_review_click_at(900,30)==0);
      assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick();if(ESPHomeHA.snapshot().deviceCards[0].cardSize!=='large')throw Error('large card not selected')")==0);
      snprintf(path,sizeof(path),"remaining-%s-large",types[i]);review_capture(display,path,types[i]);
      if(!strcmp(types[i],"camera"))
        {
          const char *labels[]={"摄像机","查看设备详情","设备状态","暂无预览画面","空闲"};
          for(unsigned j=0;j<sizeof(labels)/sizeof(labels[0]);j++)
            {
              assert(camera_text_lines(g_desktop.screen,labels[j])>0);
            }
          assert(camera_text_lines(g_desktop.screen,"\xee\x80\xa9")>0);
        }
      panel_hide(NULL);
    }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("REMAINING_CARDS_NATIVE_OK: 48 production LVGL captures");return 0;
}
