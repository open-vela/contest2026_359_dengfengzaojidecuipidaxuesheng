/* SPDX-License-Identifier: Apache-2.0 */
/* Native label subclass: all label APIs, layout and live text remain valid.
 * Only a single supported icon overrides label drawing. Mixed text, font
 * fallback and layered device silhouettes retain the normal label renderer.
 * UI thread only. Twelve finite animations and one bounded running timer. */
#include <lvgl/src/widgets/label/lv_label_private.h>
#include <lvgl/src/core/lv_obj_class_private.h>
#include <lvgl/src/draw/lv_image_decoder_private.h>
#include <string.h>

struct icon_layer { const lv_image_dsc_t *image; uint16_t flags; uint8_t x,y,delay; uint16_t run_delay; };
struct icon_symbol { uint16_t code,first; uint8_t count,motion,pivot_x,pivot_y; int8_t dx,dy;
  uint8_t run_kind; uint16_t run_period; uint8_t run_x,run_y; int8_t run_dx,run_dy; uint32_t tint; };
#include "glass_material.h"
struct material_mask_source {lv_image_dsc_t image;uint32_t offset,bytes;};
#include "glass_icon_geometry.inc"
#include "glass_material_mask.inc"
struct icon_label { lv_label_t label; uint16_t phase,run_phase;
  uint32_t run_started; bool presented,hovered,pressed,running,run_pending; };
static const lv_obj_class_t glass_icon_class;
static lv_obj_t *icon_active[12];
static lv_obj_t *icon_pending[128];
static bool icon_reduced;
static lv_obj_t *icon_running[64];
static lv_timer_t *icon_run_timer;
static bool icon_run_idle=true;
static unsigned icon_run_cursor;
static void icon_drain(void);

static const struct icon_symbol *icon_lookup(lv_obj_t *obj)
{
  material_mask_register();
  const unsigned char *s=(const unsigned char *)lv_label_get_text(obj);
  if(!s||s[0]<0xe0||s[0]>=0xf0||!s[1]||!s[2]||s[3])return NULL;
  unsigned code=((s[0]&15)<<12)|((s[1]&63)<<6)|(s[2]&63);
  if(code<0xe100||code>=0xe13b)return NULL;
  /* This PUA range is owned by the household icon widget. A selected text
   * face must not disable its drawing or running-state registration. */
  unsigned lo=0,hi=sizeof(icon_symbols)/sizeof(icon_symbols[0]);
  while(lo<hi){unsigned mid=(lo+hi)/2;if(icon_symbols[mid].code<code)lo=mid+1;else hi=mid;}
  return lo<sizeof(icon_symbols)/sizeof(icon_symbols[0])&&icon_symbols[lo].code==code?&icon_symbols[lo]:NULL;
}

static unsigned icon_size(lv_obj_t *obj)
{
  const lv_font_t *font=lv_obj_get_style_text_font(obj,0),*icons=font;
  while(icons&&icons->get_glyph_dsc!=icon_unicode_glyph)icons=icons->fallback;
  if(icons)return icons->line_height-4;
  int height=lv_obj_get_content_height(obj);
  if(height<8)height=font?font->line_height:28;
  unsigned em=(unsigned)(height-4);
  return em>=48?64:em>=28?32:em>=22?24:em>=18?20:em>=15?16:14;
}

/* Read-only diagnostics, called only by the desktop/UI thread. Include
 * unsupported font/class bindings so a missing icon cannot hide its cause. */
