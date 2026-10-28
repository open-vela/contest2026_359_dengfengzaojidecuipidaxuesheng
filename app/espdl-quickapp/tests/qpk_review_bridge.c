/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_i18n.h"
#include "glass_heading.h"
/* Real QuickJS + extracted production LVGL bindings; hardware fixtures only. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <quickjs.h>
#include "qpk_runtime.h"
#include "glass_input_guard.h"
#include "glass_app_frame.h"
#include "glass_app_controls.h"
#include "qpk_app_theme.h"
#include "glass_icons.h"
#include "glass_heading.h"
#include "glass_ime.h"
#define QPK_EVENT_BUDGET 80
#define QPK_HANG_BUDGET 5000
enum qpk_widget_type_e {QPK_WIDGET_LABEL,QPK_WIDGET_NUMBER,QPK_WIDGET_PANEL,QPK_WIDGET_BUTTON,QPK_WIDGET_RECT,QPK_WIDGET_ARC,QPK_WIDGET_LINE,QPK_WIDGET_POLYGON,QPK_WIDGET_GROK};
struct qpk_event_s {struct qpk_event_s *next;JSValue function;lv_obj_t *owner;bool used;};
static struct {
  JSRuntime *runtime;JSContext *context;lv_obj_t *root;
  char package[48];
  lv_obj_t **widgets;uint8_t *widget_types;void **widget_extra;uint64_t *widget_generations;
  uint64_t widget_serial;int widget_capacity,widget_hint;struct qpk_event_s *events;
  qpk_font_cb_t font_cb;qpk_message_cb_t toast_cb,dialog_cb;
  lv_obj_t *input_shade,*input_textarea;JSValue input_callback;bool input_notify_cancel;
  uint32_t primary_color,secondary_color,surface_color,card_color,accent_color;
  JSValue touch_event;bool touch_used,touch_test_active;struct qpk_event_s swipe_event,back_event;
} g_qpk;
static char review_error[256];
static bool review_hass_ready;
static unsigned review_gestures;
static void review_gesture_count(lv_event_t *e){(void)e;review_gestures++;}
unsigned qpk_review_gesture_count(void){return review_gestures;}
void qpk_review_hass_fixture(bool ready){review_hass_ready=ready;}
static int qpk_arg_int(JSContext *,int,JSValueConst*,int,int);
static uint32_t qpk_arg_color(JSContext *,int,JSValueConst*,int,uint32_t);
static JSValue qpk_call_args(JSValueConst fn,unsigned budget,int argc,JSValueConst *argv)
{
  (void)budget;
  /* Match production: deleting a button inside its callback can release the
   * binding's last reference while QuickJS is still executing its closure. */
  JSValue retained=JS_DupValue(g_qpk.context,fn);
  JSValue result=JS_Call(g_qpk.context,retained,JS_UNDEFINED,argc,argv);
  JS_FreeValue(g_qpk.context,retained);return result;
}
static void qpk_event_clicked(lv_event_t *e){struct qpk_event_s *b=lv_event_get_user_data(e);if(b&&b->used){JSValue r=qpk_call_args(b->function,80,0,NULL);JS_FreeValue(g_qpk.context,r);}}
static int qpk_color_wheel_resize(lv_obj_t *,int,int);
static unsigned review_text_writes;
unsigned qpk_review_text_writes(void){return review_text_writes;}
static void review_label_set_text(lv_obj_t *label,const char *text)
{review_text_writes++;lv_label_set_text(label,text);}
#define lv_label_set_text review_label_set_text
#include "production_qpk_bindings.inc"
#undef lv_label_set_text
#include "qpk_grok.inc"
#include "qpk_color_wheel.inc"
static JSValue review_noop(JSContext *c,JSValueConst t,int n,JSValueConst *a){(void)c;(void)t;(void)n;(void)a;return JS_UNDEFINED;}
static JSValue review_pointer(JSContext *c,JSValueConst t,int n,JSValueConst *a,int magic)
{
  (void)t;if(n!=1||!JS_IsFunction(c,a[0]))return JS_ThrowTypeError(c,"pointer handler");
  JSValue global=JS_GetGlobalObject(c);
  JS_SetPropertyStr(c,global,magic?"__reviewSwipe":"__reviewTouch",JS_DupValue(c,a[0]));
  if(!magic){if(g_qpk.touch_used)JS_FreeValue(c,g_qpk.touch_event);g_qpk.touch_event=JS_DupValue(c,a[0]);g_qpk.touch_used=true;}
  else{if(g_qpk.swipe_event.used)JS_FreeValue(c,g_qpk.swipe_event.function);g_qpk.swipe_event.function=JS_DupValue(c,a[0]);g_qpk.swipe_event.used=true;}
  JS_FreeValue(c,global);return JS_UNDEFINED;
}
static JSValue review_preview(JSContext *c,JSValueConst t,int n,JSValueConst *a)
{JSValue p[5]={a[0],a[1],a[2],JS_NewInt32(c,qpk_arg_int(c,n,a,2,480)*3/4),JS_NewInt32(c,0)};JSValue result=js_ui_panel(c,t,5,p);JS_FreeValue(c,p[3]);JS_FreeValue(c,p[4]);return result;}
int qpk_review_eval(const char *source)
{
  if(!g_qpk.context)return -ENOENT;
  JSValue result=JS_Eval(g_qpk.context,source,strlen(source),"review-fixture.js",JS_EVAL_TYPE_GLOBAL);
  int error=JS_IsException(result);
  if(error){JSValue ex=JS_GetException(g_qpk.context);const char *s=JS_ToCString(g_qpk.context,ex);snprintf(review_error,sizeof(review_error),"%s",s?s:"JS error");fprintf(stderr,"QPK_ERROR %s\n",review_error);JS_FreeCString(g_qpk.context,s);JS_FreeValue(g_qpk.context,ex);}
  JS_FreeValue(g_qpk.context,result);return error?-EIO:0;
}
double qpk_review_number(const char *source)
{
  JSValue result=JS_Eval(g_qpk.context,source,strlen(source),"numeric-review.js",JS_EVAL_TYPE_GLOBAL);
  double number=0;int ret=JS_ToFloat64(g_qpk.context,&number,result);JS_FreeValue(g_qpk.context,result);
  if(ret<0){fprintf(stderr,"Invalid numeric review expression\n");abort();}return number;
}
void qpk_review_root_area(lv_area_t *area){lv_obj_update_layout(g_qpk.root);lv_obj_get_coords(g_qpk.root,area);}
/* Dispatch through the real LVGL button callback, in app-root coordinates. */
lv_obj_t *qpk_review_widget(int handle)
{return handle>0&&handle<=g_qpk.widget_capacity?g_qpk.widgets[handle-1]:NULL;}
int qpk_review_click_at(int x,int y)
{
  if(!g_qpk.root)return -ENOENT;
  lv_obj_update_layout(g_qpk.root);
  lv_area_t root;lv_obj_get_coords(g_qpk.root,&root);
  for(struct qpk_event_s *b=g_qpk.events;b;b=b->next){
    if(!b->used||!b->owner||!lv_obj_is_visible(b->owner)||!lv_obj_check_type(b->owner,&lv_button_class))continue;
    lv_area_t a;lv_obj_get_coords(b->owner,&a);
    bool clipped=false;
    for(lv_obj_t *parent=lv_obj_get_parent(b->owner);parent;parent=lv_obj_get_parent(parent))if(qpk_viewport_object(parent)){
      lv_area_t clip;lv_obj_get_coords(parent,&clip);
      if(x+root.x1<clip.x1||x+root.x1>clip.x2||y+root.y1<clip.y1||y+root.y1>clip.y2)clipped=true;
    }
    if(clipped)continue;
    if(x+root.x1>=a.x1&&x+root.x1<=a.x2&&y+root.y1>=a.y1&&y+root.y1<=a.y2){
      lv_obj_send_event(b->owner,LV_EVENT_CLICKED,NULL);return 0;
    }
  }
  fprintf(stderr,"No review button at %d,%d\n",x,y);return -ENOENT;
}
void qpk_runtime_stop(void)
{
  if(g_qpk.context){
    if(g_qpk.root)lv_obj_remove_event_cb(g_qpk.root,qpk_event_touched);
    if(g_qpk.root)lv_obj_remove_event_cb(g_qpk.root,qpk_event_swiped);
    if(g_qpk.root)lv_obj_remove_event_cb(g_qpk.root,review_gesture_count);
    if(g_qpk.swipe_event.used)JS_FreeValue(g_qpk.context,g_qpk.swipe_event.function);
    if(g_qpk.back_event.used)JS_FreeValue(g_qpk.context,g_qpk.back_event.function);
    if(g_qpk.touch_used)JS_FreeValue(g_qpk.context,g_qpk.touch_event);
    if(!JS_IsUndefined(g_qpk.input_callback)){JS_FreeValue(g_qpk.context,g_qpk.input_callback);g_qpk.input_callback=JS_UNDEFINED;}
    lv_obj_t *rail=g_qpk.root?lv_obj_get_user_data(g_qpk.root):NULL;
    if(rail)while(lv_obj_get_child_count(rail)>2)lv_obj_delete(lv_obj_get_child(rail,2));
    if(g_qpk.root)lv_obj_clean(g_qpk.root);
    for(int i=0;i<g_qpk.widget_capacity;i++)free(g_qpk.widget_extra[i]);
    struct qpk_event_s *b=g_qpk.events;while(b){struct qpk_event_s *next=b->next;if(b->used)JS_FreeValue(g_qpk.context,b->function);free(b);b=next;}
    JS_FreeContext(g_qpk.context);JS_FreeRuntime(g_qpk.runtime);
  }
  free(g_qpk.widgets);free(g_qpk.widget_types);free(g_qpk.widget_extra);free(g_qpk.widget_generations);
  memset(&g_qpk,0,sizeof(g_qpk));
}
bool qpk_runtime_running(void){return g_qpk.context!=NULL;}
bool qpk_runtime_camera_active(void){return false;}
bool qpk_runtime_camera_busy(void){return false;}
bool qpk_runtime_camera_capture(void){return false;}
bool qpk_runtime_camera_capture_request(uint32_t *request){(void)request;return false;}
int qpk_runtime_camera_photo_status(uint32_t request,char *path,size_t cap){(void)request;if(cap)path[0]=0;return -1;}
bool glass_assistant_take_notice(char *out,size_t cap){(void)out;(void)cap;return false;}
int qpk_runtime_camera_video_action(unsigned action){(void)action;return -ENOSYS;}
bool qpk_runtime_camera_poll(void){return false;}
bool qpk_runtime_camera_take_album(void){
  if(!g_qpk.context)return false;
  JSValue global=JS_GetGlobalObject(g_qpk.context);
  JSValue value=JS_GetPropertyStr(g_qpk.context,global,"__cameraAlbumRequested");
  bool requested=JS_ToBool(g_qpk.context,value)>0;
  JS_SetPropertyStr(g_qpk.context,global,"__cameraAlbumRequested",JS_FALSE);
  JS_FreeValue(g_qpk.context,value);JS_FreeValue(g_qpk.context,global);return requested;
}
int qpk_runtime_touch_test(const char *state,double x,double y){(void)state;(void)x;(void)y;return 0;}
const char *qpk_runtime_last_error(void){return review_error;}
unsigned qpk_runtime_widget_count(void){return g_qpk.widget_hint;}
int qpk_runtime_select_espdl_mode(unsigned mode){return qpk_review_eval(mode?"EspDlApp.selectMode(1)":"EspDlApp.selectMode(0)");}
#include "production_qpk_ha_navigation.inc"
static char review_launch_filename[256];
const char *review_qpk_filename(void){return review_launch_filename;}
int qpk_runtime_launch(lv_obj_t *root,const char *name,const char *package,const char *version,
 const char *filename,const char *source,size_t source_len,qpk_font_cb_t font,qpk_message_cb_t toast,qpk_message_cb_t dialog,qpk_unresponsive_cb_t unresponsive)
{
  (void)source_len;(void)name;(void)package;(void)version;(void)filename;(void)unresponsive;
  snprintf(review_launch_filename,sizeof(review_launch_filename),"%s",filename?filename:"");
  qpk_runtime_stop();snprintf(g_qpk.package,sizeof(g_qpk.package),"%s",package);review_error[0]=0;g_qpk.root=root;g_qpk.font_cb=font;g_qpk.toast_cb=toast;g_qpk.dialog_cb=dialog;
  uint32_t background=lv_color_to_u32(lv_obj_get_style_bg_color(root,0));
  bool light=((background>>16)&255)+((background>>8)&255)+(background&255)>384;
  struct glass_app_palette palette=glass_app_palette(light);
  g_qpk.primary_color=palette.ink;g_qpk.secondary_color=palette.muted;
  g_qpk.surface_color=palette.soft;g_qpk.accent_color=palette.accent;g_qpk.card_color=palette.paper;
  g_qpk.runtime=JS_NewRuntime();g_qpk.context=JS_NewContext(g_qpk.runtime);g_qpk.input_callback=JS_UNDEFINED;
  lv_obj_add_event_cb(root,qpk_event_touched,LV_EVENT_PRESSED,NULL);
  lv_obj_add_event_cb(root,qpk_event_touched,LV_EVENT_PRESSING,NULL);
  lv_obj_add_event_cb(root,qpk_event_touched,LV_EVENT_RELEASED,NULL);
  lv_obj_add_event_cb(root,qpk_event_touched,LV_EVENT_PRESS_LOST,NULL);
  lv_obj_remove_flag(root,LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root,qpk_event_swiped,LV_EVENT_GESTURE,NULL);
  lv_obj_add_event_cb(root,review_gesture_count,LV_EVENT_GESTURE,NULL);
  JSContext *c=g_qpk.context;JSValue global=JS_GetGlobalObject(c),ui=JS_NewObject(c);
#include "production_qpk_registration.inc"
  JS_SetPropertyStr(c,ui,"onTouch",JS_NewCFunctionMagic(c,review_pointer,"onTouch",1,JS_CFUNC_generic_magic,0));
  JS_SetPropertyStr(c,ui,"onBack",JS_NewCFunction(c,js_ui_on_back,"onBack",1));
  JS_SetPropertyStr(c,ui,"onSwipe",JS_NewCFunctionMagic(c,review_pointer,"onSwipe",1,JS_CFUNC_generic_magic,1));
  JS_SetPropertyStr(c,ui,"primary",JS_NewUint32(c,g_qpk.primary_color));JS_SetPropertyStr(c,ui,"secondary",JS_NewUint32(c,g_qpk.secondary_color));
  JS_SetPropertyStr(c,ui,"surface",JS_NewUint32(c,g_qpk.surface_color));JS_SetPropertyStr(c,ui,"card",JS_NewUint32(c,g_qpk.card_color));JS_SetPropertyStr(c,ui,"accent",JS_NewUint32(c,g_qpk.accent_color));
  qpk_app_theme_bind(c,ui,light);
  JS_SetPropertyStr(c,global,"ui",ui);
  JS_SetPropertyStr(c,global,"__nativePreview",JS_NewCFunction(c,review_preview,"preview",3));
  JS_SetPropertyStr(c,global,"__nativeInput",JS_NewCFunction(c,js_prompt_input,"input",2));
  JS_SetPropertyStr(c,global,"__nativeToast",JS_NewCFunction(c,js_prompt_toast,"showToast",1));
  JS_SetPropertyStr(c,global,"__nativeDialog",JS_NewCFunction(c,js_prompt_dialog,"dialog",1));
  JSValue info=JS_NewObject(c);
  JS_SetPropertyStr(c,info,"name",JS_NewString(c,name));
  JS_SetPropertyStr(c,info,"packageName",JS_NewString(c,package));
  JS_SetPropertyStr(c,info,"versionName",JS_NewString(c,version));
  JS_SetPropertyStr(c,global,"__reviewAppInfo",info);
  JS_FreeValue(c,global);
  FILE *f=fopen(REVIEW_PRELUDE,"rb");if(!f)return -ENOENT;fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);char *prelude=calloc(1,size+1);fread(prelude,1,size,f);fclose(f);
  int ret=qpk_review_eval(prelude);free(prelude);if(ret)return ret;
  global=JS_GetGlobalObject(c);JSValue system=JS_GetPropertyStr(c,global,"system");
  JS_SetPropertyStr(c,system,"language",JS_NewString(c,glass_i18n_code()));
  JS_SetPropertyStr(c,system,"translate",JS_NewCFunction(c,js_system_translate,"translate",1));
  JS_FreeValue(c,system);JS_FreeValue(c,global);
  if(review_hass_ready)qpk_review_eval("__haConfigured=true");
  ret=qpk_review_eval(source);if(ret){fprintf(stderr,"Failed app: %s\n",name);exit(2);}return ret;
}
