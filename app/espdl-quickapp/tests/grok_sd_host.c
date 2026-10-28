/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_grok_player.inc"
#include <assert.h>
static struct chat_activity_snapshot chat;
static struct glass_voice_snapshot voice;
void glass_chat_activity_get(struct chat_activity_snapshot *out){*out=chat;}
void glass_voice_get(struct glass_voice_snapshot *out){*out=voice;}
static unsigned char framebuffer[300*180*2];
static const lv_font_t *font(int size){(void)size;return LV_FONT_DEFAULT;}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *data){(void)a;(void)data;lv_display_flush_ready(d);}
/* Advance virtual time only after outstanding reads complete. Real host disk
 * latency must not turn a 3-second animation into a few displayed frames. */
static void pump(unsigned n){while(n--){
  lv_tick_inc(20);glass_grok_test_step_visible();lv_timer_handler();
  for(unsigned j=0;j<8;j++)if(instances[j]){
    bool busy=true;
    for(unsigned tries=0;busy&&tries<10000;tries++){
      pthread_mutex_lock(&instances[j]->lock);busy=instances[j]->busy;pthread_mutex_unlock(&instances[j]->lock);
      if(busy)usleep(100);
    }
    assert(!busy);
  }
}}
static void wait_frame(struct glass_grok *b,unsigned before){for(unsigned i=0;i<1000&&b->displayed==before;i++)pump(1);assert(b->displayed>before);assert(!b->error);}
static void choose(struct glass_grok *b,unsigned id){unsigned before=b->displayed;glass_grok_select(b,id);wait_frame(b,before);assert(b->current==id);}
static void mood_cases(lv_obj_t *root)
{
  const unsigned moods[]={3,10,19,14,4,16};
  for(unsigned black=0;black<2;black++){
    struct glass_grok *b=glass_grok_create_sprite(root,0,0,170,170,120,black,font);assert(b);wait_frame(b,0);
    glass_grok_select(b,2);pump(500);assert(b->current==2); /* App selection stays explicit. */
    glass_grok_set_ambient(b,true);unsigned seen=0;
    for(unsigned i=0;i<3900;i++){
      glass_grok_select(b,2);pump(1); /* Repeated UI hints do not postpone moods. */
      for(unsigned j=0;j<6;j++)if(b->current==moods[j])seen|=1u<<j;
    }
    assert(seen==63);
    for(unsigned i=0;i<3;i++){
      unsigned work[]={35,30,39};choose(b,work[i]);pump(420);assert(b->current==work[i]);
    }
    glass_grok_select(b,2);glass_grok_set_reduced_motion(b,true);pump(600);assert(b->current==2);
    glass_grok_set_reduced_motion(b,false);glass_grok_set_ambient(b,false);pump(600);assert(b->current==2);
    lv_obj_delete(b->root);
  }
  puts("GROK_MOODS_OK: six idle moods in both palettes; repeated idle hints; immediate service priority; explicit app selection; reduced motion");
}
int main(int argc,char **argv)
{
  assert(argc==2);lv_init();lv_display_t *d=lv_display_create(300,180);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,framebuffer,NULL,sizeof(framebuffer),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(d,flush);
  lv_obj_t *root=lv_screen_active();lv_obj_remove_style_all(root);
  for(unsigned style=0;style<5;style++){
    struct glass_grok *b=style==0?glass_grok_create_avatar(root,font):style<3?glass_grok_create_pixel(root,285,style==2,font):glass_grok_create_sprite(root,0,0,170,170,120,style==4,font);assert(b);wait_frame(b,0);
    for(unsigned i=0;i<32;i++)choose(b,ids[i]);
    const char *invalid[]={"08","002","99","-1","2.5","__proto__",""};for(unsigned i=0;i<7;i++)assert(glass_grok_test_expression(b,invalid[i])==2);
    choose(b,30);pump(20);unsigned before=b->displayed;pump(30);assert(b->displayed>before);
    glass_grok_set_reduced_motion(b,true);pump(20);before=b->displayed;pump(50);assert(b->displayed==before);choose(b,39);assert(b->next_frame==1);
    glass_grok_set_reduced_motion(b,false);pump(10);
    lv_obj_add_flag(b->root,LV_OBJ_FLAG_HIDDEN);before=b->displayed;pump(30);assert(b->displayed==before);lv_obj_remove_flag(b->root,LV_OBJ_FLAG_HIDDEN);wait_frame(b,before);
    if(style>=3){glass_grok_action(b,42);pump(170);assert(b->action==0);for(unsigned id=43;id<=51;id++){before=b->displayed;glass_grok_action(b,id);wait_frame(b,before);assert(b->action==id);}glass_grok_action(b,0);}
    if(style>=3){
      assert(glass_grok_gaze(b,0,0));wait_frame(b,b->displayed);
      choose(b,2);
      unsigned left=0,right=0;
      uint16_t *front=(uint16_t *)b->pixels[b->front];
      for(unsigned y=40;y<82;y++)for(unsigned x=35;x<92;x++){
        unsigned v=front[y*128+x],r=v>>11,g=(v>>5)&63,blue=v&31;
        bool eye=b->black?(r>26&&g>53&&blue>26):(r<7&&g<14&&blue<7);
        if(eye){if(x<64)left++;else right++;}
      }
      assert(left&&left==right); /* Frontal idle eyes have equal contour area. */
      for(unsigned i=0;i<32;i++){choose(b,ids[i]);assert(b->gaze_enabled);}
      for(int x=-1000;x<=1000;x+=500){before=b->displayed;assert(glass_grok_gaze(b,x,-x));wait_frame(b,before);assert(b->gaze_x==x);}
      glass_grok_set_reduced_motion(b,true);before=b->displayed;
      assert(glass_grok_gaze(b,0,0));wait_frame(b,before);assert(b->gaze_x==0&&b->gaze_y==0);
    }
    lv_refr_now(d);char path[1024];snprintf(path,sizeof(path),"%s/style-%u.rgb565",argv[1],style);FILE *f=fopen(path,"wb");assert(f);fwrite(framebuffer,1,sizeof(framebuffer),f);fclose(f);
    lv_obj_delete(b->root);
  }
  struct glass_grok *b=glass_grok_create_pixel(root,285,false,font);assert(b);wait_frame(b,0);chat.turn_id=1;chat.phase=CHAT_REQUESTING;chat.state=CHAT_WAITING;chat.activity=CHAT_ACTIVITY_THINK;pump(20);assert(b->current==30);chat.activity=CHAT_ACTIVITY_REPLY;pump(20);assert(b->current==39);chat.phase=CHAT_IDLE;chat.state=CHAT_DONE;pump(20);assert(b->current==33);pump(140);assert(b->current==2);
  lv_obj_t *overlay=lv_obj_create(root);lv_obj_update_layout(root);assert(lv_obj_is_visible(overlay));glass_grok_set_obscured_by(b,overlay);unsigned before=b->displayed;pump(25);assert(b->displayed==before);lv_obj_add_flag(overlay,LV_OBJ_FLAG_HIDDEN);wait_frame(b,before);lv_obj_delete(b->root);lv_obj_delete(overlay);
  mood_cases(root);
  for(unsigned i=0;i<24;i++){b=glass_grok_create_avatar(root,font);assert(b);if(i%2)pump(1);lv_obj_delete(b->root);}
  b=glass_grok_create_avatar(root,font);strcpy(b->path,"/nonexistent/grok.gbs");pump(30);assert(!b->initialized&&b->errors>0);lv_obj_delete(b->root);
  b=glass_grok_create_avatar(root,font);snprintf(b->path,sizeof(b->path),"%s/bad.gbs",argv[1]);FILE *f=fopen(b->path,"wb");assert(f);fwrite("bad",1,3,f);fclose(f);pump(30);assert(!b->initialized&&b->error==-EIO);lv_obj_delete(b->root);
  lv_obj_delete(root);lv_display_delete(d);lv_deinit();puts("GROK_SD_HOST_OK: 160 palette/state checks; spin/gaze; stream reducer; completion once; hidden/reduced pause; missing/truncated banks; 24 in-flight reader teardowns");return 0;
}
