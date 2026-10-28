/* SPDX-License-Identifier: Apache-2.0 */
/* Text and fenced-code chunks share a bounded, reusable live object pool. */
extern const unsigned char glass_code_font_data[],glass_bold_font_data[];
extern const size_t glass_code_font_size,glass_bold_font_size;
static lv_font_t *chat_code_font,*chat_bold_font;
static const lv_font_t *chat_ui_mono(void)
{
  if(!chat_code_font){chat_code_font=lv_tiny_ttf_create_data_ex(glass_code_font_data,glass_code_font_size,16,LV_FONT_KERNING_NONE,96);
    if(chat_code_font){chat_code_font->line_height=24;chat_code_font->base_line=4;}}
  if(!chat_code_font)return zh_font(16);
  chat_code_font->fallback=zh_font(16);return chat_code_font;
}
static const lv_font_t *chat_ui_bold(void)
{
  if(!chat_bold_font){
    chat_bold_font=lv_tiny_ttf_create_data_ex(glass_bold_font_data,glass_bold_font_size,20,LV_FONT_KERNING_NORMAL,64);
    if(chat_bold_font)chat_bold_font->fallback=zh_font(20);
    glass_global_font_bind(chat_bold_font,20);
  }
  if(!chat_bold_font)return zh_font(20);
  if(!global_font_selected)chat_bold_font->fallback=zh_font(20);
  return chat_bold_font;
}
static void chat_ui_markdown(struct chat_output_part *part,const char *body,int width,uint32_t color)
{
  size_t skip;unsigned level;enum chat_md_kind kind=chat_md_prefix(body,&skip,&level);
  const char *content=body+skip;
  lv_obj_set_style_border_width(part->root,0,0);
  lv_obj_set_style_pad_left(part->root,0,0);
  if(kind==CHAT_MD_RULE){
    lv_obj_set_height(part->root,12);lv_obj_set_style_border_width(part->root,1,0);
    lv_obj_set_style_border_side(part->root,LV_BORDER_SIDE_TOP,0);
    lv_obj_set_style_border_color(part->root,lv_color_hex(g_chat_ui.secondary),0);
    lv_obj_set_style_border_opa(part->root,40,0);return;
  }
  if(kind==CHAT_MD_QUOTE){
    lv_obj_set_style_border_width(part->root,2,0);lv_obj_set_style_border_side(part->root,LV_BORDER_SIDE_LEFT,0);
    lv_obj_set_style_border_color(part->root,lv_color_hex(g_chat_ui.secondary),0);
    lv_obj_set_style_border_opa(part->root,80,0);lv_obj_set_style_pad_left(part->root,14,0);
  }
  uint32_t hash=2166136261u;for(const unsigned char *p=(const unsigned char *)body;*p;p++)hash=(hash^*p)*16777619u;
  if(part->signature==hash)return;
  part->signature=hash;
  unsigned index=0;struct chat_md_inline token;const char *cursor=content;
  if(kind==CHAT_MD_LIST&&!level){
    lv_span_t *span=lv_spangroup_get_child(part->root,index++);if(!span)span=lv_spangroup_new_span(part->root);
    lv_span_set_text(span,"•  ");lv_style_set_text_font(lv_span_get_style(span),zh_font(20));
  }
  while(chat_md_inline_next(&cursor,&token)){
    if(index==63){token.bytes=strlen(token.text);token.style=CHAT_MD_NORMAL;cursor=token.text+token.bytes;}
    char buffer[CHAT_MARKUP_BYTES*4+1];size_t n=token.bytes<sizeof(buffer)-1?token.bytes:sizeof(buffer)-1;
    memcpy(buffer,token.text,n);buffer[n]=0;
    lv_span_t *span=lv_spangroup_get_child(part->root,index++);if(!span)span=lv_spangroup_new_span(part->root);
    lv_style_t *style=lv_span_get_style(span);lv_style_reset(style);lv_span_set_text(span,buffer);
    lv_style_set_text_color(style,lv_color_hex(kind==CHAT_MD_QUOTE?g_chat_ui.secondary:color));
    const lv_font_t *font=kind==CHAT_MD_HEADING?zh_font(level<3?28:20):zh_font(20);
    if(token.style==CHAT_MD_CODE)font=chat_ui_mono();
    else if(token.style==CHAT_MD_STRONG&&kind!=CHAT_MD_HEADING)font=chat_ui_bold();
    lv_style_set_text_font(style,font);
    if(token.style==CHAT_MD_CODE){lv_style_set_bg_color(style,lv_color_hex(g_desktop.light_theme?0xeae8e3:0x333530));lv_style_set_bg_opa(style,LV_OPA_COVER);}
    if(token.style==CHAT_MD_LINK||token.style==CHAT_MD_EM)lv_style_set_text_decor(style,LV_TEXT_DECOR_UNDERLINE);
    if(token.style==CHAT_MD_STRIKE)lv_style_set_text_decor(style,LV_TEXT_DECOR_STRIKETHROUGH);
    if(index==64)break;
  }
  while(lv_spangroup_get_span_count(part->root)>index)lv_spangroup_delete_span(part->root,lv_spangroup_get_child(part->root,-1));
  lv_obj_set_width(part->root,width-36);lv_spangroup_refr_mode(part->root);
}
static const char *chat_ui_language(const char *tag)
{
  if(!strcmp(tag,"js")||!strcmp(tag,"javascript"))return "JavaScript";
  if(!strcmp(tag,"py")||!strcmp(tag,"python"))return "Python";
  if(!strcmp(tag,"json"))return "JSON";
  if(!strcmp(tag,"cpp")||!strcmp(tag,"c++"))return "C++";
  if(!strcmp(tag,"c"))return "C";
  return tag[0]?tag:"代码";
}
static int chat_ui_output_part(lv_obj_t *bubble,struct chat_output_part *p,
                               const struct chat_markup_part *token,int width,int y,
                               uint32_t color,bool live,bool whole_tail)
{
  bool rich=!token->code&&!whole_tail;
  if(p->root&&(p->code!=token->code||p->rich!=rich)){lv_obj_delete(p->root);memset(p,0,sizeof(*p));}
  p->code=token->code;
  p->rich=rich;
  char text[CHAT_MARKUP_BYTES*4+1];const char *body=text;
  size_t used=0,n=token->bytes;
  if(whole_tail)body=token->text;
  else {
    for(size_t i=0;i<n;i++){
      if(token->code&&token->text[i]=='\t'){memcpy(text+used,"    ",4);used+=4;}
      else text[used++]=token->text[i];
    }
    if(token->code&&used&&text[used-1]=='\n')used--;
    text[used]=0;
  }
  if(!p->root){
    if(p->rich){
      p->root=p->text=lv_spangroup_create(bubble);lv_obj_remove_style_all(p->root);
      lv_obj_set_pos(p->root,18,y);lv_obj_set_width(p->root,width-36);
      lv_obj_set_style_text_line_space(p->root,8,0);lv_obj_set_style_text_font(p->root,zh_font(20),0);
      lv_spangroup_set_mode(p->root,LV_SPAN_MODE_BREAK);lv_obj_remove_flag(p->root,LV_OBJ_FLAG_CLICKABLE);
    }
    else if(!p->code)p->root=p->text=chat_ui_text(bubble,"",18,y,width-36,20,color,true);
    else {
      uint32_t background=g_desktop.light_theme?0xefeee9:0x242522;
      p->root=glass_box(bubble,18,y,width-36,80,background,10);
      lv_obj_remove_flag(p->root,LV_OBJ_FLAG_CLICKABLE);
      p->caption=chat_ui_text(p->root,"",14,10,width-64,16,g_chat_ui.secondary,false);
      p->numbers=chat_ui_text(p->root,"",10,42,32,16,g_chat_ui.secondary,true);
      lv_obj_set_style_text_font(p->numbers,chat_ui_mono(),0);
      lv_obj_set_style_text_line_space(p->numbers,0,0);
      lv_obj_set_style_text_align(p->numbers,LV_TEXT_ALIGN_RIGHT,0);
      p->viewport=glass_box(p->root,54,42,width-104,24,background,0);
      lv_obj_add_flag(p->viewport,LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scroll_dir(p->viewport,LV_DIR_HOR);
      lv_obj_set_scrollbar_mode(p->viewport,LV_SCROLLBAR_MODE_AUTO);
      p->text=chat_ui_text(p->viewport,"",0,0,1,16,g_chat_ui.primary,false);
      lv_obj_set_style_text_font(p->text,chat_ui_mono(),0);
      lv_obj_set_style_text_line_space(p->text,0,0);
      lv_label_set_long_mode(p->text,LV_LABEL_LONG_CLIP);
      lv_obj_set_width(p->text,LV_SIZE_CONTENT);
    }
  }
  lv_obj_set_y(p->root,y);
  if(p->rich)chat_ui_markdown(p,body,width,color);
  else chat_ui_label(p->text,body);
  if(p->code){
    unsigned lines=1;for(const char *q=body;*q;q++)if(*q=='\n')lines++;
    char numbers[160],caption[100];size_t offset=0;
    for(unsigned i=0;i<lines&&offset+12<sizeof(numbers);i++)
      offset+=snprintf(numbers+offset,sizeof(numbers)-offset,"%s%u",i?"\n":"",token->line+i);
    if(!offset)numbers[0]=0;
    snprintf(caption,sizeof(caption),"%s  ·  L%u%s",chat_ui_language(token->language),token->line,
             live&&token->open?"  ·  生成中":"");
    chat_ui_label(p->numbers,numbers);chat_ui_label(p->caption,caption);
    lv_obj_set_height(p->viewport,lines*24+4);
    lv_obj_set_height(p->root,lines*24+58);
  }
  lv_obj_update_layout(p->root);
  return lv_obj_get_height(p->root);
}
static int chat_ui_chunks(lv_obj_t *bubble,const char *text,int width,uint32_t color,bool live)
{
  struct chat_markup it={.text=text,.cursor=text,.line=1,.markdown=true};struct chat_markup_part token;
  int y=16;unsigned index=0;
  while(index<CHAT_OUTPUT_PARTS&&chat_markup_next(&it,&token)){
    bool tail=index==CHAT_OUTPUT_PARTS-1&&*it.cursor;
    if(tail){token.code=false;token.bytes=strlen(token.text);}
    struct chat_output_part local={0},*part=live?&g_chat_ui.live_parts[index]:&local;
    y+=chat_ui_output_part(bubble,part,&token,width,y,color,live,tail)+8;
    if(live&&g_chat_ui.live_count<=index)g_chat_ui.live_count=index+1;
    index++;if(tail)break;
  }
  if(live)while(g_chat_ui.live_count>index){
    struct chat_output_part *part=&g_chat_ui.live_parts[--g_chat_ui.live_count];
    lv_obj_delete(part->root);memset(part,0,sizeof(*part));
  }
  int height=index?y+8:0;
  lv_obj_set_height(bubble,height);return height;
}
