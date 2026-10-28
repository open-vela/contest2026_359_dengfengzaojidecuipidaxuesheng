/* Shared v1/v3 file manager. Filesystem ownership stays in the worker. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glass_file_service.h"
#include "qpk_mjpeg.h"
#include "glass_file_types.h"
#include "glass_file_folder.inc"
#include "glass_file_icons.inc"
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
#include "local_album.h"
#include "local_album_ui.h"
#endif
#ifndef GLASS_FILES_ROOT
#define GLASS_FILES_ROOT "/data"
#endif
#define GLASS_FILES_SDCARD_ROOT "/sdcard"
static const char *g_files_root = GLASS_FILES_ROOT;
static char g_files_path[GLASS_FILE_PATH_CAP] = GLASS_FILES_ROOT;
static char g_files_selected[GLASS_FILE_PATH_CAP];
static char g_files_error[160];
static unsigned g_files_page;
static bool g_files_pending;
static bool g_files_list_error;
static lv_obj_t *g_files_input;
static lv_obj_t *g_files_shell;
static lv_obj_t *g_files_title;
static lv_obj_t *g_files_auxiliary;
static bool g_files_return_apps;
static void glass_files_open(lv_event_t *e);
static void glass_files_storage(lv_event_t *e);
static void glass_files_cancel(lv_event_t *e);
static void glass_files_up(lv_event_t *e);
static void glass_files_cancel_calls(void);
static void glass_files_stop(void);
static void glass_files_back(lv_event_t *e)
{
  if(g_files_auxiliary&&!lv_obj_has_flag(g_files_auxiliary,LV_OBJ_FLAG_HIDDEN)){glass_files_cancel(e);return;}
  if(g_files_pending){glass_files_cancel_calls();glass_files_stop();}
  if(strcmp(g_files_path,g_files_root)){glass_files_up(e);return;}
  if(g_files_return_apps)glass_apps(e);else panel_hide(e);
}
struct files_tile {
  lv_obj_t *button,*name,*image,*type;
  unsigned revision,pressed_revision;
  bool pressed;
};
static struct
{
  lv_obj_t *card;
  lv_obj_t *list;
  lv_obj_t *path;
  lv_obj_t *error;
  lv_obj_t *footer;
  lv_obj_t *back;
  lv_obj_t *device;
  lv_obj_t *sdcard;
  lv_obj_t *up;
  lv_obj_t *pager;
  lv_obj_t *cancel;
  lv_obj_t *paste;
  lv_obj_t *note;
  struct files_tile tiles[GLASS_FILE_PAGE_SIZE];
  bool has_next;
} g_files_browser;
static struct glass_swipe_tracker g_files_drag;
static int g_files_operation;
static char g_files_clipboard[GLASS_FILE_PATH_CAP];
static bool g_files_clipboard_move;
static struct glass_file_job *g_files_job;
/* Tile callbacks borrow result entries; filename labels own their copies. */
static struct glass_file_job *g_files_list_job;
static lv_timer_t *g_files_timer;
static bool g_files_retry;
static struct
{
  enum glass_file_action action;
  unsigned page;
  char path[GLASS_FILE_PATH_CAP];
  char source[GLASS_FILE_PATH_CAP];
  char name[GLASS_FILE_NAME_CAP];
} g_files_request;
static lv_obj_t *g_files_copy_status;
static uint64_t g_files_copy_bytes;
static void glass_files_render(void *unused);
static void glass_files_details(void *unused);
static void glass_files_image(void *unused);
static bool glass_files_image_name(const char *name);
static void glass_files_dialog_async(void *unused);
static void glass_files_copy_start(void *unused);
static void glass_files_select(lv_event_t *e);
static void glass_files_tick(lv_timer_t *timer);
static void glass_files_page(void *context,unsigned page);
static void glass_files_details_ready(const struct glass_file_result *result);

static lv_obj_t *files_button(lv_obj_t *parent,const char *text,int x,int y,int w,
                              uint32_t color,lv_event_cb_t cb,void *data)
{
  LV_UNUSED(color);
  lv_obj_t *button=glass_button(parent,text,x,y,w,detail_soft(),cb,data);
  lv_obj_set_style_radius(button,10,0);lv_obj_set_style_shadow_width(button,0,0);
  lv_obj_set_style_border_width(button,0,0);
  lv_obj_t *label=lv_obj_get_child(button,0);
  lv_obj_set_style_text_font(label,home_font(14),0);
  lv_obj_set_style_text_color(label,lv_color_hex(detail_ink()),0);
  return button;
}
static const char *files_type(const char *name){return files_design(name)->label;}
static lv_obj_t *files_material(lv_obj_t *parent,const char *name,bool directory,int x,int y)
{
  lv_obj_t *image=lv_image_create(parent);
  lv_image_set_src(image,directory?&files_folder_image:files_type_images[files_design(name)->kind]);
  lv_obj_set_pos(image,x+(directory?0:4),y);lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE);
  return image;
}

static void glass_files_cancel_calls(void)
{
  lv_async_call_cancel(glass_files_render, NULL);
  lv_async_call_cancel(glass_files_details, NULL);
  lv_async_call_cancel(glass_files_image, NULL);
  lv_async_call_cancel(glass_files_dialog_async, NULL);
  lv_async_call_cancel(glass_files_copy_start, NULL);
}