int glass_icon_diagnose(lv_obj_t *obj,char *buffer,size_t size)
{
  if(!obj||!lv_obj_has_class(obj,&lv_label_class))return 0;
  const unsigned char *text=(const unsigned char *)lv_label_get_text(obj);
  if(!text||text[0]<0xe0||text[0]>=0xf0||!text[1]||!text[2]||text[3])return 0;
  unsigned code=((text[0]&15)<<12)|((text[1]&63)<<6)|(text[2]&63);
  if(code<0xe100||code>=0xe13b)return 0;
  bool custom=lv_obj_check_type(obj,&glass_icon_class);
  const struct icon_symbol *symbol=icon_lookup(obj);
  bool matched=symbol!=NULL;
  if(!symbol)symbol=&icon_symbols[code-0xe100];
  const lv_font_t *font=lv_obj_get_style_text_font(obj,0);
  unsigned font_height=font?font->line_height:0;
  while(font&&font->get_glyph_dsc!=icon_unicode_glyph)font=font->fallback;
  unsigned pixels=icon_size(obj);
  unsigned index=pixels<=14?0:pixels<=16?1:pixels<=20?2:pixels<=24?3:pixels<=32?4:5;
  unsigned opened=0,failed=0,covered=0;
  for(unsigned i=0;i<symbol->count;i++){
    const lv_image_dsc_t *image=icon_layers[symbol->first+index*symbol->count+i].image;
    lv_image_decoder_dsc_t decoded;
    lv_image_decoder_args_t args={.no_cache=true};
    if(lv_image_decoder_open(&decoded,image,&args)==LV_RESULT_OK){
      opened++;
      if(decoded.decoded){
        const lv_draw_buf_t *buf=decoded.decoded;
        for(unsigned y=0;y<buf->header.h;y++)for(unsigned x=0;x<buf->header.w;x++)
          covered+=buf->data[y*buf->header.stride+x]!=0;
      }
      lv_image_decoder_close(&decoded);
    }else failed++;
  }
  return lv_snprintf(buffer,size,"ICON_STATUS code=%04x custom=%u lookup=%u font_h=%u icon_font=%u px=%u opa=%u recursive_opa=%u running=%u reduced=%u layers=%u opened=%u failed=%u coverage=%u\n",
    code,custom,matched,font_height,font!=NULL,pixels,lv_obj_get_style_text_opa(obj,0),lv_obj_get_style_opa_recursive(obj,0),
    custom?((struct icon_label *)obj)->running:0,icon_reduced,symbol->count,opened,failed,covered);
}

static void icon_run_tick(lv_timer_t *timer)
{
  bool visible=false;unsigned updated=0;
  for(unsigned n=0;n<64;n++){
    unsigned i=(icon_run_cursor+n)%64;lv_obj_t *obj=icon_running[i];
    if(!obj||!lv_obj_is_visible(obj))continue;
    const struct icon_symbol *symbol=icon_lookup(obj);
    if(!symbol||!symbol->run_kind)continue;
    visible=true;
    if(icon_reduced)continue;
    struct icon_label *icon=(struct icon_label *)obj;
    icon->run_phase=(lv_tick_elaps(icon->run_started)%symbol->run_period)*1000/symbol->run_period;
    lv_obj_invalidate(obj);
    if(++updated==12){icon_run_cursor=(i+1)%64;break;}
  }
  if(!visible||icon_reduced){lv_timer_pause(timer);icon_run_idle=true;}
}
static void icon_run_wake(void *object)
{
  struct icon_label *icon=object;icon->run_pending=false;
  if(!icon->running||icon_reduced)return;
  if(!icon_run_timer)icon_run_timer=lv_timer_create(icon_run_tick,40,NULL);
  if(icon_run_timer){lv_timer_resume(icon_run_timer);icon_run_idle=false;}
}
void glass_icon_set_running(lv_obj_t *obj,bool running)
{
  if(!obj||!lv_obj_check_type(obj,&glass_icon_class))return;
  if(running&&!icon_lookup(obj))running=false;
  struct icon_label *icon=(struct icon_label *)obj;
  if(icon->running==running)return;
  icon->running=running;icon->run_phase=0;icon->run_started=lv_tick_get();
  if(running){
    for(unsigned i=0;i<64;i++)if(!icon_running[i]){icon_running[i]=obj;break;}
    icon_run_wake(obj);glass_icon_play(obj);
  }else{
    for(unsigned i=0;i<64;i++)if(icon_running[i]==obj)icon_running[i]=NULL;
    lv_async_call_cancel(icon_run_wake,obj);icon->run_pending=false;
  }
  lv_obj_invalidate(obj);
}

