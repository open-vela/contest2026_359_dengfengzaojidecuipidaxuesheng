/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_reading.h"
#include <string.h>
#include <dom/dom.h>
#include "netsurf/content.h"
#include "content/hlcache.h"
#include "content/handlers/html/private.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/html/css.h"
#include "content/handlers/html/rebuild.h"

#define SHEET_ID "ov-browser-reading-style"
static const char reader_css[] =
 "[data-ov-reader-path='1']{display:block!important;width:auto!important;min-width:0!important;"
 "max-width:100%!important;height:auto!important;position:static!important;float:none!important;"
 "margin:0!important;padding:0!important;box-sizing:border-box!important;transform:none!important;"
 "overflow:visible!important;background:#fff!important}"
 "[data-ov-reader-hide='1']{display:none!important}"
 "[data-ov-reader-root='1']{display:block!important;width:92%!important;min-width:0!important;"
 "max-width:760px!important;margin:12px auto!important;padding:0!important;"
 "font:18px/1.7 sans-serif!important;color:#222!important;background:#fff!important;"
 "text-align:left!important;overflow-wrap:anywhere!important;word-wrap:break-word!important}"
 "[data-ov-reader-root='1'] *{max-width:100%!important;min-width:0!important;"
 "box-sizing:border-box!important;position:static!important;float:none!important;"
 "width:auto!important;margin-left:0!important;margin-right:0!important;"
 "padding-left:0!important;padding-right:0!important;transform:none!important;"
 "font-size:inherit!important;line-height:inherit!important;"
 "white-space:normal!important;overflow-wrap:anywhere!important;word-wrap:break-word!important}"
 "[data-ov-reader-root='1'] p{margin:0 0 1em!important}"
 "[data-ov-reader-root='1'] img,[data-ov-reader-root='1'] svg{height:auto!important}"
 "[data-ov-reader-root='1'] table{width:100%!important;table-layout:fixed!important}"
 "[data-ov-reader-root='1'] pre{white-space:pre-wrap!important}"
 "[data-ov-reader-root='1'] input,[data-ov-reader-root='1'] textarea,"
 "[data-ov-reader-root='1'] button{width:100%!important;max-width:100%!important}"
 "[data-ov-reader-title='1']{display:block!important;font-size:24px!important;"
 "line-height:1.4!important;margin:0 0 16px!important;text-indent:0!important}"
 "[data-ov-reader-root='1'] iframe,[data-ov-reader-root='1'] video{display:none!important}";