static void glass_files_stop(void)
{
  if (g_files_timer) lv_timer_delete(g_files_timer);
  g_files_timer = NULL;
  /* release only cancels and drops ownership; it never closes or joins. */
  glass_file_release(g_files_job);
  g_files_job = NULL;
  g_files_retry = false;
  g_files_pending = false;
  g_files_copy_status = NULL;
}

/* Reset borrowed result pointers before releasing worker data. Keep widgets
 * only while browsing; entering a viewer/details page frees the tile pool. */
static void glass_files_list_reset(void)
{
  for(size_t i=0;i<GLASS_FILE_PAGE_SIZE;i++) {
    lv_obj_t *button=g_files_browser.tiles[i].button;
    if(button){lv_obj_set_user_data(button,NULL);lv_obj_add_flag(button,LV_OBJ_FLAG_HIDDEN);}
  }
  if(g_files_browser.note)lv_obj_add_flag(g_files_browser.note,LV_OBJ_FLAG_HIDDEN);
  glass_file_release(g_files_list_job);g_files_list_job=NULL;
}
static void glass_files_list_clear(void)
{
  glass_files_list_reset();
  if(g_files_browser.list)lv_obj_clean(g_files_browser.list);
  memset(g_files_browser.tiles,0,sizeof(g_files_browser.tiles));g_files_browser.note=NULL;
}
static void files_list_note(const char *text)
{
  if(!g_files_browser.note)
    g_files_browser.note=glass_text(g_files_browser.list,text,12,12,800,20,theme_secondary());
  else lv_label_set_text(g_files_browser.note,text);
  lv_obj_remove_flag(g_files_browser.note,LV_OBJ_FLAG_HIDDEN);
}

static void glass_files_closed(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_files_cancel_calls();
  glass_files_stop();
  glass_files_list_clear();
  g_files_error[0]=0;g_files_list_error=false;
  g_files_input = NULL;
  g_files_shell = NULL;
  g_files_title = NULL;
  g_files_auxiliary = NULL;
  memset(&g_files_browser, 0, sizeof(g_files_browser));
  g_files_drag.tracking=false;
}

static lv_obj_t *glass_files_card(const char *title)
{
  glass_files_cancel_calls();
  glass_files_stop();
  glass_files_list_clear();
  if (!g_files_shell)
    {
      bool from_apps=g_desktop.apps_page;
      g_files_shell = panel_card(title);
      if (!g_files_shell) return NULL;
      g_files_return_apps=from_apps;
      glass_navigation_register(glass_files_open,from_apps,0);
      detail_page(g_files_shell);
      lv_obj_t *back=detail_sidebar(g_files_shell,glass_tr("文件"),LV_SYMBOL_DIRECTORY,glass_files_back);
      g_files_browser.back=back;
      lv_label_set_text(lv_obj_get_child(back,0),from_apps?glass_tr("返回应用"):glass_tr("返回桌面"));
      g_desktop.page_back=glass_files_back;
      g_files_browser.device=detail_nav(g_files_shell,glass_tr("设备存储"),LV_SYMBOL_DIRECTORY,104,140,glass_files_storage,(void *)GLASS_FILES_ROOT);
      g_files_browser.sdcard=detail_nav(g_files_shell,glass_tr("SD 卡"),LV_SYMBOL_SD_CARD,156,140,glass_files_storage,(void *)GLASS_FILES_SDCARD_ROOT);
      glass_sidebar_item(g_files_browser.device,!strcmp(g_files_root,GLASS_FILES_ROOT));
      glass_sidebar_item(g_files_browser.sdcard,!strcmp(g_files_root,GLASS_FILES_SDCARD_ROOT));
      g_files_title = detail_text(g_files_shell,title,216,26,784,28,detail_ink());
      lv_obj_set_size(g_files_shell,1024,GLASS_CONTENT_HEIGHT);
      lv_obj_align(g_files_shell,LV_ALIGN_TOP_LEFT,0,0);
      lv_obj_set_style_radius(g_files_shell,0,0);lv_obj_set_style_border_width(g_files_shell,0,0);
      lv_obj_set_style_bg_color(g_files_shell,lv_color_hex(detail_canvas()),0);
      lv_obj_add_event_cb(g_files_shell, glass_files_closed, LV_EVENT_DELETE, NULL);
    }
  g_files_input = NULL;
  lv_label_set_text(g_files_title, title);
  if (g_files_browser.card) lv_obj_add_flag(g_files_browser.card, LV_OBJ_FLAG_HIDDEN);
  if (!g_files_auxiliary)
    {
      g_files_auxiliary = lv_obj_create(g_files_shell);
      lv_obj_remove_style_all(g_files_auxiliary);
      lv_obj_set_pos(g_files_auxiliary,192,0);
      lv_obj_set_size(g_files_auxiliary,832,GLASS_CONTENT_HEIGHT);
      lv_obj_remove_flag(g_files_auxiliary, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
  else lv_obj_clean(g_files_auxiliary);
  lv_obj_remove_flag(g_files_auxiliary, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lv_obj_get_child(g_files_browser.back,0),glass_tr("返回目录"));
  return g_files_auxiliary;
}

static lv_obj_t *glass_files_icon(lv_obj_t *parent, const char *symbol,
                                 int x, int y, lv_event_cb_t callback, void *data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, 48, 40);lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(detail_soft()),0);lv_obj_set_style_radius(button,10,0);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
  lv_obj_t *label = lv_label_create(button);
  lv_obj_set_style_text_font(label, glass_icon_font(24), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_label_set_text(label, symbol);
  lv_obj_center(label);
  return button;
}

static bool glass_files_name(const char *name)
{
  /* The textarea's limit counts characters; the filesystem limit is bytes. */
  size_t length = strlen(name);
  if (!length || length >= GLASS_FILE_NAME_CAP ||
      !strcmp(name, ".") || !strcmp(name, "..")) return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
  return true;
}

/* Syntax only. The worker checks every existing component before using it. */
static bool glass_files_path_valid(const char *path)
{
  size_t root = strlen(g_files_root);
  if (strlen(path) >= GLASS_FILE_PATH_CAP ||
      strncmp(path, g_files_root, root) ||
      (path[root] && path[root] != '/')) return false;
  for (size_t i = root; path[i]; )
    {
      const char *component = path + ++i;
      size_t length = strcspn(component, "/");
      if (!length || length >= GLASS_FILE_NAME_CAP ||
          (length == 1 && component[0] == '.') ||
          (length == 2 && !strncmp(component, "..", 2))) return false;
      for (size_t j = 0; j < length; j++)
        if ((unsigned char)component[j] < 32 || component[j] == 127 ||
            component[j] == '\\') return false;
      i += length;
    }
  return true;
}

static void glass_files_refresh(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_files_pending && lv_async_call(glass_files_render, NULL) == LV_RESULT_OK)
    g_files_pending = true;
}

