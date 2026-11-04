/* Private TinyTTF stream over verified compressed material pages.
 * Published views remain stable; each glyph records the selected backing face
 * in its private gid bit until release, so fallback changes cannot mis-release.
 */
#include "glass_material.h"
struct material_font {lv_font_t view;lv_font_t *fallback,*external;unsigned height,normal;lv_font_kerning_t kerning;struct material_font *next;};
static struct material_font *material_fonts;
#include <stdatomic.h>
static bool material_font_trimmed,material_font_changed;
static uint32_t material_font_retry;
static void *material_font_data;
static pthread_t material_font_thread;
static bool material_font_running;
static atomic_bool material_font_done,material_font_cancel;
static void *material_font_pending;
extern void glass_boot_stage(const char *,int) __attribute__((weak));
static void material_font_log(const char *stage,int result)
{if(glass_boot_stage)glass_boot_stage(stage,result);}
static bool material_font_cancelled(void){return atomic_load(&material_font_cancel);}
static void *material_font_load(void *unused)
{
 (void)unused;material_font_log("font-background-read-begin",0);
 void *data=gmat_acquire_background(GMAT_FONTS_DEFAULT_OTF,material_font_cancelled);
 if(atomic_load(&material_font_cancel)||gmat_under_pressure()){gmat_release(data);data=NULL;}
 material_font_pending=data;
 material_font_log("font-background-read-end",data?0:-EAGAIN);
 atomic_store_explicit(&material_font_done,true,memory_order_release);return NULL;
}
static bool material_font_glyph(const lv_font_t *font,lv_font_glyph_dsc_t *glyph,uint32_t letter,uint32_t next)
{
 struct material_font *m=(struct material_font *)font->dsc;lv_font_t *actual=m->external?m->external:m->fallback;
 bool found=actual->get_glyph_dsc(actual,glyph,letter,next);if(found&&actual==m->fallback)glyph->gid.index|=0x80000000u;return found;
}
static const void *material_font_bitmap(lv_font_glyph_dsc_t *glyph,lv_draw_buf_t *draw)
{
 const lv_font_t *view=glyph->resolved_font;struct material_font *m=(struct material_font *)view->dsc;uint32_t gid=glyph->gid.index;
 lv_font_t *actual=gid>>31?m->fallback:m->external;if(!actual)return NULL;
 glyph->gid.index=gid&0x7fffffffu;glyph->resolved_font=actual;const void *result=actual->get_glyph_bitmap(glyph,draw);glyph->resolved_font=view;glyph->gid.index=gid;return result;
}
static void material_font_release(const lv_font_t *font,lv_font_glyph_dsc_t *glyph)
{
 struct material_font *m=(struct material_font *)font->dsc;uint32_t gid=glyph->gid.index;lv_font_t *actual=gid>>31?m->fallback:m->external;
 if(actual&&actual->release_glyph){const lv_font_t *view=glyph->resolved_font;glyph->gid.index=gid&0x7fffffffu;glyph->resolved_font=actual;actual->release_glyph(actual,glyph);glyph->resolved_font=view;glyph->gid.index=gid;}
}
static void material_font_limit(lv_font_t *font,unsigned normal)
{
 if(!font)return;ttf_font_desc_t *dsc=(ttf_font_desc_t *)font->dsc;unsigned count=material_font_trimmed&&normal>64?64:normal;dsc->cache_size=(int)count;
 lv_cache_set_max_size(dsc->glyph_cache,count,dsc);lv_cache_set_max_size(dsc->draw_data_cache,count,dsc);
 while(lv_cache_get_size(dsc->glyph_cache,dsc)>count&&lv_cache_evict_one(dsc->glyph_cache,dsc)){}
 while(lv_cache_get_size(dsc->draw_data_cache,dsc)>count&&lv_cache_evict_one(dsc->draw_data_cache,dsc)){}
}
static void material_font_update_view(struct material_font *m);
void glass_material_font_pressure(bool active)
{
 material_font_trimmed=active;
 if(active){
  atomic_store(&material_font_cancel,true);
  /* Called by the GUI owner after the complete LVGL handler: no glyphs or
   * draw tasks still borrow these faces. Published proxy pointers survive. */
  for(struct material_font *m=material_fonts;m;m=m->next){
   if(m->external){sd_ttf_destroy(m->external);m->external=NULL;material_font_update_view(m);material_font_changed=true;}
  }
  gmat_release(material_font_data);material_font_data=NULL;
 }
 for(struct material_font *m=material_fonts;m;m=m->next)material_font_limit(m->fallback,m->normal>64?64:m->normal);
}
static void material_font_update_view(struct material_font *m)
{lv_font_t *actual=m->external?m->external:m->fallback;const lv_font_t *fallback=m->view.fallback;m->view=*actual;m->view.dsc=m;m->view.get_glyph_dsc=material_font_glyph;m->view.get_glyph_bitmap=material_font_bitmap;m->view.release_glyph=material_font_release;m->view.fallback=fallback;}
bool glass_material_font_refresh(lv_font_t *font)
{if(!font||font->get_glyph_dsc!=material_font_glyph)return false;struct material_font *m=(struct material_font *)font->dsc;const lv_font_t *fallback=font->fallback;*font=m->view;font->fallback=fallback;return true;}
int glass_material_fonts_poll(void)
{
 int changed=material_font_changed;material_font_changed=false;
 if(material_font_running&&atomic_load_explicit(&material_font_done,memory_order_acquire)){
  pthread_join(material_font_thread,NULL);material_font_running=false;
  if(material_font_trimmed||atomic_load(&material_font_cancel)||gmat_under_pressure()){gmat_release(material_font_pending);material_font_pending=NULL;}
  material_font_data=material_font_pending;material_font_pending=NULL;
 }
 if(!material_fonts||material_font_trimmed||gmat_under_pressure()||!gmat_boot_enabled())return changed;
 if(material_font_data){
  /* One in-memory face per GUI turn. No storage I/O or full-file checksum
   * runs on the GUI thread. Outlines remain in the verified immutable blob. */
  for(struct material_font *m=material_fonts;m;m=m->next)if(!m->external){
   m->external=sd_ttf_create_data_ex(material_font_data,gmat_size(GMAT_FONTS_DEFAULT_OTF),m->height,m->kerning,m->normal);
   if(m->external){material_font_update_view(m);return 1;}return changed;
  }
  return changed;
 }
 uint32_t now=lv_tick_get();
 if(material_font_running||(material_font_retry&&(uint32_t)(now-material_font_retry)<5000))return changed;
 material_font_retry=now?now:1;atomic_store(&material_font_cancel,false);atomic_store(&material_font_done,false);
 pthread_attr_t attr;int ret=pthread_attr_init(&attr);
 if(!ret){ret=pthread_attr_setstacksize(&attr,16384);if(!ret)ret=pthread_create(&material_font_thread,&attr,material_font_load,NULL);pthread_attr_destroy(&attr);}
 if(!ret)material_font_running=true;else material_font_log("font-worker-start-failed",-ret);
 return changed;
}
lv_font_t *glass_material_font_create(const void *fallback,size_t bytes,unsigned height,lv_font_kerning_t kerning,unsigned count)
{
 struct material_font *m=calloc(1,sizeof(*m));if(!m)return NULL;m->height=height;m->normal=count;m->kerning=kerning;
 m->fallback=sd_ttf_create_data_ex(fallback,bytes,height,kerning,count>64?64:count);if(!m->fallback){free(m);return NULL;}
 material_font_update_view(m);m->next=material_fonts;material_fonts=m;return &m->view;
}
