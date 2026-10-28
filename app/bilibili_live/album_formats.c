/* SPDX-License-Identifier: Apache-2.0 */
#include "local_album.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum album_format local_album_name_format(const char *name)
{
  const char *dot=name?strrchr(name,'.'):NULL;if(!dot)return ALBUM_UNKNOWN;
  const char *const extensions[]={".jpg",".jpeg",".png",".bmp",".heic",".heif",".avif",".webp",".gif",".tif",".tiff",".mp4",".m4v",".mov",".qt",".mkv",".avi"};
  const enum album_format formats[]={ALBUM_JPEG,ALBUM_JPEG,ALBUM_PNG,ALBUM_BMP,ALBUM_HEIC,ALBUM_HEIC,ALBUM_AVIF,ALBUM_WEBP,ALBUM_GIF,ALBUM_TIFF,ALBUM_TIFF,ALBUM_MP4,ALBUM_MP4,ALBUM_MOV,ALBUM_MOV,ALBUM_MKV,ALBUM_AVI};
  for(unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);i++)if(!strcasecmp(dot,extensions[i]))return formats[i];
  return ALBUM_UNKNOWN;
}
int local_album_format_kind(enum album_format format)
{
  if(format>=ALBUM_JPEG&&format<=ALBUM_TIFF)return ALBUM_PHOTO;
  if(format>=ALBUM_MP4&&format<=ALBUM_AVI)return ALBUM_VIDEO;
  return -1;
}
int local_album_kind(const char *name){return local_album_format_kind(local_album_name_format(name));}
const char *local_album_format_label(enum album_format format)
{
  static const char *const names[]={"未知格式","JPEG","PNG","BMP","HEIC / HEIF","AVIF","WEBP","GIF","TIFF","MP4","MOV","MKV","AVI"};
  return (unsigned)format<sizeof(names)/sizeof(names[0])?names[format]:names[0];
}
bool local_album_format_supported(enum album_format format)
{return format==ALBUM_JPEG||format==ALBUM_PNG||format==ALBUM_BMP||format==ALBUM_MP4||format==ALBUM_MOV;}
static unsigned format_be32(const unsigned char *p)
{return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];}
enum album_format local_album_probe(const char *path)
{
  if(!local_album_valid_path(path))return ALBUM_UNKNOWN;
  struct stat st;if(lstat(path,&st)||!S_ISREG(st.st_mode))return ALBUM_UNKNOWN;
  return local_album_probe_regular(path);
}
enum album_format local_album_probe_regular(const char *path)
{
  unsigned char head[256];FILE *file=fopen(path,"rb");if(!file)return ALBUM_UNKNOWN;
  size_t n=fread(head,1,sizeof(head),file);fclose(file);
  if(n>=3&&head[0]==0xff&&head[1]==0xd8&&head[2]==0xff)return ALBUM_JPEG;
  if(n>=8&&!memcmp(head,"\x89PNG\r\n\x1a\n",8))return ALBUM_PNG;
  if(n>=2&&!memcmp(head,"BM",2))return ALBUM_BMP;
  if(n>=6&&(!memcmp(head,"GIF87a",6)||!memcmp(head,"GIF89a",6)))return ALBUM_GIF;
  if(n>=12&&!memcmp(head,"RIFF",4)&&!memcmp(head+8,"WEBP",4))return ALBUM_WEBP;
  if(n>=12&&!memcmp(head,"RIFF",4)&&!memcmp(head+8,"AVI ",4))return ALBUM_AVI;
  if(n>=4&&(!memcmp(head,"II\x2a\0",4)||!memcmp(head,"MM\0\x2a",4)))return ALBUM_TIFF;
  if(n>=4&&!memcmp(head,"\x1a\x45\xdf\xa3",4))return ALBUM_MKV;
  if(n>=16&&!memcmp(head+4,"ftyp",4)){
    unsigned bytes=format_be32(head);if(bytes>n)bytes=n;
    enum album_format result=ALBUM_MP4;
    for(unsigned i=8;i+4<=bytes;i+=4){
      if(i==12)continue;
      if(!memcmp(head+i,"avif",4)||!memcmp(head+i,"avis",4))return ALBUM_AVIF;
      if(!memcmp(head+i,"heic",4)||!memcmp(head+i,"heix",4)||!memcmp(head+i,"hevc",4)||!memcmp(head+i,"hevx",4))result=ALBUM_HEIC;
      else if(!memcmp(head+i,"mif1",4)&&result!=ALBUM_HEIC)result=ALBUM_HEIC;
      else if(!memcmp(head+i,"qt  ",4)&&result==ALBUM_MP4)result=ALBUM_MOV;
    }
    return result;
  }
  /* Older QuickTime files can start with a movie/media atom, before ftyp. */
  if(n>=8&&(!memcmp(head+4,"moov",4)||!memcmp(head+4,"mdat",4)))return ALBUM_MOV;
  return ALBUM_UNKNOWN;
}
bool local_album_entry_path(const struct album_listing *listing,unsigned index,char *path,size_t size)
{
  if(!listing||index>=listing->count||!path||!size)return false;
  const struct album_entry *entry=&listing->entries[index];
  if(entry->directory && entry->directory>=listing->directory_bytes)return false;
  const char *directory=listing->directories+entry->directory;
  int n=snprintf(path,size,"%s/%s%s%s",listing->path,directory,*directory?"/":"",entry->name);
  return n>=0&&(size_t)n<size;
}