static void glass_files_cancel(lv_event_t *e)
{
  LV_UNUSED(e);
  /* Cancellation must remain available while any request is pending. */
  glass_files_cancel_calls();
  glass_files_stop();
  glass_files_refresh(NULL);
}

static void glass_files_error(int error)
{
  g_files_list_error=false;
  snprintf(g_files_error, sizeof(g_files_error), "%s (%d)", strerror(error), error);
}

static void files_tile_guard(lv_event_t *e)
{
  struct files_tile *tile=lv_event_get_user_data(e);
  lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_PRESSED){tile->pressed=true;tile->pressed_revision=tile->revision;}
  if(code==LV_EVENT_SHORT_CLICKED||code==LV_EVENT_LONG_PRESSED){
    bool stale=tile->pressed&&tile->pressed_revision!=tile->revision;
    tile->pressed=false;
    if(stale||!lv_obj_get_user_data(tile->button)||lv_obj_has_flag(tile->button,LV_OBJ_FLAG_HIDDEN))
      lv_event_stop_processing(e);
  }else if(code==LV_EVENT_PRESS_LOST)tile->pressed=false;
}

static void files_grid_pointer(lv_event_t *e)
{
  lv_dir_t direction=glass_swipe_direction(e,&g_files_drag);
  int delta=direction==LV_DIR_LEFT||direction==LV_DIR_TOP?1:
            direction==LV_DIR_RIGHT||direction==LV_DIR_BOTTOM?-1:0;
  if(!delta)return;
  /* Release-based recognition also accepts slow drags. Tile guards consume
   * the resulting short click, including swipes at either page boundary. */
  if(g_files_pending)return;
  unsigned page=g_files_page;
  if(delta>0){if(!g_files_browser.has_next||page==UINT_MAX)return;page++;}
  else {if(!page)return;page--;}
  glass_files_page(NULL,page);
}