static void icon_release(lv_obj_t *obj)
{for(unsigned i=0;i<12;i++)if(icon_active[i]==obj)icon_active[i]=NULL;}
static void icon_unqueue(lv_obj_t *obj)
{for(unsigned i=0;i<128;i++)if(icon_pending[i]==obj)icon_pending[i]=NULL;}
static void icon_step(void *object,int32_t value)
{
  lv_obj_t *obj=object;
  bool hidden=!lv_obj_is_visible(obj);
  if(hidden){lv_anim_delete(obj,icon_step);value=1000;}
  ((struct icon_label *)obj)->phase=value;
  lv_obj_invalidate(obj);
  if(value==1000){icon_release(obj);if(hidden)icon_drain();}
}
static void icon_done(lv_anim_t *a)
{icon_step(a->var,1000);icon_drain();}
void glass_icon_play(lv_obj_t *obj)
{
  if(!obj||!lv_obj_check_type(obj,&glass_icon_class)||icon_reduced||!icon_lookup(obj))return;
  if(!lv_obj_is_visible(obj))return;
  unsigned slot=12;
  for(unsigned i=0;i<12;i++)if(icon_active[i]==obj)return;else if(!icon_active[i])slot=i;
  if(slot==12){
    for(unsigned i=0;i<128;i++)if(icon_pending[i]==obj)return;
    for(unsigned i=0;i<128;i++)if(!icon_pending[i]){icon_pending[i]=obj;return;}
    return;
  }
  icon_unqueue(obj);
  lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,obj);
  lv_anim_set_values(&a,0,1000);lv_anim_set_duration(&a,520);
  lv_anim_set_exec_cb(&a,icon_step);lv_anim_set_completed_cb(&a,icon_done);
  icon_active[slot]=obj;
  if(!lv_anim_start(&a)){icon_release(obj);((struct icon_label *)obj)->phase=1000;}
}
static void icon_drain(void)
{
  if(icon_reduced)return;
  for(unsigned i=0;i<128;i++)if(icon_pending[i]){
    bool full=true;for(unsigned j=0;j<12;j++)if(!icon_active[j]){full=false;break;}
    if(full)return;
    lv_obj_t *obj=icon_pending[i];icon_pending[i]=NULL;
    glass_icon_play(obj);
  }
}
static void icon_play_tree(lv_obj_t *obj,unsigned depth)
{
  if(!obj||depth>24||!lv_obj_is_visible(obj))return;
  if(lv_obj_check_type(obj,&glass_icon_class)){glass_icon_play(obj);return;}
  for(unsigned i=0;i<lv_obj_get_child_count(obj);i++)icon_play_tree(lv_obj_get_child(obj,i),depth+1);
}
void glass_icon_play_children(lv_obj_t *obj){if(!icon_reduced)icon_play_tree(obj,0);}
void glass_icon_set_reduced(bool reduced)
{
  icon_reduced=reduced;
  for(unsigned i=0;i<64;i++)if(icon_running[i]){
    ((struct icon_label *)icon_running[i])->run_phase=0;
    lv_obj_invalidate(icon_running[i]);
    if(!reduced)icon_run_wake(icon_running[i]);
  }
  if(reduced&&icon_run_timer){lv_timer_pause(icon_run_timer);icon_run_idle=true;}
  if(reduced)memset(icon_pending,0,sizeof(icon_pending));
  if(reduced)for(unsigned i=0;i<12;i++)if(icon_active[i]){
    lv_obj_t *obj=icon_active[i];lv_anim_delete(obj,icon_step);icon_step(obj,1000);
  }
}
void glass_icon_set_active(lv_obj_t *obj,bool active)
{
  if(!obj)return;
  if(active)lv_obj_add_state(obj,LV_STATE_CHECKED);else lv_obj_remove_state(obj,LV_STATE_CHECKED);
}
static bool icon_selected(lv_obj_t *obj)
{
  for(unsigned i=0;obj&&i<4;i++,obj=lv_obj_get_parent(obj))
    if(lv_obj_has_state(obj,LV_STATE_CHECKED))return true;
  return false;
}
/* WinUI AnimatedIcon state vocabulary, implemented with LVGL events.
 * Pointer feedback is visual only; selection remains owned by the control. */
