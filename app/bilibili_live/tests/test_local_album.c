/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_album.c"

static void quiet(void)
{
  for(unsigned i=0;i<10000;i++){
    pthread_mutex_lock(&album_lock);bool busy=album_busy;pthread_mutex_unlock(&album_lock);
    if(!busy)return;usleep(1000);
  }
  assert(false);
}
static struct album_image decode(const char *name,unsigned w,unsigned h)
{
  char path[ALBUM_PATH];snprintf(path,sizeof(path),"%s/%s",ALBUM_ROOT,name);
  assert(local_album_decode(path,w,h)==0);quiet();struct album_image out;
  assert(local_album_take_image(&out));assert(!local_album_take_image(&(struct album_image){0}));return out;
}
int main(void)
{
  assert(local_album_valid_path(ALBUM_ROOT));assert(local_album_valid_path(ALBUM_ROOT "/照片/a.JPEG"));
  const char *bad[]={NULL,"/etc/passwd",ALBUM_ROOT "2/a.jpg",ALBUM_ROOT "/../a.jpg",ALBUM_ROOT "/a/..",ALBUM_ROOT "/./a.jpg",ALBUM_ROOT "/a//b.jpg",ALBUM_ROOT "/a/",ALBUM_ROOT "/a\nb.jpg",ALBUM_ROOT "/a\\b.jpg"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!local_album_valid_path(bad[i]));
  assert(local_album_kind("Photo.JpEg")==ALBUM_PHOTO);assert(local_album_kind("clip.MP4")==ALBUM_VIDEO);
  assert(local_album_kind("x.png")==ALBUM_PHOTO);assert(local_album_kind("x.heic")==ALBUM_PHOTO);assert(local_album_kind("x.MOV")==ALBUM_VIDEO);
  assert(local_album_decode(ALBUM_ROOT "/wide.jpg",0,140)==-EINVAL);
  assert(local_album_decode(ALBUM_ROOT "/wide.jpg",1025,140)==-EINVAL);
  assert(local_album_scan(ALBUM_ROOT)==0);quiet();struct album_listing *list=local_album_take_listing();
  assert(list && !list->error && list->count==17 && !list->truncated);assert(list->entries[0].kind==ALBUM_FOLDER);
  for(unsigned i=1;i<list->count;i++)assert(strcmp(list->entries[i].name,"linked.jpg") && strcmp(list->entries[i].name,".hidden.jpg"));free(list);
  assert(local_album_scan(ALBUM_ROOT "/many")==0);quiet();list=local_album_take_listing();
  assert(list && list->count==ALBUM_FILES && list->truncated);free(list);
  struct album_image image=decode("wide.jpg",224,140);assert(!image.error && image.pixels && image.width<=224 && image.height<=140);free(image.pixels);
  image=decode("portrait.jpg",976,376);assert(!image.error && image.pixels && image.width<image.height && image.height<=376);free(image.pixels);
  image=decode("wide.jpg",976,376);assert(!image.error && image.width<=976 && image.height<=376);free(image.pixels);
  image=decode("camera.bmp",224,140);assert(!image.error && image.pixels && image.width==224 && image.height<=140);assert(image.pixels[0]==0xf800 && image.pixels[image.width*(image.height-1)]==0x001f);free(image.pixels);
  image=decode("bottomup.bmp",224,140);assert(!image.error && image.width==7 && image.height==5);assert(image.pixels[0]==0xf800 && image.pixels[28]==0x001f);free(image.pixels);
  image=decode("bad.bmp",224,140);assert(image.error && !image.pixels);
  assert(local_album_scan(ALBUM_LOCAL_ROOT)==0);quiet();list=local_album_take_listing();assert(list && !list->error && list->count==2 && list->entries[0].kind==ALBUM_FOLDER);free(list);
  assert(local_album_search(ALBUM_LOCAL_ROOT "/library",ALBUM_FOLDER)==-EINVAL);
  for(int kind=ALBUM_PHOTO;kind<=ALBUM_VIDEO;kind++){
    assert(local_album_search(ALBUM_LOCAL_ROOT "/library",kind)==0);quiet();list=local_album_take_listing();
    assert(list&&!list->error&&!list->truncated&&list->count==2);
    assert(list->directory_bytes<32);
    assert(list->entries[0].directory!=list->entries[1].directory);
    for(unsigned i=0;i<2;i++){
      assert(list->entries[i].kind==(enum album_kind)kind&&list->entries[i].directory);
      char full[ALBUM_PATH];assert(local_album_entry_path(list,i,full,sizeof(full)));
      assert(!strstr(full,"loop")&&!strstr(full,"hidden-link"));
      struct stat st;assert(!lstat(full,&st)&&S_ISREG(st.st_mode));
      char short_path[4];assert(!local_album_entry_path(list,i,short_path,sizeof(short_path)));
      if(kind==ALBUM_PHOTO){assert(local_album_decode(full,224,140)==0);quiet();struct album_image nested;assert(local_album_take_image(&nested)&&!nested.error);free(nested.pixels);}
    }
    free(list);
  }
  assert(local_album_search(ALBUM_ROOT "/many",ALBUM_PHOTO)==0);quiet();list=local_album_take_listing();
  assert(list&&list->count==ALBUM_FILES&&list->truncated);free(list);
  assert(local_album_search(ALBUM_ROOT "/missing",ALBUM_VIDEO)==0);quiet();list=local_album_take_listing();
  assert(list&&list->error==-ENOENT);free(list);
  char deep[ALBUM_PATH]=ALBUM_LOCAL_ROOT "/depth-bound";
  assert(!mkdir(deep,0700));
  for(unsigned i=0;i<14;i++){strcat(deep,"/d");assert(!mkdir(deep,0700));}
  strcat(deep,"/beyond.jpg");FILE *beyond=fopen(deep,"wb");assert(beyond);fclose(beyond);
  assert(local_album_search(ALBUM_LOCAL_ROOT "/depth-bound",ALBUM_PHOTO)==0);quiet();list=local_album_take_listing();
  assert(list&&!list->error&&list->truncated&&!list->count);free(list);
  char budget[ALBUM_PATH]=ALBUM_LOCAL_ROOT "/path-budget";assert(!mkdir(budget,0700));
  for(unsigned i=0;i<170;i++){
    char folder[ALBUM_PATH],prefix[108];memset(prefix,'x',100);snprintf(prefix+100,8,"%03u",i);
    assert(snprintf(folder,sizeof(folder),"%s/%s",budget,prefix)<(int)sizeof(folder));assert(!mkdir(folder,0700));
    strcat(folder,"/p.jpg");FILE *file=fopen(folder,"wb");assert(file);fclose(file);
  }
  assert(local_album_search(budget,ALBUM_PHOTO)==0);quiet();list=local_album_take_listing();
  assert(list&&!list->error&&list->truncated&&list->count>0&&list->count<170);
  assert(list->directory_bytes<=ALBUM_DIRECTORIES);
  for(unsigned i=0;i<list->count;i++){
    char full[ALBUM_PATH];struct stat st;assert(local_album_entry_path(list,i,full,sizeof(full))&&!lstat(full,&st));
  }
  free(list);
  image=decode("alpha.png",224,140);assert(!image.error && image.width==7 && image.height==5);assert(image.pixels[0]==0xf800 && image.pixels[1]==0xffff && image.pixels[2]==0x7bff);free(image.pixels);
  image=decode("too-large.png",224,140);assert(image.error==-E2BIG && !image.pixels);
  image=decode("broken.png",224,140);assert(image.error && !image.pixels);
  image=decode("phone.HEIC",224,140);assert(image.error==-ENOTSUP && !image.pixels);
  image=decode("renamed.bin",224,140);assert(!image.error && image.pixels);free(image.pixels);
  image=decode("wrong.png",224,140);assert(!image.error && image.pixels);free(image.pixels);
  assert(local_album_probe(ALBUM_ROOT "/phone.HEIC")==ALBUM_HEIC);assert(local_album_probe(ALBUM_ROOT "/clip.MOV")==ALBUM_MOV);
  image=decode("corrupt.jpg",224,140);assert(image.error && !image.pixels);
  image=decode("huge.jpg",224,140);assert(image.error==-E2BIG && !image.pixels);
  image=decode("missing.jpg",224,140);assert(image.error==-ENOENT && !image.pixels);
  image=decode("linked.jpg",224,140);assert(image.error==-EINVAL && !image.pixels);
  for(unsigned i=0;i<80;i++){
    assert(local_album_decode(ALBUM_ROOT "/wide.jpg",224,140)==0);
    local_album_cancel();quiet();assert(!local_album_take_image(&image));
    assert(local_album_scan(ALBUM_ROOT)==0);local_album_cancel();quiet();assert(!local_album_take_listing());
    assert(local_album_search(ALBUM_LOCAL_ROOT,ALBUM_PHOTO)==0);local_album_cancel();quiet();assert(!local_album_take_listing());
  }
  image=decode("wide.jpg",224,140);assert(!image.error);free(image.pixels);local_album_cancel();
  puts("ALBUM_BACKEND_OK: real PNG/JPEG/BMP; recursive photo/video search; duplicate basenames retain distinct decode paths; symlink loops excluded; missing roots and 256-entry bound; path confinement and 240 cancellation cycles");return 0;
}