static void files_tile_set(size_t index,const struct glass_file_entry *entry)
{
  const int width=184,height=96,gap=16;
  const struct files_design *design=entry->directory?NULL:files_design(entry->name);
  struct files_tile *tile=&g_files_browser.tiles[index];
  tile->revision++;
  if(!tile->button) {
    tile->button=files_button(g_files_browser.list,entry->name,
      (index%4)*(width+gap),(index/4)*(height+gap),width,detail_paper(),NULL,NULL);
    lv_obj_set_height(tile->button,height);
    lv_obj_add_flag(tile->button,LV_OBJ_FLAG_PRESS_LOCK|LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_color(tile->button,lv_color_hex(detail_paper()),0);
    lv_obj_set_style_radius(tile->button,12,0);
    lv_obj_add_event_cb(tile->button,files_tile_guard,LV_EVENT_ALL,tile);
    lv_obj_add_event_cb(tile->button,glass_files_select,LV_EVENT_SHORT_CLICKED,NULL);
    lv_obj_add_event_cb(tile->button,glass_files_select,LV_EVENT_LONG_PRESSED,NULL);
    tile->name=lv_obj_get_child(tile->button,0);
    lv_obj_set_width(tile->name,width-32);lv_obj_set_height(tile->name,24);
    lv_obj_set_style_text_font(tile->name,home_font(16),0);
    lv_obj_set_style_text_align(tile->name,LV_TEXT_ALIGN_LEFT,0);
    lv_obj_align(tile->name,LV_ALIGN_TOP_LEFT,16,64);
    lv_label_set_long_mode(tile->name,LV_LABEL_LONG_DOT);
    tile->image=lv_image_create(tile->button);
    lv_obj_remove_flag(tile->image,LV_OBJ_FLAG_CLICKABLE);
    tile->type=glass_text(tile->button,"",72,14,width-88,14,detail_muted());
    lv_label_set_long_mode(tile->type,LV_LABEL_LONG_CLIP);
  }
  const lv_image_dsc_t *image=entry->directory?&files_folder_image:files_type_images[design->kind];
  if(lv_image_get_src(tile->image)!=image)lv_image_set_src(tile->image,image);
  lv_obj_set_pos(tile->image,entry->directory?16:20,12);
  /* LONG_DOT can temporarily replace filename bytes with dots. */
  lv_label_set_text(tile->name,entry->name);
  const char *type=entry->directory?glass_tr("文件夹"):design->label;
  if(lv_label_get_text(tile->type)!=type)lv_label_set_text_static(tile->type,type);
  lv_obj_set_user_data(tile->button,(void *)entry);
  lv_obj_remove_flag(tile->button,LV_OBJ_FLAG_HIDDEN);
}
static void files_pager_update(bool has_next)
{
  g_files_browser.has_next=has_next;
  unsigned first=g_files_page>3?g_files_page-3:0;
  unsigned last=g_files_page+(has_next&&g_files_page<UINT_MAX?1:0);
  glass_pager_update_window(g_files_browser.pager,g_files_page,first,last-first+1,true);
  if(g_files_browser.pager)lv_obj_align(g_files_browser.pager,LV_ALIGN_BOTTOM_MID,0,-20);
}
static void glass_files_list_ready(struct glass_file_job *job,
                                   const struct glass_file_result *result)
{
  glass_files_list_reset();g_files_list_job=job;
  size_t shown=result?result->count:0;
  if(shown>GLASS_FILE_PAGE_SIZE)shown=GLASS_FILE_PAGE_SIZE;
  for(size_t i=0;i<shown;i++)files_tile_set(i,&result->entries[i]);
  if(!shown)files_list_note(result?glass_tr("目录为空"):glass_tr("目录无法读取"));
  /* Only known reachable pages are shown; counting dots never scans SD. */
  files_pager_update(result&&result->has_next);
  char footer[160];
  if(result&&!result->space_error)
    snprintf(footer,sizeof(footer),glass_tr("%u 项 · 可用 %llu MiB · 长按管理"),
             (unsigned)shown,(unsigned long long)(result->free_bytes/(1024*1024)));
  else snprintf(footer,sizeof(footer),glass_tr("%u 项 · 长按管理"),(unsigned)shown);
  lv_label_set_text(g_files_browser.footer,footer);
  lv_label_set_text(g_files_browser.error,g_files_error);
}

static void glass_files_complete(const struct glass_file_status *status)
{
  struct glass_file_job *job = g_files_job;
  enum glass_file_action action = g_files_request.action;
  const struct glass_file_result *result = glass_file_get_result(job);
  g_files_job = NULL;
  glass_files_stop();
  int error = status->error;
  if (!error && !result) error = EIO;
  if(error) {
    glass_files_error(error);g_files_list_error=action==GLASS_FILE_LIST;
  } else if(action!=GLASS_FILE_LIST || g_files_list_error) {
    /* Recovery clears a failed LIST. Keep a mutation error through its
     * automatic directory reload so the user can still see what failed. */
    g_files_error[0]=0;g_files_list_error=false;
  }
  if (status->cleanup_error)
    snprintf(g_files_error, sizeof(g_files_error), glass_tr("%s (%d)，清理失败 (%d)"),
             strerror(error), error, status->cleanup_error);
  if (action == GLASS_FILE_LIST)
    {
      glass_files_list_ready(error ? NULL : job, error ? NULL : result);
      if (error) glass_file_release(job);
      return;
    }
  if (action == GLASS_FILE_DETAILS && !error)
    {
      glass_files_details_ready(result);
      /* LVGL copies the preview; no result remains owned by this view. */
      glass_file_release(job);
      return;
    }
  if (!error && (action == GLASS_FILE_MOVE ||
      ((action == GLASS_FILE_RENAME || action == GLASS_FILE_REMOVE) &&
       !strcmp(g_files_clipboard, g_files_request.source))))
    {
      g_files_clipboard[0] = 0;
      g_files_clipboard_move = false;
    }
  glass_file_release(job);
  if (action != GLASS_FILE_DETAILS) g_files_page = 0;
  glass_files_refresh(NULL);
}

static int glass_files_submit(void)
{
  struct glass_file_request request =
    {
      .action = g_files_request.action,
      .root = g_files_root,
      .path = g_files_request.path,
      .source = g_files_request.source,
      .name = g_files_request.name,
      .page = g_files_request.page
    };
  int error = glass_file_submit(&request, &g_files_job);
  g_files_retry = error == EBUSY;
  return error;
}

static void glass_files_tick(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  struct glass_file_status status = {0};
  if (g_files_retry)
    {
      int error = glass_files_submit();
      if (error == EBUSY) return;
      if (error)
        {
          status.error = error;
          glass_files_complete(&status);
          return;
        }
    }
  int error = glass_file_poll(g_files_job, &status);
  if (error) status.error = error;
  if (status.error || status.done)
    {
      glass_files_complete(&status);
      return;
    }
  if (g_files_copy_status && g_files_copy_bytes != status.bytes)
    {
      g_files_copy_bytes = status.bytes;
      lv_label_set_text_fmt(g_files_copy_status, "%llu bytes",
                           (unsigned long long)status.bytes);
    }
}

static void glass_files_begin(enum glass_file_action action, const char *path,
                              const char *source, const char *name)
{
  g_files_request.action = action;
  g_files_request.page = g_files_page;
  int error = 0;
  if (strlcpy(g_files_request.path, path ? path : "", sizeof(g_files_request.path)) >=
      sizeof(g_files_request.path) ||
      strlcpy(g_files_request.source, source ? source : "", sizeof(g_files_request.source)) >=
      sizeof(g_files_request.source) ||
      strlcpy(g_files_request.name, name ? name : "", sizeof(g_files_request.name)) >=
      sizeof(g_files_request.name)) error = ENAMETOOLONG;
  g_files_pending = true;
  if (!error)
    {
      g_files_timer = lv_timer_create(glass_files_tick, 100, NULL);
      if (!g_files_timer) error = ENOMEM;
    }
  if (!error) error = glass_files_submit();
  /* A cancelled worker may still be closing or syncing. Retry without I/O. */
  if (error && error != EBUSY)
    {
      struct glass_file_status status = {.error = error};
      glass_files_complete(&status);
    }
}

static void glass_files_copy_choose(lv_event_t *e)
{
  if (g_files_pending) return;
  g_files_clipboard_move = e && lv_event_get_user_data(e) != NULL;
  strlcpy(g_files_clipboard, g_files_selected, sizeof(g_files_clipboard));
  glass_files_refresh(NULL);
}

static void glass_files_copy_start(void *unused)
{
  LV_UNUSED(unused);
  char target[GLASS_FILE_PATH_CAP];
  const char *name = strrchr(g_files_clipboard, '/');
  if (!glass_files_path_valid(g_files_path) ||
      !glass_files_path_valid(g_files_clipboard) || !name ||
      !glass_files_name(name + 1))
    { glass_files_error(EINVAL); glass_files_render(NULL); return; }
  if (snprintf(target, sizeof(target), "%s/%s", g_files_path, name + 1) >=
      (int)sizeof(target))
    { glass_files_error(ENAMETOOLONG); glass_files_render(NULL); return; }
  lv_obj_t *card = glass_files_card(g_files_clipboard_move ? glass_tr("移动文件") : glass_tr("复制文件"));
  if (!card) return;
  glass_text(card, target, 24, 80, 784, 20, theme_primary());
  g_files_copy_status = glass_text(card, "0 bytes", 24, 140, 784, 20, theme_secondary());
  g_files_copy_bytes = 0;
  files_button(card, glass_tr("取消"), 24, 200, 100, theme_surface(), glass_files_cancel, NULL);
  glass_files_begin(g_files_clipboard_move ? GLASS_FILE_MOVE : GLASS_FILE_COPY,
                    g_files_path, g_files_clipboard, NULL);
}

static void glass_files_paste(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_files_pending && lv_async_call(glass_files_copy_start, NULL) == LV_RESULT_OK)
    g_files_pending = true;
}