enum icon_visual_state { ICON_NORMAL, ICON_POINTER_OVER, ICON_PRESSED,
  ICON_SELECTED, ICON_POINTER_OVER_SELECTED, ICON_PRESSED_SELECTED };
static enum icon_visual_state icon_state(lv_obj_t *obj)
{
  struct icon_label *icon=(struct icon_label *)obj;
  return (icon_selected(obj)?ICON_SELECTED:ICON_NORMAL)+
         (icon->pressed?2:icon->hovered?1:0);
}
static void icon_pointer_tree(lv_obj_t *obj,lv_event_code_t code,unsigned depth)
{
  if(!obj||depth>24)return;
  if(lv_obj_check_type(obj,&glass_icon_class)){
    if(!icon_lookup(obj))return;
    struct icon_label *icon=(struct icon_label *)obj;
    enum icon_visual_state previous=icon_state(obj);
    if(code==LV_EVENT_HOVER_OVER||code==LV_EVENT_FOCUSED)icon->hovered=true;
    if(code==LV_EVENT_HOVER_LEAVE||code==LV_EVENT_DEFOCUSED)icon->hovered=false;
    if(code==LV_EVENT_PRESSED)icon->pressed=true;
    if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST||code==LV_EVENT_DEFOCUSED)icon->pressed=false;
    /* Hover, focus and cancelled presses only change the static pose.
     * A confirmed click/value change owns the finite activation gesture. */
    if(previous!=icon_state(obj))lv_obj_invalidate(obj);
    return;
  }
  for(unsigned i=0;i<lv_obj_get_child_count(obj);i++)
    icon_pointer_tree(lv_obj_get_child(obj,i),code,depth+1);
}
static void icon_parent_event(lv_event_t *event)
{
  if(lv_event_get_target(event)!=lv_event_get_current_target(event))return;
  lv_event_code_t code=lv_event_get_code(event);
  if(code==LV_EVENT_PRESSED||code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST||
     code==LV_EVENT_HOVER_OVER||code==LV_EVENT_HOVER_LEAVE||
     code==LV_EVENT_FOCUSED||code==LV_EVENT_DEFOCUSED)
    icon_pointer_tree(lv_event_get_current_target(event),code,0);
  if(code==LV_EVENT_CLICKED||code==LV_EVENT_VALUE_CHANGED)
    glass_icon_play_children(lv_event_get_current_target(event));
}
static void icon_watch_parent(lv_obj_t *obj)
{
  for(unsigned depth=0;obj&&lv_obj_get_parent(obj)&&depth<24;obj=lv_obj_get_parent(obj),depth++){
    if(!lv_obj_has_flag(obj,LV_OBJ_FLAG_CLICKABLE))continue;
    bool found=false;
    for(unsigned i=0;i<lv_obj_get_event_count(obj);i++)
      if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj,i))==icon_parent_event){found=true;break;}
    if(!found)lv_obj_add_event_cb(obj,icon_parent_event,LV_EVENT_ALL,NULL);
    /* Bind the nearest owner. A parent panel must not animate every sibling
     * when one of its nested buttons receives focus or pointer input. */
    break;
  }
}

