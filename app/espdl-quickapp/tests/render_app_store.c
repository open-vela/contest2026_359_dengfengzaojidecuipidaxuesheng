/* SPDX-License-Identifier: Apache-2.0 */
#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
#include <sys/stat.h>
#include <unistd.h>
struct store_seed {struct glass_store_entry entry;const char *source;};
#include "glass_app_store_assets.inc"
static bool review_cloud_online;
int glass_store_http_fetch(const char *base,const char *path,size_t maximum,atomic_bool *cancel,char **out)
{
  (void)base;*out=NULL;if(atomic_load(cancel))return -ECANCELED;
  const char *fixture=getenv("STORE_CLOUD_FIXTURE");if(!review_cloud_online||!fixture)return -ENETDOWN;
  char file[512];snprintf(file,sizeof(file),"%s/%s",fixture,!strcmp(path,"/v1/catalog")?"catalog.json":"app.js");
  FILE *f=fopen(file,"rb");if(!f)return -ENOENT;
  char *text=malloc(maximum+1);assert(text);size_t n=fread(text,1,maximum,f);int extra=fgetc(f);fclose(f);
  if(extra!=EOF){free(text);return -EFBIG;}text[n]=0;*out=text;return 0;
}
static unsigned timers(void)
{unsigned n=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))n++;return n;}
static unsigned objects(lv_obj_t *root)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)n+=objects(lv_obj_get_child(root,i));return n;}
static void store_settle(void)
{
  for(unsigned n=0;n<10000&&g_store_ui.root;n++){
    usleep(1000);store_ui_poll(NULL);if(!g_store_ui.snapshot->busy){settle();return;}
  }
  assert(!"store worker timeout");
}
static void click_label(lv_obj_t *root,const char *name)
{lv_obj_update_layout(root);lv_obj_t *label=review_label_find(root,name);if(!label)fprintf(stderr,"Missing click target: %s\n",name);assert(label);lv_obj_send_event(lv_obj_get_parent(label),LV_EVENT_CLICKED,NULL);settle();}
static void sd_fixture(void)
{
  assert(!mkdir(GLASS_STORE_REPOSITORY,0700)||errno==EEXIST);
  cJSON *root=cJSON_CreateObject(),*apps=cJSON_AddArrayToObject(root,"apps");cJSON_AddNumberToObject(root,"schema",1);
  for(unsigned n=0;n<5;n++){
    cJSON *app=cJSON_CreateObject();cJSON_AddItemToArray(apps,app);char id[48],name[64],dir[256],path[280];
    snprintf(id,sizeof(id),"store.sd_fixture%u",n);snprintf(name,sizeof(name),"SD 示例工具 %u",n+1);
    cJSON_AddStringToObject(app,"package",id);cJSON_AddStringToObject(app,"name",name);cJSON_AddStringToObject(app,"version","1.0.0");
    cJSON_AddStringToObject(app,"summary","本地仓库测试应用：四则运算，无需联网。");cJSON_AddNumberToObject(app,"category",0);
    cJSON_AddStringToObject(app,"sha256",store_seeds[0].entry.sha256);cJSON_AddNumberToObject(app,"bytes",store_seeds[0].entry.bytes);
    snprintf(dir,sizeof(dir),"%s/%s",GLASS_STORE_REPOSITORY,id);assert(!mkdir(dir,0700)||errno==EEXIST);
    snprintf(path,sizeof(path),"%s/app.js",dir);FILE *f=fopen(path,"wb");assert(f);fputs(store_seeds[0].source,f);fclose(f);
  }
  char *text=cJSON_PrintUnformatted(root);FILE *f=fopen(GLASS_STORE_REPOSITORY "/index.json","wb");assert(f);fputs(text,f);fclose(f);free(text);cJSON_Delete(root);
}
static void sd_home_fixture(void)
{
  FILE *file=fopen(GLASS_STORE_REPOSITORY "/index.json","rb");assert(file);char text[16384];size_t n=fread(text,1,sizeof(text)-1,file);fclose(file);text[n]=0;
  cJSON *root=cJSON_Parse(text);assert(root);
  cJSON_AddStringToObject(root,"headline","SD EDITION");cJSON_AddStringToObject(root,"tagline","Recommendations from your local catalog");
  cJSON *featured=cJSON_AddArrayToObject(root,"featured"),*feature=cJSON_CreateObject();cJSON_AddItemToArray(featured,feature);
  cJSON_AddStringToObject(feature,"package","store.sd_fixture4");cJSON_AddStringToObject(feature,"title","From your SD card.\nMade for this screen.");
  cJSON_AddStringToObject(feature,"subtitle","Live catalog update");cJSON_AddStringToObject(feature,"art","none");cJSON_AddStringToObject(feature,"tone","lavender");
  char *output=cJSON_PrintUnformatted(root);file=fopen(GLASS_STORE_REPOSITORY "/index.json","wb");assert(file);fputs(output,file);fclose(file);free(output);cJSON_Delete(root);
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);g_glass.reduced_motion=getenv("PAGE_AUDIT_MOTION")==NULL;
  char mount[256];strcpy(mount,QPK_VOLUME_INTERNAL_ROOT);*strrchr(mount,'/')=0;assert(!mkdir(mount,0700)||errno==EEXIST);
  strcpy(mount,QPK_VOLUME_SD_ROOT);*strrchr(mount,'/')=0;assert(!mkdir(mount,0700)||errno==EEXIST);
  for(unsigned theme=0;theme<2;theme++){
    unlink(GLASS_STORE_REPOSITORY "/index.json");
    unlink(QPK_VOLUME_INTERNAL_ROOT "/.store-cloud.json");
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_store_app(NULL);store_settle();
    assert(g_store_ui.snapshot->count==3&&lv_obj_get_x(g_store_ui.body)==164);
    assert(review_label_find(g_store_ui.view,g_store_ui.snapshot->featured[0].title));
    assert(review_label_find(g_store_ui.view,g_store_ui.snapshot->featured[1].title));
    review_capture(display,"store-featured","统一框架：精选与三个可安装应用");
    unsigned timer_count=timers(),object_count=objects(lv_screen_active());
    /* No repeated layout/text construction on stable polling. */
    for(unsigned n=0;n<20;n++)store_ui_poll(NULL);
    assert(timers()==timer_count&&objects(lv_screen_active())==object_count);
    lv_obj_t *tile=lv_obj_get_parent(review_label_find(g_store_ui.view,"轻量计算器"));assert(tile);lv_area_t a;lv_obj_get_coords(tile,&a);
    stroke_at(a.x1+50,a.y1+50,24,0,4);assert(!g_store_ui.selected[0]);
    stroke_at(a.x1+50,a.y1+50,0,0,1);assert(!strcmp(g_store_ui.selected,"store.calculator"));
    review_capture(display,"store-details","应用详情与明确的安装介质");
    if(!(g_store_ui.snapshot->entries[0].installed&2)){
      click_label(g_store_ui.view,"安装到 SD 卡");store_settle();assert(g_store_ui.snapshot->entries[0].installed==2);
    }
    assert(review_label_find(g_store_ui.view,"打开 SD 卡应用"));
    review_capture(display,"store-installed-detail","安装完成与保留数据卸载");
    click_label(g_store_ui.view,"卸载（保留数据）");assert(g_store_ui.modal);
    review_capture(display,"store-uninstall-confirm","卸载确认");click_label(g_store_ui.modal,"卸载");store_settle();assert(!g_store_ui.snapshot->entries[0].installed);
    click_label(g_store_ui.view,"安装到 SD 卡");store_settle();assert(g_store_ui.snapshot->entries[0].installed==2);
    store_ui_back(NULL);click_label(g_store_ui.rail,"云端商城");assert(review_label_find(g_store_ui.view,"尚未配置云端商店"));
    review_capture(display,"store-cloud-empty","云端商城配置入口");
    click_label(g_store_ui.view,"配置地址");assert(g_store_ui.cloud_input);
    lv_textarea_set_text(g_store_ui.query_input,"https://store.example.com");store_ui_search_apply(NULL);store_settle();
    assert(g_store_ui.snapshot->cloud_url[0]&&g_store_ui.snapshot->cloud_error==-ENETDOWN);
    review_capture(display,"store-cloud-offline","云端不可达时保留本地能力");
    if(getenv("STORE_CLOUD_FIXTURE")){
      review_cloud_online=true;store_ui_refresh(NULL);store_settle();assert(g_store_ui.snapshot->cloud_repository);
      assert(review_label_find(g_store_ui.view,"云端计算器"));review_capture(display,"store-cloud-catalog","实际服务端目录驱动的云端列表");
      click_label(g_store_ui.view,"云端计算器");assert(!strcmp(g_store_ui.selected,"store.cloud_demo"));
      if(!g_store_ui.snapshot->entries[store_ui_selected()].installed){click_label(g_store_ui.view,"安装到 SD 卡");store_settle();}
      assert(g_store_ui.snapshot->entries[store_ui_selected()].installed==2);review_capture(display,"store-cloud-installed","云端应用校验后安装完成");
      click_label(g_store_ui.view,"卸载（保留数据）");click_label(g_store_ui.modal,"卸载");store_settle();store_ui_back(NULL);
    }
    click_label(g_store_ui.view,"配置地址");lv_textarea_set_text(g_store_ui.query_input,"");store_ui_search_apply(NULL);store_settle();assert(!g_store_ui.snapshot->cloud_url[0]);review_cloud_online=false;
    click_label(g_store_ui.rail,"精选");click_label(g_store_ui.view,"轻量计算器");
    store_ui_back(NULL);click_label(g_store_ui.rail,"分类");click_label(g_store_ui.view,"专注效率");
    assert(review_label_find(g_store_ui.view,"专注计时")&&!review_label_find(g_store_ui.view,"轻量计算器"));
    review_capture(display,"store-categories","分类筛选");
    store_ui_search(NULL);assert(g_store_ui.query_input&&g_store_ui.keyboard);
    review_capture(display,"store-search-input","原生搜索输入法");
    lv_textarea_set_text(g_store_ui.query_input,"找不到");store_ui_search_apply(NULL);settle();
    review_capture(display,"store-empty-search","搜索空状态");assert(review_label_find(g_store_ui.view,"没有找到匹配的应用"));
    g_store_ui.query[0]=0;click_label(g_store_ui.rail,"SD 仓库");
    review_capture(display,"store-sd-empty","无本地仓库的可恢复空状态");
    sd_fixture();store_ui_refresh(NULL);store_settle();assert(g_store_ui.snapshot->count==8);
    assert(review_label_find(g_store_ui.view,"SD 示例工具 1"));review_capture(display,"store-sd-catalog","有效的 SD 仓库与分页");
    click_label(g_store_ui.view,"下一页");assert(review_label_find(g_store_ui.view,"SD 示例工具 5"));
    click_label(g_store_ui.view,"上一页");
    click_label(g_store_ui.rail,"已安装");assert(review_label_find(g_store_ui.view,"轻量计算器"));
    review_capture(display,"store-installed","已安装管理");
    sd_home_fixture();click_label(g_store_ui.rail,"精选");store_ui_refresh(NULL);store_settle();
    assert(g_store_ui.snapshot->featured_count==1&&review_label_find(g_store_ui.view,"SD EDITION"));
    assert(review_label_find(g_store_ui.view,"SD 示例工具 5"));assert(review_label_find(g_store_ui.view,g_store_ui.snapshot->featured[0].title));review_capture(display,"store-dynamic-home","SD 目录热更新首页与推荐应用");
    click_label(g_store_ui.view,"探索应用  →");assert(!strcmp(g_store_ui.selected,"store.sd_fixture4"));
    int local=store_ui_selected();assert(local>=0);if(!g_store_ui.snapshot->entries[local].installed){click_label(g_store_ui.view,"安装到 SD 卡");store_settle();}
    assert(g_store_ui.snapshot->entries[store_ui_selected()].installed==2);
    click_label(g_store_ui.view,"卸载（保留数据）");click_label(g_store_ui.modal,"卸载");store_settle();
    assert(!g_store_ui.snapshot->entries[store_ui_selected()].installed);store_ui_back(NULL);
    /* Opaque cover defers drawing but completion is still consumed. */
    unsigned painted=g_store_ui.painted;lv_obj_t *cover=glass_box(lv_layer_top(),0,0,1024,600,detail_paper(),0);lv_obj_update_layout(cover);
    assert(!glass_ui_can_paint(g_store_ui.root));assert(!glass_store_submit(GLASS_STORE_REFRESH,0,QPK_VOLUME_INTERNAL));store_settle();
    assert(g_store_ui.painted==painted&&!g_store_ui.snapshot->busy);lv_obj_delete(cover);store_ui_poll(NULL);assert(g_store_ui.painted==g_store_ui.snapshot->revision);
    panel_hide(NULL);settle();assert(!g_store_ui.root&&!g_store_ui.timer);
    unsigned baseline_timers=timers(),baseline_objects=objects(lv_screen_active());
    for(unsigned n=0;n<12;n++){glass_store_app(NULL);store_settle();store_ui_search(NULL);panel_hide(NULL);settle();assert(timers()==baseline_timers&&objects(lv_screen_active())==baseline_objects);}
    /* Actual native QuickJS constructors and callbacks for every seed. */
    for(unsigned i=0;i<3;i++){
      lv_obj_t *body=qapp_page_prepare(store_seeds[i].entry.name);assert(body);
      assert(!qpk_runtime_launch(body,store_seeds[i].entry.name,store_seeds[i].entry.package,"1.0.0","builtin:/store-fixture.js",store_seeds[i].source,strlen(store_seeds[i].source),zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive));
      qapp_page_commit(body);settle();
      if(i==0){click_label(body,"1");click_label(body,"+");click_label(body,"2");click_label(body,"=");assert(review_label_find(body,"3"));}
      if(i==1){assert(review_label_find(body,"25:00"));click_label(body,"开始");assert(review_label_find(body,"暂停"));click_label(body,"暂停");assert(review_label_find(body,"继续"));click_label(body,"休息 5 分钟");assert(review_label_find(body,"05:00"));}
      if(i==2){click_label(body,"＋");assert(review_label_find(body,"1"));click_label(body,"保存计数");assert(review_label_find(body,"已保存"));}
      const char *ids[]={"store-app-calculator","store-app-focus","store-app-counter"};review_capture(display,ids[i],"精选应用：实际 QuickJS/LVGL 渲染");panel_hide(NULL);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("APP_STORE_UI_OK: both themes; real LVGL input guard; installation with real worker; filters/search/confirmation/empty states; opaque cover completes without paint; 24 exit/keyboard cleanup cycles; actual QuickJS execution and calculator/focus/counter callbacks");
}