static void glass_files_clipboard_clear(lv_event_t *e)
{
  LV_UNUSED(e);
  g_files_clipboard[0] = 0;
  g_files_clipboard_move = false;
  glass_files_cancel(NULL);
}

static void glass_files_storage(lv_event_t *e)
{
  const char *root = lv_event_get_user_data(e);
  if (g_files_pending || !root ||
      (strcmp(root, GLASS_FILES_ROOT) &&
       strcmp(root, GLASS_FILES_SDCARD_ROOT)) ||
      !strcmp(root, g_files_root)) return;
  g_files_root = root;
  strlcpy(g_files_path, root, sizeof(g_files_path));
  g_files_selected[0] = 0;
  g_files_clipboard[0] = 0;
  g_files_clipboard_move = false;
  g_files_error[0] = 0;
  g_files_page = 0;
  glass_files_refresh(NULL);
}

static void glass_files_up(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_files_pending) return;
  if (strcmp(g_files_path, g_files_root))
    {
      char *slash = strrchr(g_files_path, '/');
      if (slash) *slash = 0;
    }
  g_files_page = 0;
  glass_files_refresh(NULL);
}

static void glass_files_page(void *context,unsigned page)
{
  (void)context;if(g_files_pending||page==g_files_page)return;
  g_files_page=page;glass_files_refresh(NULL);
}

static void glass_files_apply(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_files_pending) return;
  if (g_files_operation == 3)
    {
      glass_files_begin(GLASS_FILE_REMOVE, NULL, g_files_selected, NULL);
      return;
    }
  if (!g_files_input) return;
  glass_ime_commit_tree(lv_obj_get_parent(g_files_input));
  const char *name = lv_textarea_get_text(g_files_input);
  if (!glass_files_name(name))
    {
      glass_files_error(strlen(name) >= GLASS_FILE_NAME_CAP ? ENAMETOOLONG : EINVAL);
      glass_files_refresh(NULL);
      return;
    }
  if (g_files_operation == 1)
    glass_files_begin(GLASS_FILE_MKDIR, g_files_path, NULL, name);
  else
    {
      char parent[GLASS_FILE_PATH_CAP];
      strlcpy(parent, g_files_selected, sizeof(parent));
      char *slash = strrchr(parent, '/');
      if (!slash) { glass_files_error(EINVAL); glass_files_refresh(NULL); return; }
      *slash = 0;
      glass_files_begin(GLASS_FILE_RENAME, parent, g_files_selected, name);
    }
}

