/* SPDX-License-Identifier: Apache-2.0 */
/* Device result cards for smart home tool calls: read-only mirrors of the
 * native panel cards, rendered inside the dialog. No touch targets, no page. */
#include "glass_icons.h"
#define CHAT_CARD_W 344
#define CHAT_CARD_H 76
#define CHAT_CARD_GAP 8

static const char *chat_card_icon(const char *kind)
{
  if(!strcmp(kind,"music"))return GLASS_ICON_HEADPHONES;
  if(!strcmp(kind,"light"))return GLASS_ICON_DEVICE_LAMP;
  if(!strcmp(kind,"switch"))return GLASS_ICON_DEVICE_PLUG;
  if(!strcmp(kind,"fan"))return GLASS_ICON_DEVICE_FAN;
  if(!strcmp(kind,"sensor"))return GLASS_ICON_DEVICE_SENSOR;
  if(!strcmp(kind,"binary"))return GLASS_ICON_DEVICE_CONTACT;
  if(!strcmp(kind,"climate"))return GLASS_ICON_DEVICE_CLIMATE;
  if(!strcmp(kind,"media"))return GLASS_ICON_DEVICE_MEDIA;
  if(!strcmp(kind,"cover"))return GLASS_ICON_DEVICE_CURTAIN;
  if(!strcmp(kind,"vacuum"))return GLASS_ICON_DEVICE_VACUUM;
  if(!strcmp(kind,"camera"))return GLASS_ICON_DEVICE_CAMERA;
  if(!strcmp(kind,"humidifier"))return GLASS_ICON_DEVICE_HUMIDIFIER;
  if(!strcmp(kind,"heater"))return GLASS_ICON_DEVICE_HEATER;
  if(!strcmp(kind,"lock"))return GLASS_ICON_DEVICE_LOCK;
  return GLASS_ICON_DEVICE_GENERIC;
}
static int chat_ui_cards_height(const struct chat_turn *turn)
{
  if(!turn||!turn->card_count)return 0;
  unsigned rows=(turn->card_count+1)/2;
  return (int)(rows*CHAT_CARD_H+(rows-1)*CHAT_CARD_GAP);
}
static uint32_t chat_ui_cards_signature(const struct chat_turn *turn)
{
  uint32_t hash=2166136261u;
  for(unsigned i=0;i<turn->card_count;i++){
    const struct chat_card *card=&turn->cards[i];
    for(const char *p=card->entity;*p;p++)hash=(hash^(uint32_t)(unsigned char)*p)*16777619u;
    for(const char *p=card->kind;*p;p++)hash=(hash^(uint32_t)(unsigned char)*p)*16777619u;
    for(const char *p=card->name;*p;p++)hash=(hash^(uint32_t)(unsigned char)*p)*16777619u;
    for(const char *p=card->state;*p;p++)hash=(hash^(uint32_t)(unsigned char)*p)*16777619u;
    hash=(hash^(uint32_t)card->level)*16777619u;hash=(hash^(card->on?1u:0u))*16777619u;
  }
  return hash?hash:1u;
}
static lv_obj_t *chat_ui_cards_build(const struct chat_turn *turn,int y)
{
  lv_obj_t *root=lv_obj_create(g_chat_ui.messages);
  lv_obj_set_pos(root,96,y);lv_obj_set_size(root,700,chat_ui_cards_height(turn));
  lv_obj_set_style_bg_opa(root,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(root,0,0);
  lv_obj_set_style_pad_all(root,0,0);lv_obj_set_style_radius(root,0,0);
  lv_obj_remove_flag(root,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  uint32_t warm=g_desktop.light_theme?0xe0870f:0xf2b23d;
  uint32_t warm_bg=g_desktop.light_theme?0xfff0da:0x3a2e1c;
  uint32_t track=g_desktop.light_theme?0xe6e3da:0x35342f;
  for(unsigned i=0;i<turn->card_count;i++){
    const struct chat_card *card=&turn->cards[i];
    int cx=(int)(i%2)*(CHAT_CARD_W+CHAT_CARD_GAP),cy=(int)(i/2)*(CHAT_CARD_H+CHAT_CARD_GAP);
    lv_obj_t *box=glass_box(root,cx,cy,CHAT_CARD_W,CHAT_CARD_H,g_chat_ui.surface,16);
    lv_obj_remove_flag(box,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *badge=glass_box(box,14,16,44,44,card->on?warm_bg:g_chat_ui.sidebar,22);
    lv_obj_remove_flag(badge,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *glyph=glass_icon_create(badge);
    lv_obj_set_style_text_font(glyph,glass_icon_font(24),0);
    lv_obj_set_style_text_color(glyph,lv_color_hex(card->on?warm:g_chat_ui.secondary),0);
    lv_label_set_text(glyph,chat_card_icon(card->kind));lv_obj_center(glyph);
    chat_ui_text(box,card->name,68,8,CHAT_CARD_W-82,20,g_chat_ui.primary,false);
    char line[112];
    if(card->level>=0)snprintf(line,sizeof(line),"%s · %d%%",card->state,card->level);
    else snprintf(line,sizeof(line),"%s",card->state);
    chat_ui_text(box,line,68,36,CHAT_CARD_W-82,16,card->on?g_chat_ui.primary:g_chat_ui.secondary,false);
    if(card->level>=0){
      lv_obj_t *bar=glass_box(box,68,60,CHAT_CARD_W-82,6,track,3);
      lv_obj_remove_flag(bar,LV_OBJ_FLAG_CLICKABLE);
      if(card->level>0){
        int fill=(CHAT_CARD_W-82)*card->level/100;
        if(fill<3)fill=3;
        lv_obj_t *value=glass_box(bar,0,0,fill,6,card->on?warm:g_chat_ui.secondary,3);
        lv_obj_remove_flag(value,LV_OBJ_FLAG_CLICKABLE);
      }
    }
  }
  return root;
}
/* Cards of the live turn sit between the tool trace and the reply bubble so
 * streamed text never moves them. */
static void chat_ui_cards_attach(const struct chat_turn *turn)
{
  if(!turn||!turn->card_count)return;
  g_chat_ui.cards_root=chat_ui_cards_build(turn,g_chat_ui.live_y);
  g_chat_ui.cards_h=chat_ui_cards_height(turn);
  chat_ui_trace_update(turn);
}
/* Cards of older turns follow their reply. */
static int chat_ui_cards_inline(const struct chat_turn *turn,int y)
{
  if(!turn->card_count)return y;
  chat_ui_cards_build(turn,y);
  return y+chat_ui_cards_height(turn)+22;
}