/* Smooth acceleration at each pose, with an exact and repeatable endpoint. */
static int icon_lerp(int from,int to,unsigned progress,unsigned duration)
{
  int t=(int)progress*1024/(int)duration;
  int ease=t*t/1024*(3072-2*t)/1024;
  return from+(to-from)*ease/1024;
}
static int icon_wave(unsigned phase)
{
  if(phase>=1000)return 0;
  if(phase<160)return icon_lerp(0,-256,phase,160);
  if(phase<440)return icon_lerp(-256,1024,phase-160,280);
  if(phase<720)return icon_lerp(1024,-256,phase-440,280);
  return icon_lerp(-256,0,phase-720,280);
}
static int icon_swing(unsigned phase)
{
  if(phase>=1000)return 0;
  if(phase<200)return icon_lerp(0,-120,phase,200);
  if(phase<450)return icon_lerp(-120,120,phase-200,250);
  if(phase<700)return icon_lerp(120,-50,phase-450,250);
  return icon_lerp(-50,0,phase-700,300);
}
static void icon_draw(lv_event_t *e,const struct icon_symbol *symbol)
{
  lv_obj_t *obj=lv_event_get_current_target(e);
  lv_layer_t *layer=lv_event_get_layer(e);lv_area_t area;lv_obj_get_content_coords(obj,&area);
  int size=icon_size(obj);
  int x=area.x1,y=area.y1+2;
  int width=lv_area_get_width(&area);
  lv_text_align_t align=lv_obj_get_style_text_align(obj,0);
  if(align==LV_TEXT_ALIGN_CENTER)x+=(width-size)/2;
  else if(align==LV_TEXT_ALIGN_RIGHT)x+=width-size;
  struct icon_label *icon=(struct icon_label *)obj;
  bool running=icon->running&&symbol->run_kind;
  /* Startup lights the mechanism. Hover must not shake the shell of a
   * running appliance or reset its continuous operating phase. */
  unsigned phase=icon_reduced||running?1000:icon->phase;
  unsigned run_phase=icon_reduced?0:icon->run_phase;
  int run_pulse=lv_trigo_sin((int)run_phase*360/1000)*1024/32767;
  unsigned startup=icon_reduced?255:LV_MIN(lv_tick_elaps(icon->run_started),240)*255/240;
  lv_opa_t opacity=(unsigned)lv_obj_get_style_text_opa(obj,0)*lv_obj_get_style_opa_recursive(obj,0)/255;
  lv_color_t primary=lv_obj_get_style_text_color(obj,0);
  if(symbol->tint)primary=lv_color_mix(primary,lv_color_hex(lv_color_luminance(primary)<150?0x64728a:0xbac4d4),80);
  lv_color_t accent=primary;
  bool selected=icon_selected(obj)||running;
  enum icon_visual_state state=icon_state(obj);
  bool pressed=state==ICON_PRESSED||state==ICON_PRESSED_SELECTED;
  bool highlighted=state!=ICON_NORMAL||running;
  if(highlighted||(phase>0&&phase<1000)){
    bool light=lv_color_luminance(primary)<150;
    accent=lv_color_hex(light?0x3975db:0x93b8ff);
  }
  if(symbol->tint)accent=running?lv_color_hex(symbol->tint):lv_color_hex(lv_color_luminance(primary)<150?0x98a5b5:0x7d8c9f);
  unsigned size_index=size<=14?0:size<=16?1:size<=20?2:size<=24?3:size<=32?4:5;
  for(unsigned i=0;i<symbol->count;i++){
    const struct icon_layer *p=&icon_layers[symbol->first+size_index*symbol->count+i];
    if((p->flags&4096)&&!running)continue;
    bool moving=(p->flags&2)!=0;
    unsigned local=phase<=p->delay?0:(phase-p->delay)*1000/(1000-p->delay);
    int wave=icon_wave(local),direction=(p->flags&4)?-1:1;
    lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.base.obj=obj;
    d.src=p->image;d.recolor=(p->flags&(1|8))?accent:primary;
    d.recolor_opa=255;d.opa=opacity;d.antialias=true;
    if(p->flags&16384)d.recolor=lv_color_hex(symbol->tint);
    if(!running&&symbol->tint&&p->flags==0&&phase>0&&phase<1000)
      d.recolor=lv_color_mix(lv_color_white(),primary,LV_MAX(wave,0)*30/1024);
    if(p->flags&0x0f00){
      bool light=lv_color_luminance(primary)<150;
      unsigned tone=p->flags&0x0f00;
      if((p->flags&8192)&&!running)tone=256;
      d.recolor=tone==1024?lv_color_white():tone==2048?lv_color_hex(light?0xf4e1ca:0x66513c):
        lv_color_hex(tone==512?(light?0xc6ddf4:0x304f70):(light?0xe6f0fb:0x23425e));
      if(tone==1024)d.opa=(unsigned)opacity*(light?190:95)/255;
    }
    d.pivot=(lv_point_t){symbol->pivot_x*size/24-p->x,symbol->pivot_y*size/24-p->y};
    int base_scale=pressed?242:256;d.scale_x=base_scale;d.scale_y=base_scale;
    if(p->flags&8)d.opa=(unsigned)opacity*(selected?60:highlighted?30:0)/255;
    if(p->flags&(32|64)){
      d.recolor=lv_color_hex(0xf2c472);
      d.opa=running?(unsigned)opacity*(p->flags&32?100:65)*startup/65025:0;
      if(running&&!icon_reduced)d.opa=(unsigned)d.opa*(224+run_pulse*24/1024)/255;
      if(!running&&!symbol->tint&&wave>0)d.opa=(unsigned)opacity*(p->flags&32?100:65)*wave/261120;
    }
    if(moving&&symbol->motion==6)d.opa=(unsigned)opacity*(255-LV_ABS(wave)*120/1024)/255;
    int angle=moving?(symbol->motion==2?(int)phase*360/1000:symbol->motion==1?wave*9/1024:0):0;
    d.rotation=(angle+360)%360*10;
    if(moving&&symbol->motion==8)d.rotation=(3600+icon_swing(local))%3600;
    if(moving&&symbol->motion==14)d.rotation=(3600+wave*240/1024)%3600;
    int dx=0,dy=0;
    if(moving&&symbol->motion==3)dx=direction*wave*size/16384;
    if(moving&&symbol->motion==1)dy=-wave*size/24576;
    if(moving&&(symbol->motion==4||symbol->motion==5)){
      d.scale_x+=base_scale*wave*9/102400;d.scale_y=d.scale_x;
    }
    if(moving&&symbol->motion==7)dx=(p->flags&4?-1:1)*wave*size/24576;
    if(moving&&symbol->motion==9){
      /* Nearest-pixel rounding preserves a visible nudge at 14/16 px. */
      int distance=(wave*size+(wave<0?-8192:8192))/16384;
      dx=direction*symbol->dx*distance;dy=direction*symbol->dy*distance;
    }
    if(moving&&symbol->motion==10){
      int pulse=LV_MAX(wave,0);
      d.opa=(unsigned)opacity*(255-pulse*153/1024)/255;dy=-pulse*size/49152;
    }
    if(moving&&symbol->motion==11){dy=-wave*size/16384;d.rotation=(3600-wave*60/1024)%3600;}
    if(moving&&symbol->motion==12)d.rotation=(3600+direction*wave*80/1024)%3600;
    if(moving&&symbol->motion==13){
      int pulse=LV_MAX(wave,0);
      dx=symbol->dx*pulse*size/32768;dy=symbol->dy*pulse*size/32768;
      d.opa=(unsigned)opacity*(255-pulse*140/1024)/255;
    }
    if(p->flags&128)d.opa=highlighted?d.opa:(unsigned)d.opa*LV_MAX(wave,0)/1024;
    if(running&&(p->flags&16)){
      /* Running feedback animates only the designated mechanism. The shell,
       * labels and hit target stay fixed; no device operation occurs here. */
      d.pivot=(lv_point_t){symbol->run_x*size/24-p->x,symbol->run_y*size/24-p->y};
      d.rotation=0;d.scale_x=d.scale_y=base_scale;dx=dy=0;if(!(p->flags&0x0f00))d.opa=opacity;
      /* The lightbulb's glass glows warm; its socket remains stationary. */
      if(symbol->code==0xf0eb)d.recolor=lv_color_hex(0xf3b84b);
      unsigned local_run=(run_phase+1000-p->run_delay)%1000;
      int pulse=lv_trigo_sin((int)local_run*360/1000)*1024/32767;
      if(!icon_reduced){
      if(symbol->run_kind==2)d.rotation=local_run*3600/1000;
      else if(symbol->run_kind==3){
        int offset=(local_run*size+8000)/16000;
        dx=symbol->run_dx*offset;dy=symbol->run_dy*offset;
        unsigned fade=local_run<200?local_run*255/200:local_run>700?(1000-local_run)*255/300:255;
        d.opa=(unsigned)d.opa*fade/255;
      }else if(symbol->run_kind==4||symbol->run_kind==1){
        d.opa=(unsigned)opacity*(220+pulse*35/1024)/255;
      }else if(symbol->run_kind==5){
        int offset=(pulse*size+(pulse<0?-12288:12288))/24576;
        dx=direction*symbol->run_dx*offset;dy=direction*symbol->run_dy*offset;
      }else if(symbol->run_kind==6){
        d.rotation=(3600+direction*pulse*60/1024)%3600;
      }else if(symbol->run_kind==7){
        int beat=LV_MAX(lv_trigo_sin(local_run*720/1000),0)*1024/32767;
        d.scale_x+=base_scale*beat*6/102400;d.scale_y=d.scale_x;
      }else if(symbol->run_kind==8){
        d.opa=(unsigned)opacity*(180+pulse*75/1024)/255;
      }else if(symbol->run_kind==9){
        int blink=local_run>760&&local_run<920?80-LV_ABS((int)local_run-840):0;
        d.scale_y=base_scale-base_scale*blink*3/320;
      }
      }
      d.opa=(unsigned)d.opa*(160+startup*95/255)/255;
    }
    lv_area_t bounds={x+p->x+dx,y+p->y+dy,0,0};
    bounds.x2=bounds.x1+p->image->header.w-1;bounds.y2=bounds.y1+p->image->header.h-1;
    lv_draw_image(layer,&d,&bounds);
  }
}
static void icon_event(const lv_obj_class_t *cls,lv_event_t *e)
{
  lv_obj_t *obj=lv_event_get_current_target(e);
  lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_GET_SELF_SIZE&&icon_lookup(obj)){
    unsigned pixels=icon_size(obj);
    lv_point_t *size=lv_event_get_param(e);
    size->x=LV_MAX(size->x,pixels);size->y=LV_MAX(size->y,pixels+4);
    return;
  }
  if(code==LV_EVENT_DRAW_MAIN){
    const struct icon_symbol *symbol=icon_lookup(obj);
    if(symbol){
      struct icon_label *icon=(struct icon_label *)obj;
      if(icon->running&&!icon_reduced&&icon_run_idle&&!icon->run_pending){
        icon->run_pending=true;lv_async_call(icon_run_wake,obj);
      }
      if(!icon->presented){
        icon->presented=true;icon_watch_parent(obj);
      }
      if(lv_obj_event_base(&lv_label_class,e)!=LV_RESULT_OK)return;
      icon_draw(e,symbol);return;
    }
  }
  if(lv_obj_event_base(cls,e)!=LV_RESULT_OK)return;
  if(code==LV_EVENT_DELETE){
    lv_async_call_cancel(icon_run_wake,obj);
    for(unsigned i=0;i<64;i++)if(icon_running[i]==obj)icon_running[i]=NULL;
    lv_anim_delete(obj,icon_step);icon_release(obj);icon_unqueue(obj);
  }
}
static void icon_constructor(const lv_obj_class_t *cls,lv_obj_t *obj)
{LV_UNUSED(cls);((struct icon_label *)obj)->phase=1000;}
static const lv_obj_class_t glass_icon_class={
  .base_class=&lv_label_class,.instance_size=sizeof(struct icon_label),
  .constructor_cb=icon_constructor,.event_cb=icon_event,.name="glass_icon",
  .width_def=LV_SIZE_CONTENT,.height_def=LV_SIZE_CONTENT
};
lv_obj_t *glass_icon_create(lv_obj_t *parent)
{
  lv_obj_t *obj=lv_obj_class_create_obj(&glass_icon_class,parent);
  if(obj)lv_obj_class_init_obj(obj);
  return obj;
}