static void glass_files_dialog_async(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = glass_files_card(g_files_operation == 1 ? glass_tr("新建目录") :
                            g_files_operation == 2 ? glass_tr("重命名") : glass_tr("确认删除"));
  if (!card) return;
  glass_text(card, g_files_operation == 1 ? g_files_path : g_files_selected,
             24, 72, 784, 20, theme_secondary());
  if (g_files_operation != 3)
    {
      g_files_input = lv_textarea_create(card);      glass_input_style(g_files_input);
      lv_obj_set_pos(g_files_input, 24, 110);
      lv_obj_set_size(g_files_input, 560, 48);
      lv_textarea_set_one_line(g_files_input, true);
      lv_textarea_set_max_length(g_files_input, GLASS_FILE_NAME_CAP - 1);
      const char *name = strrchr(g_files_selected, '/');
      if (g_files_operation == 2 && name) lv_textarea_set_text(g_files_input, name + 1);
      lv_obj_t *keyboard = lv_keyboard_create(card);
      glass_keyboard_style(keyboard);
      lv_obj_set_size(keyboard, 784, 230);
      lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 24, 210);
      lv_keyboard_set_textarea(keyboard, g_files_input);
      glass_ime_attach(keyboard, zh_font(20), true, theme_primary(), theme_surface(), theme_accent());
    }
  files_button(card, glass_tr("取消"), 600, 110, 90, theme_surface(), glass_files_cancel, NULL);
  files_button(card, glass_tr("确认"), 704, 110, 90, theme_surface(), glass_files_apply, NULL);
}

static void glass_files_dialog(lv_event_t *e)
{
  if (g_files_pending) return;
  g_files_operation = (intptr_t)lv_event_get_user_data(e);
  if (lv_async_call(glass_files_dialog_async, NULL) == LV_RESULT_OK)
    g_files_pending = true;
}