static dom_string *string(const char *s)
{
 if(!s) s="";
 dom_string *v=NULL;
 if(dom_string_create((const uint8_t *)s,strlen(s),&v)!=DOM_NO_ERR) return NULL;
 return v;
}
static bool attribute(dom_node *node,const char *name,const char *value)
{
 dom_string *n=string(name),*v=string(value);
 bool ok=n && v && dom_element_set_attribute((dom_element *)node,n,v)==DOM_NO_ERR;
 if(n) dom_string_unref(n);if(v) dom_string_unref(v);return ok;
}
static bool remove_attribute(dom_node *node,const char *name)
{
 dom_string *key=string(name);
 bool ok=key && dom_element_remove_attribute((dom_element *)node,key)==DOM_NO_ERR;
 if(key) dom_string_unref(key);return ok;
}
static dom_node *by_id(dom_document *doc,const char *id)
{
 dom_string *s=string(id);dom_element *node=NULL;
 if(s) {dom_document_get_element_by_id(doc,s,&node);dom_string_unref(s);}
 return (dom_node *)node;
}
static dom_node *head(dom_document *doc)
{
 dom_string *tag=string("head");dom_nodelist *nodes=NULL;dom_node *node=NULL;
 if(tag) {
  if(dom_document_get_elements_by_tag_name(doc,tag,&nodes)==DOM_NO_ERR && nodes)
   dom_nodelist_item(nodes,0,&node);
  dom_string_unref(tag);
 }
 if(nodes)dom_nodelist_unref(nodes);return node;
}
static bool owned(dom_node *node)
{
 dom_string *key=string("data-ov-reader-owned"),*value=NULL;
 if(key) {dom_element_get_attribute((dom_element *)node,key,&value);dom_string_unref(key);}
 bool result=value && dom_string_byte_length(value)==1 && dom_string_data(value)[0]=='1';
 if(value)dom_string_unref(value);return result;
}
static dom_node *article(dom_document *doc)
{
 const char *ids[]={"article-content","article","artibody","mp-editor","endText"};
 for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
  dom_node *node=by_id(doc,ids[i]);
  /* Only visible, substantial article bodies. Hidden alternatives stay hidden. */
  if(node) {
   dom_string *text=NULL;bool ok=box_for_node(node)!=NULL &&
    dom_node_get_text_content(node,&text)==DOM_NO_ERR && text && dom_string_byte_length(text)>=200;
   if(text) dom_string_unref(text);if(ok) return node;dom_node_unref(node);
  }
 }
 return NULL;
}
static bool mark_path(dom_node *root)
{
 dom_node *node=dom_node_ref(root);unsigned depth=0;
 while(node && depth++<64) {
  dom_node_type type;
  if(dom_node_get_node_type(node,&type)!=DOM_NO_ERR || type!=DOM_ELEMENT_NODE) break;
  /* A previous article may have hidden this branch. The newly selected
   * path must be visible before its reader rules become active again. */
  if(!remove_attribute(node,"data-ov-reader-hide") ||
     !remove_attribute(node,"data-ov-reader-root") ||
     !attribute(node,"data-ov-reader-path","1")) {dom_node_unref(node);return false;}
  dom_node *parent=NULL,*sibling=NULL;
  if(dom_node_get_parent_node(node,&parent)!=DOM_NO_ERR) {dom_node_unref(node);return false;}
  if(parent) dom_node_get_first_child(parent,&sibling);
  unsigned count=0;
  while(sibling && count++<4096) {
   dom_node *next=NULL;dom_node_get_next_sibling(sibling,&next);
   dom_node_type st;
   if(sibling!=node && dom_node_get_node_type(sibling,&st)==DOM_NO_ERR && st==DOM_ELEMENT_NODE) {
    /* The root rule follows the hide rule; a former sibling root must
     * lose that rule before it can be hidden for the new article. */
    if(!remove_attribute(sibling,"data-ov-reader-root") ||
       !attribute(sibling,"data-ov-reader-hide","1")) {
     if(next) dom_node_unref(next);
     dom_node_unref(sibling);dom_node_unref(parent);dom_node_unref(node);return false;
    }
   }
   dom_node_unref(sibling);sibling=next;
  }
  if(sibling) dom_node_unref(sibling);
  dom_node_unref(node);node=parent;
 }
 if(node) dom_node_unref(node);
 return attribute(root,"data-ov-reader-root","1");
}
static bool set_text(dom_node *node,const char *text)
{
 dom_string *s=string(text);if(!s) return false;
 bool ok=dom_node_set_text_content(node,s)==DOM_NO_ERR;dom_string_unref(s);return ok;
}
static dom_node *element(dom_document *doc,const char *name)
{
 dom_string *s=string(name);dom_element *node=NULL;
 if(s) {dom_document_create_element(doc,s,&node);dom_string_unref(s);}
 return (dom_node *)node;
}
bool ov_reading_apply(struct hlcache_handle *content,bool enabled,bool *available)
{
 *available=false;
 if(!content || content_get_type(content)!=CONTENT_HTML) return false;
 html_content *html=(html_content *)hlcache_handle_get_content(content);
 dom_document *doc=html->document;
 dom_node *sheet=by_id(doc,SHEET_ID),*root=article(doc);
 if(sheet && !owned(sheet)) {
  dom_node_unref(sheet);if(root) dom_node_unref(root);return false;
 }
 *available=root!=NULL;
 /* Webpage mutations can rename, shorten or remove the selected body.
  * Restoring author presentation must not depend on finding it again. */
 enabled=enabled && root!=NULL;
 bool ok=true;
 if(!sheet && enabled) {
  sheet=element(doc,"style");
  dom_node *parent=NULL,*inserted=NULL;
  parent=head(doc);
  ok=sheet && parent && attribute(sheet,"id",SHEET_ID) && attribute(sheet,"type","text/css") &&
   attribute(sheet,"data-ov-reader-owned","1");
  if(ok) ok=dom_node_append_child(parent,sheet,&inserted)==DOM_NO_ERR;
  if(inserted) dom_node_unref(inserted);if(parent) dom_node_unref(parent);
 }
 if(enabled && ok) ok=mark_path(root);
 if(enabled && ok) {
  dom_node *heading=by_id(doc,"ov-browser-reading-title");
  if(!heading) {
   heading=element(doc,"div");dom_node *first=NULL,*inserted=NULL;
   dom_node_get_first_child(root,&first);
   bool title_ok=heading && attribute(heading,"id","ov-browser-reading-title") && attribute(heading,"data-ov-reader-owned","1") &&
    attribute(heading,"data-ov-reader-title","1") && attribute(heading,"style","display:none") &&
    set_text(heading,content_get_title(content));
   if(title_ok) dom_node_insert_before(root,heading,first,&inserted);
   if(inserted) dom_node_unref(inserted);if(first) dom_node_unref(first);
  }
  if(heading) dom_node_unref(heading);
 }
 if(!enabled) {
  dom_node *heading=by_id(doc,"ov-browser-reading-title"),*parent=NULL,*removed=NULL;
  if(heading && owned(heading)) {
   dom_node_get_parent_node(heading,&parent);
   if(parent)dom_node_remove_child(parent,heading,&removed);
   if(removed)dom_node_unref(removed);if(parent)dom_node_unref(parent);
  }
  if(heading)dom_node_unref(heading);
 }
 if(sheet && ok) {
  /* Empty the owned stylesheet to restore the exact author rules. Original
   * styles, nodes, text, URLs and hidden article sections are retained. */
  ok=set_text(sheet,enabled?reader_css:"") && html_css_update_style(html,sheet);
  if(ok) html_rebuild_schedule(html);
 }
 if(root) dom_node_unref(root);if(sheet) dom_node_unref(sheet);
 return enabled && ok;
}
