/* SPDX-License-Identifier: Apache-2.0 */
#define main unused_all_pages_main
#include "render_all_ui.c"
#undef main
extern double qpk_review_number(const char *source);
static lv_indev_state_t motion_pointer_state;
static lv_point_t motion_pointer_point;
static void motion_pointer_read(lv_indev_t *input,lv_indev_data_t *data)
{LV_UNUSED(input);data->state=motion_pointer_state;data->point=motion_pointer_point;}
static void motion_pointer(lv_indev_t *input,int x,int y,bool pressed)
{motion_pointer_point=(lv_point_t){x,y};motion_pointer_state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;lv_obj_update_layout(lv_layer_top());lv_indev_read(input);}
static void fluid_frames(lv_display_t *display,const char *name,unsigned count)
{
  char path[1024];
  for(unsigned i=0;i<count;i++){
    for(unsigned ms=0;ms<GLASS_MOTION_TICK;ms++){lv_tick_inc(1);lv_timer_handler();}
    lv_refr_now(display);
    snprintf(path,sizeof(path),"%s/%s-%s-%03u.rgb565",review_out,g_desktop.light_theme?"light":"dark",name,i);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(review_fb,1,sizeof(review_fb),f)==sizeof(review_fb));fclose(f);
  }
}
static void fluid_page(unsigned page)
{
  char js[256];snprintf(js,sizeof(js),"globalThis.__fluidDot=ESPHomeHA.snapshot().pageIndicator.slots.find(s=>s.page===%u)",page);
  assert(qpk_review_eval(js)==0);
  assert(qpk_review_click_at((int)qpk_review_number("__fluidDot.x+12"),(int)qpk_review_number("__fluidDot.y+20"))==0);
  assert(qpk_review_eval("for(let i=0;i<12;i++)__reviewTick()")==0);
}
static void motion_frames(lv_display_t *display,const char *name,unsigned count)
{
  char path[1024];
  for(unsigned i=0;i<count;i++){
    lv_tick_inc(20);lv_timer_handler();lv_refr_now(display);
    snprintf(path,sizeof(path),"%s/%s-%s-%03u.rgb565",review_out,
             g_desktop.light_theme?"light":"dark",name,i);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(review_fb,1,sizeof(review_fb),f)==sizeof(review_fb));fclose(f);
  }
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];FILE *f=fopen(argv[1],"rb");assert(f);
  fseek(f,0,SEEK_END);size_t bytes=ftell(f);rewind(f);void *data=malloc(bytes);
  assert(data&&fread(data,1,bytes,f)==bytes);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(data,bytes,16);
  g_desktop.font20=lv_tiny_ttf_create_data(data,bytes,20);
  g_desktop.font28=lv_tiny_ttf_create_data(data,bytes,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(data,bytes,48);glass_home_fonts_init(data,bytes);
  g_desktop.screen=lv_screen_active();lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,600,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;g_glass.reduced_motion=false;
    glass_rebuild();motion_frames(display,"home-enter",18);
    review_capture(display,"motion-home","首页静态布局");
    lv_obj_t *button=g_status_ui.island;assert(button&&lv_obj_is_visible(button));
    lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);motion_frames(display,"press",6);
    lv_obj_send_event(button,LV_EVENT_RELEASED,NULL);motion_frames(display,"release",12);
    glass_apps(NULL);motion_frames(display,"apps-enter",16);
    assert(g_desktop.apps_page&&g_desktop.current_card);
    assert(lv_obj_get_style_translate_y(g_desktop.current_card,0)==0);
    review_capture(display,"motion-apps","应用页静态布局");
    assert(!lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_CLICKABLE));
    unsigned top_children=lv_obj_get_child_count(lv_layer_top());
    lv_obj_send_event(g_status_ui.bar,LV_EVENT_CLICKED,NULL);
    assert(lv_obj_get_child_count(lv_layer_top())==top_children);
    lv_obj_t *apps_card=g_apps_card,*apps_pager=g_apps_pager;
    glass_apps_page(NULL,1);assert(g_apps_card==apps_card&&g_apps_pager==apps_pager);
    fluid_frames(display,"apps-page-next",18);
    glass_apps_page(NULL,0);assert(g_apps_card==apps_card&&g_apps_pager==apps_pager);
    fluid_frames(display,"apps-page-back",18);
    /* Real LVGL pointer dispatch: a short drag restores immediately; a committed drag closes on dispatch. */
    if(theme==0)glass_edge_back_create();
    lv_indev_t *pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,display);lv_indev_set_read_cb(pointer,motion_pointer_read);
    motion_pointer(pointer,5,300,true);motion_pointer(pointer,45,300,true);
    assert(g_edge_tracking&&lv_obj_get_style_translate_x(apps_card,0)>0);
    fluid_frames(display,"edge-follow",2);motion_pointer(pointer,45,300,false);
    assert(lv_obj_get_style_translate_x(apps_card,0)==0);fluid_frames(display,"edge-spring-back",1);
    motion_pointer(pointer,5,300,true);motion_pointer(pointer,100,300,true);
    motion_pointer(pointer,100,300,false);fluid_frames(display,"edge-commit",1);
    assert(!g_desktop.current_card&&!g_apps_card&&!g_apps_pager);
    glass_apps(NULL);fluid_frames(display,"apps-reopen",12);
    apps_card=g_apps_card;
    motion_pointer(pointer,1019,300,true);motion_pointer(pointer,979,300,true);
    assert(g_edge_tracking&&lv_obj_get_style_translate_x(apps_card,0)<0);
    motion_pointer(pointer,979,300,false);fluid_frames(display,"edge-right-cancel",12);
    assert(lv_obj_get_style_translate_x(apps_card,0)==0);
    motion_pointer(pointer,5,300,true);motion_pointer(pointer,45,300,true);
    motion_pointer(pointer,45,360,true);assert(!g_edge_tracking);
    motion_pointer(pointer,45,360,false);fluid_frames(display,"edge-vertical-cancel",12);
    assert(lv_obj_get_style_translate_x(apps_card,0)==0);
    glass_motion_set_reduced(true);
    motion_pointer(pointer,5,300,true);motion_pointer(pointer,45,300,true);
    assert(lv_obj_get_style_translate_x(apps_card,0)==0);
    motion_pointer(pointer,45,300,false);glass_motion_set_reduced(false);
    motion_pointer(pointer,5,300,true);motion_pointer(pointer,45,300,true);
    panel_hide(NULL);motion_pointer(pointer,45,300,false);assert(!g_edge_page);
    lv_indev_delete(pointer);glass_apps(NULL);fluid_frames(display,"apps-final",12);
    show_toast("设置已保存");motion_frames(display,"toast-enter",14);
    assert(g_desktop.toast);
    motion_frames(display,"toast-hold",70);motion_frames(display,"toast-exit",10);
    assert(!g_desktop.toast&&!g_desktop.toast_timer);
    qpk_show_dialog("动效测试 · 确认后立即关闭");motion_frames(display,"dialog-enter",16);
    assert(g_desktop.dialog);dialog_cancel();
    /* Destruction is immediate even during entry; no delayed permission action. */
    qpk_show_dialog("中断测试");dialog_cancel();motion_frames(display,"dialog-cancel",2);
    panel_hide(NULL);launch_builtin_2048_qapp(NULL);motion_frames(display,"app-enter",16);
    assert(g_desktop.current_card&&lv_obj_get_style_translate_x(g_desktop.current_card,0)==0);
    review_capture(display,"motion-quickapp","快应用静态布局");
    assert(qpk_review_eval("var __motionClicks=0;var __motionButton=ui.button('FX',20,80,160,44,function(){__motionClicks++;ui.setColor(__motionButton,0x339966)},0xebe6f4)")==0);
    lv_obj_update_layout(g_desktop.current_card);
    lv_obj_t *runtime_button=review_find(g_desktop.current_card,"FX");assert(runtime_button);
    lv_obj_send_event(runtime_button,LV_EVENT_PRESSED,NULL);motion_frames(display,"runtime-press",3);
    lv_obj_send_event(runtime_button,LV_EVENT_RELEASED,NULL);lv_obj_send_event(runtime_button,LV_EVENT_CLICKED,NULL);
    assert(qpk_review_eval("if(__motionClicks!==1)throw Error('motion delayed click')")==0);
    motion_frames(display,"runtime-release",12);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(runtime_button,0))==lv_color_to_u32(lv_color_hex(0x339966)));
    panel_hide(NULL);qpk_review_hass_fixture(true);glass_homeassistant(NULL);
    assert(qpk_review_eval(
      "system.homeAssistantService.callService=system.homeAssistantService.control=()=>{throw Error('no commands in motion preview')};"
      "__haStates=Array.from({length:18},(_,i)=>({entity_id:'light.fluid_'+i,state:'on',attributes:{friendly_name:'灯具 '+(i+1),brightness:200,supported_color_modes:i===0?['rgb','color_temp']:['brightness'],rgb_color:[255,150,46],color_mode:'rgb',color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500,supported_features:4,effect_list:['Day','Night','Relax'],effect:'Day'}}));"
      "ESPHomeHA.refresh();for(let i=0;i<70;i++)__reviewTick();"
      "if(!ESPHomeHA.snapshot().waterfall&&ESPHomeHA.snapshot().pageIndicator.total!==3)throw Error('expected three pages')")==0);
    fluid_frames(display,"fluid-ha-settle",20);
    if(qpk_review_number("!!ESPHomeHA.snapshot().waterfall")){
      assert(qpk_review_eval("if(ESPHomeHA.snapshot().pageIndicator.visible)throw Error('unexpected list pager');ui.scrollTo(ESPHomeHA.snapshot().waterfall.viewport,240,false)")==0);
      fluid_frames(display,"waterfall-list",18);
      assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick();ui.scrollTo(ESPHomeHA.snapshot().waterfall.viewport,0,false)")==0);
      fluid_frames(display,"waterfall-list-return",18);
      assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()")==0);
    }else{
    unsigned pages[]={1,2,1,0};
    const char *names[]={"pager-middle-forward","pager-right","pager-middle-back","pager-left"};
    lv_obj_t *pager_pill=NULL;
    for(unsigned step=0;step<4;step++){
      fluid_page(pages[step]);
      unsigned colors=0,geometry=0;
      for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object){
        if(g_motion_jobs[i].kind==MOTION_RECT){
          geometry++;
          assert(g_motion_jobs[i].target[2]==20);
          if(!pager_pill)pager_pill=g_motion_jobs[i].object;
          assert(pager_pill==g_motion_jobs[i].object);
          assert(g_motion_jobs[i].target[3]==6);
        }
        if(g_motion_jobs[i].kind==MOTION_COLOR){
          colors++;
        }
      }
      assert(colors==0&&geometry==1); /* The same pill moves through every node. */
      bool paging=false;
      for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_BATCH)paging=true;
      assert(!paging);
      fluid_frames(display,names[step],18);
      for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)assert(!g_motion_jobs[i].object);
    }
    fluid_page(1);fluid_frames(display,"pager-quick-forward",2);
    struct glass_motion_job *pill_job=NULL;
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==pager_pill&&g_motion_jobs[i].kind==MOTION_RECT)pill_job=&g_motion_jobs[i];
    assert(pill_job);int64_t pill_position=pill_job->position[0],pill_velocity=pill_job->velocity[0];
    fluid_page(0);assert(pill_job->start[0]==pill_position&&pill_job->initial_velocity[0]==pill_velocity);
    assert(pill_job->position[0]==pill_position);fluid_frames(display,"pager-quick-return",18);
    glass_motion_set_reduced(true);fluid_page(2);lv_obj_update_layout(pager_pill);
    assert(lv_obj_get_x(pager_pill)==(int)qpk_review_number("ESPHomeHA.snapshot().pageIndicator.slots.find(s=>s.active).x+2"));
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)assert(!g_motion_jobs[i].object);
    glass_motion_set_reduced(false);fluid_page(0);fluid_frames(display,"pager-reduced-return",18);
    }
    assert(qpk_review_eval("if(ESPHomeHA.snapshot().roomPage!==0)throw Error('wrong pager target')")==0);
    review_capture(display,"fluid-ha","流畅分页 · 最终布局");
    assert(qpk_review_eval("if(!ESPHomeHA.openDevice('http://192.168.1.88:8123','light.fluid_0'))throw Error('open lamp');for(let i=0;i<50;i++)__reviewTick()")==0);
    bool batch=false;for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_BATCH)batch=true;
    assert(!batch);fluid_frames(display,"detail-open",18);
    assert(qpk_review_eval("__haStates[0].attributes.effect='Night';ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    bool optionFade=false;for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_COLOR)optionFade=true;
    assert(optionFade);fluid_frames(display,"mode-selection",18);
    assert(qpk_review_eval("__haStates[0].attributes.brightness=40;ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    bool range=false;for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_RANGE)range=true;
    assert(range);fluid_frames(display,"slider-state",18);
    assert(qpk_review_eval("__haStates[0].attributes.rgb_color=[127,172,255];ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    fluid_frames(display,"wheel-preset",18);
    assert(qpk_review_eval("__haStates[0].state='off';ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    bool fade=false;for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_COLOR)fade=true;
    assert(fade);fluid_frames(display,"power-state",18);
    assert(qpk_review_click_at(192,30)==0);
    assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()")==0);
    fluid_frames(display,"detail-return",18);
    review_capture(display,"expanded-ha","补全交互 · 最终布局");
    int nav_top=qpk_review_number("ui.getSize().height<480?64:104");
    assert(qpk_review_click_at(80,nav_top+3*52+22)==0);
    assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick();if(ESPHomeHA.snapshot().nav!=='logs')throw Error('logs navigation failed')")==0);
    int cursor_y=qpk_review_number("ESPHomeHA.snapshot().sidebarCursor.y");
    assert(cursor_y==nav_top+3*52); /* The current sidebar snaps directly. */
    fluid_frames(display,"sidebar-logs",18);
    assert(qpk_review_click_at(80,nav_top+22)==0);assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick();if(ESPHomeHA.snapshot().nav!=='overview')throw Error('overview navigation failed')")==0);
    fluid_frames(display,"sidebar-home",18);
    assert(qpk_review_eval(
      "__haStates=[{entity_id:'fan.purifier',state:'on',attributes:{friendly_name:'净化器',device_type:'air-purifier',supported_features:57,percentage:40,preset_modes:['auto','quiet'],preset_mode:'auto',miot_properties:{'environment.pm2.5-density':12,'air-purifier.filter-life-level':83}}}];"
      "ESPHomeHA.refresh();for(let i=0;i<50;i++)__reviewTick();"
      "ESPHomeHA.openDevice('http://192.168.1.88:8123','fan.purifier');for(let i=0;i<50;i++)__reviewTick()")==0);
    fluid_frames(display,"filter-before",18);
    assert(qpk_review_eval("__haStates[0].attributes.miot_properties['air-purifier.filter-life-level']=40;ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    lv_obj_t *meter=NULL;
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_RECT&&g_motion_jobs[i].target[3]==8)meter=g_motion_jobs[i].object;
    assert(meter);fluid_frames(display,"filter-update",18);
    assert(qpk_review_eval("__haStates[0].attributes.miot_properties['air-purifier.filter-life-level']=0;ESPHomeHA.refresh();for(let i=0;i<40;i++)__reviewTick()")==0);
    assert(lv_obj_has_flag(meter,LV_OBJ_FLAG_HIDDEN));fluid_frames(display,"filter-zero",18);
    review_capture(display,"filter-final","滤芯余量 · 零值端点");
    /* Conversion callbacks may remove/reuse handles: reject stale objects. */
    assert(qpk_review_eval(
      "var __p=ui.panel(300,100,30,10,0);var __q=ui.panel(350,100,30,10,0);var __caught=false;"
      "try{ui.present([__p,{valueOf(){ui.remove(__p);return __q}}],12)}catch(e){__caught=true}"
      "if(!__caught)throw Error('stale presentation accepted');ui.remove(__q);"
      "__p=ui.panel(300,100,30,10,0);__caught=false;"
      "try{ui.range(__p,{valueOf(){ui.remove(__p);return 128}},0xffa64e,0x303640,true)}catch(e){__caught=true}"
      "if(!__caught)throw Error('stale range accepted');"
      "__caught=false;try{ui.present(Array(129).fill(1),12)}catch(e){__caught=true}"
      "if(!__caught)throw Error('unbounded presentation accepted')")==0);
    panel_hide(NULL);glass_settings(NULL);motion_frames(display,"settings-enter",16);
    glass_motion_set_reduced(true);
    assert(g_motion_reduced);
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)assert(!g_motion_jobs[i].object);
    panel_hide(NULL);glass_apps(NULL);lv_tick_inc(20);lv_timer_handler();
    assert(lv_obj_get_style_translate_y(g_desktop.current_card,0)==0);
    review_capture(display,"motion-reduced","减少动态效果");panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  glass_motion_set_reduced(true);lv_obj_delete(g_desktop.screen);
  if(g_motion_timer)lv_timer_delete(g_motion_timer);g_motion_timer=NULL;
  lv_tiny_ttf_destroy(g_desktop.font16);lv_tiny_ttf_destroy(g_desktop.font20);lv_tiny_ttf_destroy(g_desktop.font28);
  lv_tiny_ttf_destroy(g_glass.font48);
  lv_tiny_ttf_destroy(g_home_font12);lv_tiny_ttf_destroy(g_home_font14);lv_tiny_ttf_destroy(g_home_font32);
  lv_tiny_ttf_destroy(g_home_font34);lv_tiny_ttf_destroy(g_home_font40);lv_tiny_ttf_destroy(g_home_font64);
  lv_display_delete(display);lv_deinit();free(data);
  puts("INTERACTION_UI_OK: passive status strip; sidebar/HA paging/options/filter; native edge pointer follow/cancel/commit/destruction; pooled apps/pager; HA detail/range/wheel/power/return; stale conversions; both themes; press/release; apps/settings/dialog/toast/quickapp; reduced motion; immediate cancellation");
  return 0;
}