static void glass_files_details_ready(const struct glass_file_result *result)
{
  lv_obj_t *card = glass_files_card(glass_tr("文件详情"));
  if (!card) return;
  files_material(card,g_files_selected,result->directory,24,70);
  lv_obj_t *filename=glass_text(card,g_files_selected,84,72,724,20,theme_primary());
  lv_obj_set_height(filename,28);
  files_button(card, glass_tr("返回"), 24, 110, 100, theme_surface(), glass_files_cancel, NULL);
  files_button(card, glass_tr("重命名"), 140, 110, 120, theme_surface(), glass_files_dialog, (void *)2);
  files_button(card, glass_tr("删除"), 280, 110, 100, theme_surface(), glass_files_dialog, (void *)3);
  char size[64];
  snprintf(size, sizeof(size), "%llu bytes", (unsigned long long)result->size);
  glass_text(card, size, 400, 118, 150, 20, theme_secondary());
  if (!result->directory)
    {
      files_button(card, glass_tr("复制"), 572, 110, 100, theme_surface(), glass_files_copy_choose, NULL);
      files_button(card, glass_tr("移动"), 688, 110, 100, theme_surface(), glass_files_copy_choose, (void *)1);
      if(result->binary) {
        lv_obj_t *preview=glass_box(card,24,170,784,GLASS_CONTENT_HEIGHT-194,detail_paper(),14);
        files_material(preview,g_files_selected,false,372,60);
        lv_obj_t *type=glass_text(preview,files_type(g_files_selected),48,114,688,20,detail_ink());
        lv_obj_set_style_text_align(type,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_t *note=glass_text(preview,glass_tr("此文件不提供文本预览"),48,160,688,16,detail_ink());
        lv_obj_set_style_text_align(note,LV_TEXT_ALIGN_CENTER,0);
        note=glass_text(preview,glass_tr("可使用上方操作管理文件"),48,196,688,14,detail_muted());
        lv_obj_set_style_text_align(note,LV_TEXT_ALIGN_CENTER,0);
      } else {
        lv_obj_t *preview=lv_textarea_create(card);glass_input_style(preview);
        lv_obj_set_pos(preview,24,170);lv_obj_set_size(preview,784,GLASS_CONTENT_HEIGHT-222);
        lv_textarea_set_text(preview,result->preview);
        lv_obj_remove_flag(preview,LV_OBJ_FLAG_CLICKABLE);
      }
      if (result->truncated && !result->binary)
        glass_text(card, glass_tr("仅预览前 2048 字节"), 24, GLASS_CONTENT_HEIGHT-42, 784, 14, theme_secondary());
    }
}

static void glass_files_details(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = glass_files_card(glass_tr("文件详情"));
  if (!card) return;
  lv_obj_t *filename=glass_text(card,g_files_selected,24,72,784,20,theme_primary());
  lv_obj_set_height(filename,28);
  files_button(card, glass_tr("返回"), 24, 110, 100, theme_surface(), glass_files_cancel, NULL);
  glass_text(card, glass_tr("正在读取"), 24, 170, 784, 20, theme_secondary());
  glass_files_begin(GLASS_FILE_DETAILS, NULL, g_files_selected, NULL);
}

/*---------------------------------------------------------------------------
 * Photo viewer
 *
 * This build has no LVGL image decoder, so a JPEG is decoded with the same
 * verified decoder the camera preview uses and shown on an RGB565 canvas.
 * Images larger than the card are reduced by whole MCU blocks first, which
 * keeps the displayed pixels sharp instead of stretching them.
 *--------------------------------------------------------------------------*/
#define GLASS_FILES_IMAGE_LIMIT (8u * 1024u * 1024u)
static uint16_t *g_files_image_pixels;

static bool glass_files_image_name(const char *name)
{
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  return local_album_kind(name)==ALBUM_PHOTO;
#else
  const char *dot = strrchr(name, '.');
  if (dot == NULL) return false;
  return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg");
#endif
}

static void glass_files_image_deleted(lv_event_t *e)
{
  if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
  free(g_files_image_pixels);
  g_files_image_pixels = NULL;
}

static int glass_files_image_read(const char *path, uint8_t **data, size_t *size)
{
  FILE *file = fopen(path, "rb");
  if (file == NULL) return errno ? errno : EIO;
  int error = 0;
  if (fseek(file, 0, SEEK_END) != 0) error = EIO;
  else
    {
      long length = ftell(file);
      if (length <= 0) error = EINVAL;
      else if ((unsigned long)length > GLASS_FILES_IMAGE_LIMIT) error = EOVERFLOW;
      else if (fseek(file, 0, SEEK_SET) != 0) error = EIO;
      else
        {
          /* Room for the standard Huffman tables a UVC frame may omit. */
          uint8_t *buffer = malloc((size_t)length + 512);
          if (buffer == NULL) error = ENOMEM;
          else if (fread(buffer, 1, (size_t)length, file) != (size_t)length)
            { free(buffer); error = EIO; }
          else
            { *data = buffer; *size = (size_t)length; }
        }
    }
  if (fclose(file) != 0 && !error) error = EIO;
  return error;
}

static void glass_files_image(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = glass_files_card(glass_tr("图片查看"));
  if (card == NULL) return;
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  local_album_view(card,home_font(16),g_files_selected,glass_files_cancel);
  return;
#else
  const char *slash = strrchr(g_files_selected, '/');
  if (slash != NULL && slash[1] != '\0') lv_label_set_text(g_files_title, slash + 1);
  uint8_t *data = NULL;
  size_t size = 0;
  unsigned width = 0, height = 0, scale = 0;
  int error = glass_files_image_read(g_files_selected, &data, &size);
  if (error == 0) error = qpk_mjpeg_probe(data, size, &width, &height);
  lv_obj_update_layout(card);
  unsigned limit_w = lv_obj_get_width(card);
  unsigned limit_h = lv_obj_get_height(card);
  while (error == 0 && scale < 3 &&
         ((width >> scale) > limit_w || (height >> scale) > limit_h)) scale++;
  unsigned canvas_w = width >> scale, canvas_h = height >> scale;
  uint16_t *pixels = NULL;
  void *decoder = NULL;
  if (error == 0 && (!canvas_w || !canvas_h)) error = EINVAL;
  if (error == 0)
    {
      pixels = malloc((size_t)canvas_w * canvas_h * sizeof(uint16_t));
      decoder = qpk_mjpeg_create();
      if (pixels == NULL || decoder == NULL) error = ENOMEM;
    }
  if (error == 0 && qpk_mjpeg_decode(decoder, data, size, size + 512, pixels,
                                     canvas_w, canvas_h, width, height) < 0)
    error = EBADMSG;
  qpk_mjpeg_destroy(decoder);
  free(data);
  if (error)
    {
      free(pixels);
      char message[64];
      snprintf(message, sizeof(message), glass_tr("无法显示该图片：%s (%d)"), strerror(error), error);
      glass_text(card, message, 24, 170, 784, 20, theme_secondary());
      files_button(card, glass_tr("返回"), 24, 110, 100, theme_surface(), glass_files_cancel, NULL);
      return;
    }
  lv_obj_t *canvas = lv_canvas_create(card);
  if (canvas == NULL)
    {
      free(pixels);
      glass_text(card, glass_tr("内存不足，无法显示该图片"), 24, 170, 784, 20, theme_secondary());
      files_button(card, glass_tr("返回"), 24, 110, 100, theme_surface(), glass_files_cancel, NULL);
      return;
    }
  g_files_image_pixels = pixels;
  lv_canvas_set_buffer(canvas, pixels, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas, ((int)limit_w - (int)canvas_w) / 2,
                 ((int)limit_h - (int)canvas_h) / 2);
  lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, glass_files_image_deleted, LV_EVENT_DELETE, NULL);
  /* Floating footer keeps a way back for images that fill the whole card. */
  files_button(card, glass_tr("返回"), (int)limit_w - 124, (int)limit_h - 60, 100,
               theme_surface(), glass_files_cancel, NULL);
  if (scale)
    {
      char note[64];
      snprintf(note, sizeof(note), glass_tr("%u x %u · 已缩放显示"), canvas_w, canvas_h);
      glass_text(card, note, 24, (int)limit_h - 52, 320, 20, theme_secondary());
    }
#endif
}

static void glass_files_select(lv_event_t *e)
{
  if (g_files_pending) return;
  const struct glass_file_entry *entry = lv_obj_get_user_data(lv_event_get_target(e));
  if (!entry || !glass_files_name(entry->name)) return;
  if (snprintf(g_files_selected, sizeof(g_files_selected), "%s/%s",
               g_files_path, entry->name) >= (int)sizeof(g_files_selected))
    { glass_files_error(ENAMETOOLONG); glass_files_refresh(NULL); return; }
  if (entry->directory && lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED)
    {
      strlcpy(g_files_path, g_files_selected, sizeof(g_files_path));
      g_files_page = 0;
      glass_files_refresh(NULL);
    }
  else
    {
      /* Decoding a photo on the LVGL thread would stall the desktop, so the
       * viewer is entered the same way as the details card. */
      bool media=!entry->directory&&glass_files_image_name(entry->name);
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
      media|=!entry->directory&&local_album_kind(entry->name)==ALBUM_VIDEO;
#endif
      lv_async_cb_t next=media&&lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED?glass_files_image:glass_files_details;
      if (lv_async_call(next, NULL) == LV_RESULT_OK) g_files_pending = true;
    }
}

static void glass_files_render(void *unused)
{
  LV_UNUSED(unused);
  glass_files_cancel_calls();
  glass_files_stop();
  if (!g_files_browser.card)
    {
      lv_obj_t *card = glass_files_card(glass_tr("文件管理"));
      if (!card) return;
      g_files_browser.card = card;
      /* The first content container becomes the retained browser view. */
      g_files_auxiliary = NULL;
      lv_obj_set_style_radius(card, 0, 0);
      g_files_browser.path = glass_text(card, "", 24, 76, 784, 16, theme_primary());
      g_files_browser.up = glass_files_icon(card, LV_SYMBOL_UP, 24, 108, glass_files_up, NULL);
      glass_files_icon(card, LV_SYMBOL_REFRESH, 84, 108, glass_files_refresh, NULL);
      files_button(card, glass_tr("新建目录"), 144, 108, 130, theme_surface(), glass_files_dialog, (void *)1);
      g_files_browser.error = glass_text(card, "", 554, 118, 230, 16, theme_secondary());
      g_files_browser.cancel = files_button(card, glass_tr("取消"), 564, GLASS_CONTENT_HEIGHT-62, 96, theme_surface(), glass_files_clipboard_clear, NULL);
      g_files_browser.paste = files_button(card, glass_tr("粘贴"), 672, GLASS_CONTENT_HEIGHT-62, 136, theme_surface(), glass_files_paste, NULL);
      g_files_browser.list = glass_box(card, 24, 164, 784, GLASS_CONTENT_HEIGHT-244, detail_canvas(), 0);
      /* A page is a fixed 4x3 grid. Swipes select pages, not elastic scroll. */
      lv_obj_remove_flag(g_files_browser.list,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_ELASTIC|
        LV_OBJ_FLAG_SCROLL_MOMENTUM|LV_OBJ_FLAG_GESTURE_BUBBLE);
      lv_obj_add_flag(g_files_browser.list,LV_OBJ_FLAG_PRESS_LOCK);
      lv_obj_set_scroll_dir(g_files_browser.list,LV_DIR_NONE);
      lv_obj_set_scrollbar_mode(g_files_browser.list,LV_SCROLLBAR_MODE_OFF);
      lv_obj_add_event_cb(g_files_browser.list,files_grid_pointer,LV_EVENT_ALL,NULL);
      g_files_browser.pager=glass_pager_create(card,detail_soft(),detail_ink(),detail_muted(),glass_files_page,NULL);
      if(g_files_browser.pager)lv_obj_align(g_files_browser.pager,LV_ALIGN_BOTTOM_MID,0,-20);
      g_files_browser.footer=glass_text(card,"",24,GLASS_CONTENT_HEIGHT-48,350,14,theme_secondary());
    }
  g_files_input = NULL;
  if (g_files_auxiliary)
    {
      lv_obj_clean(g_files_auxiliary);
      lv_obj_add_flag(g_files_auxiliary, LV_OBJ_FLAG_HIDDEN);
    }
  lv_obj_remove_flag(g_files_browser.card, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(g_files_title, glass_tr("文件管理"));
  lv_label_set_text(lv_obj_get_child(g_files_browser.back,0),strcmp(g_files_path,g_files_root)?glass_tr("返回上级"):g_files_return_apps?glass_tr("返回应用"):glass_tr("返回桌面"));
  lv_label_set_text(g_files_browser.path, g_files_path);
  lv_label_set_text(g_files_browser.error, g_files_error);
  bool sdcard = !strcmp(g_files_root, GLASS_FILES_SDCARD_ROOT);
  lv_obj_t *volumes[]={g_files_browser.device,g_files_browser.sdcard};
  for(unsigned i=0;i<2;i++){
    bool active=i==(unsigned)sdcard;glass_sidebar_item(volumes[i],active);
    for(unsigned n=0;n<lv_obj_get_child_count(volumes[i]);n++){
      lv_obj_t *child=lv_obj_get_child(volumes[i],n);
      if(lv_obj_has_class(child,&lv_label_class))lv_obj_set_style_text_color(child,lv_color_hex(active?detail_accent():detail_ink()),0);
    }
  }
  lv_label_set_text(lv_obj_get_child(g_files_browser.paste, 0),
                    g_files_clipboard_move ? glass_tr("移到此处") : glass_tr("粘贴"));
  if (g_files_clipboard[0])
    {
      lv_obj_remove_flag(g_files_browser.cancel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_files_browser.paste, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(g_files_browser.cancel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_files_browser.paste, LV_OBJ_FLAG_HIDDEN);
    }
  if (!strcmp(g_files_path, g_files_root))
    lv_obj_add_state(g_files_browser.up, LV_STATE_DISABLED);
  else lv_obj_remove_state(g_files_browser.up, LV_STATE_DISABLED);
  /* Reuse tile widgets while the browser remains open. */
  glass_files_list_reset();
  lv_obj_scroll_to_y(g_files_browser.list, 0, LV_ANIM_OFF);
  files_list_note(glass_tr("正在读取目录"));
  glass_pager_set_enabled(g_files_browser.pager,false);
  lv_label_set_text(g_files_browser.footer, "");
  glass_files_begin(GLASS_FILE_LIST, g_files_path, NULL, NULL);
}
