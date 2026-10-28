/* SPDX-License-Identifier: Apache-2.0 */
#include "local_album.h"
#include "qpk_mjpeg.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#ifndef ALBUM_HOST_TEST
#include <sys/mount.h>
#endif
#define ALBUM_JPEG_LIMIT (2u * 1024u * 1024u)
static pthread_mutex_t album_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned album_generation;
static bool album_busy, album_image_ready;
static struct album_listing *album_list_result;
static struct album_image album_image_result;
struct album_job { char path[ALBUM_PATH]; unsigned generation, width, height, directories, visited; int kind; struct album_listing *listing; };

bool local_album_valid_path(const char *path)
{
  if (!path || strlen(path) >= ALBUM_PATH) return false;
  const char *base=ALBUM_ROOT;
  if (strncmp(path,base,strlen(base)) || (path[strlen(base)] && path[strlen(base)]!='/')) base=ALBUM_LOCAL_ROOT;
  if (strncmp(path,base,strlen(base)) || (path[strlen(base)] && path[strlen(base)]!='/')) base="/data";
  size_t root = strlen(base);
  if (strncmp(path,base,root) || (path[root] && path[root]!='/')) return false;
  const char *part = path + root;
  while (*part) {
    if (*part++ != '/') return false;
    const char *end = strchr(part, '/'); size_t n = end ? (size_t)(end-part) : strlen(part);
    if (!n || (n == 1 && part[0] == '.') || (n == 2 && !memcmp(part,"..",2))) return false;
    for (size_t i=0; i<n; i++) if ((unsigned char)part[i]<32 || part[i]=='\\') return false;
    part += n;
  }
  return true;
}
static bool album_cancelled(struct album_job *job)
{
  pthread_mutex_lock(&album_lock); bool cancel=job->generation!=album_generation;
  pthread_mutex_unlock(&album_lock); return cancel;
}
static int album_order(const void *a, const void *b)
{
  const struct album_entry *x=a,*y=b;
  if ((x->kind==ALBUM_FOLDER)!=(y->kind==ALBUM_FOLDER)) return x->kind==ALBUM_FOLDER?-1:1;
  if (x->kind!=ALBUM_FOLDER && x->modified!=y->modified) return x->modified>y->modified?-1:1;
  int order=strcmp(x->name,y->name);
  return order?order:(x->directory>y->directory)-(x->directory<y->directory);
}
static void album_collect(struct album_job *job,const char *directory,unsigned depth)
{
  struct album_listing *out=job->listing;
  if(album_cancelled(job))return;
  /* Bound stack use, open directory handles and work on directory-only trees. */
  if(depth>12 || job->directories++>=1024){out->truncated=true;return;}
  DIR *dir=opendir(directory);
  if(!dir){if(!depth)out->error=-errno;else out->truncated=true;return;}
  unsigned directory_offset=0;
  struct dirent *item;
  while(!album_cancelled(job) && (item=readdir(dir))){
    if(++job->visited>16384){out->truncated=true;break;}
    if(item->d_name[0]=='.')continue;
    char path[ALBUM_PATH];
    if(snprintf(path,sizeof(path),"%s/%s",directory,item->d_name)>=(int)sizeof(path)){
      out->truncated=true;continue;
    }
    if(!local_album_valid_path(path))continue;
    struct stat st;
    if(lstat(path,&st)){out->truncated=true;continue;}
    if(S_ISLNK(st.st_mode))continue;
    if(S_ISDIR(st.st_mode)){
      if(job->kind)album_collect(job,path,depth+1);
      if(job->kind)continue;
    }else if(!S_ISREG(st.st_mode))continue;
    enum album_format format=ALBUM_UNKNOWN;
    if(S_ISREG(st.st_mode)){format=local_album_probe_regular(path);if(format==ALBUM_UNKNOWN)format=local_album_name_format(item->d_name);}
    int kind=S_ISDIR(st.st_mode)?ALBUM_FOLDER:local_album_format_kind(format);
    if(kind<0 || (job->kind && kind!=job->kind))continue;
    if(strlen(item->d_name)>=ALBUM_NAME){out->truncated=true;continue;}
    if(out->count==ALBUM_FILES){out->truncated=true;break;}
    /* Store a prefix once for this directory, only when it contains results. */
    if(depth&&!directory_offset){
      const char *relative=directory+strlen(job->path)+1;size_t bytes=strlen(relative)+1;
      if(bytes>sizeof(out->directories)-out->directory_bytes){out->truncated=true;break;}
      directory_offset=out->directory_bytes;
      memcpy(out->directories+directory_offset,relative,bytes);out->directory_bytes+=bytes;
    }
    struct album_entry *entry=&out->entries[out->count++];
    strcpy(entry->name,item->d_name);
    entry->directory=directory_offset;
    entry->kind=kind;entry->modified=st.st_mtime;entry->format=format;
  }
  closedir(dir);
}
static struct album_listing *album_read_directory(struct album_job *job)
{
  struct album_listing *out=job->listing;
  strcpy(out->path,job->path);
  out->directory_bytes=1;
#ifndef ALBUM_HOST_TEST
  struct statfs fs;
  if (!strncmp(job->path,ALBUM_ROOT,strlen(ALBUM_ROOT)) &&
      (statfs(ALBUM_ROOT,&fs)<0 || fs.f_type!=0x4d44)) {
    if (mount("/dev/mmcsd0",ALBUM_ROOT,"vfat",0,NULL)<0 &&
        (statfs(ALBUM_ROOT,&fs)<0 || fs.f_type!=0x4d44)) {out->error=-ENODEV;return out;}
  }
#endif
  album_collect(job,job->path,0);
  qsort(out->entries,out->count,sizeof(out->entries[0]),album_order);return out;
}
static uint32_t album_le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static struct album_image album_read_bmp(struct album_job *job)
{
  struct album_image out={0};struct stat st;FILE *file=NULL;uint8_t *row=NULL,header[66];
  if(lstat(job->path,&st)<0){out.error=-errno;return out;}
  if(!S_ISREG(st.st_mode)){out.error=-EINVAL;return out;}
  if(st.st_size<54 || st.st_size>8*1024*1024){out.error=-E2BIG;return out;}
  file=fopen(job->path,"rb");if(!file){out.error=-errno;return out;}
  size_t got=fread(header,1,sizeof(header),file);
  if(got<54 || header[0]!='B' || header[1]!='M'){out.error=-EBADMSG;goto done;}
  uint32_t offset=album_le32(header+10),dib=album_le32(header+14),compression=album_le32(header+30);
  int32_t width=(int32_t)album_le32(header+18),signed_height=(int32_t)album_le32(header+22);
  uint64_t height=signed_height<0?-(int64_t)signed_height:signed_height;
  unsigned bpp=header[28]|header[29]<<8;
  if(dib<40 || dib>124 || offset<14+dib || header[26]!=1 || header[27] ||
     width<=0 || width>4096 || !height || height>4096 || (uint64_t)width*height>12000000){out.error=-ENOTSUP;goto done;}
  bool rgb565=bpp==16 && compression==3;
  if((!rgb565 && (compression || (bpp!=16&&bpp!=24&&bpp!=32))) ||
     (rgb565 && (got<66 || offset<66 || album_le32(header+54)!=0xf800 ||
                 album_le32(header+58)!=0x07e0 || album_le32(header+62)!=0x001f))){out.error=-ENOTSUP;goto done;}
  unsigned stride=(((unsigned)width*bpp+31)/32)*4;
  if((uint64_t)offset+height*stride>(uint64_t)st.st_size){out.error=-EBADMSG;goto done;}
  out.width=width;out.height=height;
  if(out.width>job->width){out.width=job->width;out.height=height*out.width/width;}
  if(out.height>job->height){out.height=job->height;out.width=(uint64_t)width*out.height/height;}
  if(!out.width)out.width=1;
  if(!out.height)out.height=1;
  row=malloc(stride);out.pixels=malloc((size_t)out.width*out.height*2);
  if(!row || !out.pixels){out.error=-ENOMEM;goto done;}
  for(unsigned y=0;y<out.height;y++){
    if(album_cancelled(job)){out.error=-ECANCELED;goto done;}
    unsigned sy=(uint64_t)y*height/out.height;
    if(signed_height>0)sy=height-1-sy;
    if(fseek(file,(long)((uint64_t)offset+(uint64_t)sy*stride),SEEK_SET) || fread(row,1,stride,file)!=stride){out.error=-EIO;goto done;}
    for(unsigned x=0;x<out.width;x++){
      unsigned sx=(uint64_t)x*width/out.width;const uint8_t *p=row+sx*(bpp/8);uint16_t pixel;
      if(bpp==16){pixel=p[0]|p[1]<<8;if(!rgb565)pixel=((pixel&0x7c00)<<1)|((pixel&0x03e0)<<1)|((pixel&0x0200)>>4)|(pixel&31);}
      else pixel=((p[2]>>3)<<11)|((p[1]>>2)<<5)|(p[0]>>3);
      out.pixels[y*out.width+x]=pixel;
    }
  }
done:
  if(file)fclose(file);
  free(row);if(out.error){free(out.pixels);out.pixels=NULL;}return out;
}
static struct album_image album_read_image(struct album_job *job)
{
  enum album_format format=local_album_probe(job->path);
  if(format==ALBUM_UNKNOWN)format=local_album_name_format(job->path);
  if(format==ALBUM_BMP)return album_read_bmp(job);
  if(format!=ALBUM_JPEG && format!=ALBUM_PNG)return (struct album_image){.error=-ENOTSUP};
  struct album_image out={0}; FILE *file=NULL; uint8_t *input=NULL;
  uint16_t *decoded=NULL; void *decoder=NULL;
  struct stat st;
  if (lstat(job->path,&st)<0) {out.error=-errno;return out;}
  if (!S_ISREG(st.st_mode)) {out.error=-EINVAL;return out;}
  if (st.st_size<=0 || (uint64_t)st.st_size>ALBUM_JPEG_LIMIT) {out.error=-E2BIG;return out;}
  size_t size=(size_t)st.st_size;
  file=fopen(job->path,"rb"); if (!file) {out.error=-errno;return out;}
  input=malloc(size+512); if (!input) {out.error=-ENOMEM;goto done;}
  for (size_t offset=0; offset<size;) {
    if (album_cancelled(job)) {out.error=-ECANCELED;goto done;}
    size_t chunk=size-offset; if (chunk>32768) chunk=32768;
    if (fread(input+offset,1,chunk,file)!=chunk) {out.error=-EIO;goto done;}
    offset+=chunk;
  }
  if(format==ALBUM_PNG){
    extern int local_album_png(const unsigned char *,size_t,unsigned,unsigned,struct album_image *);
    out.error=local_album_png(input,size,job->width,job->height,&out);goto done;
  }
  unsigned w=0,h=0; out.error=qpk_mjpeg_probe(input,size,&w,&h);
  if (out.error) goto done;
  if (!w || !h || w>4096 || h>4096 || (uint64_t)w*h>12000000) {out.error=-E2BIG;goto done;}
  unsigned scale=0;
  while (scale<3 && ((w>>scale)>job->width || (h>>scale)>job->height)) scale++;
  unsigned dw=w>>scale,dh=h>>scale;
  if (!dw || !dh || (uint64_t)dw*dh>1024u*600u) {out.error=-E2BIG;goto done;}
  decoded=malloc((size_t)dw*dh*2); decoder=qpk_mjpeg_create();
  if (!decoded || !decoder) {out.error=-ENOMEM;goto done;}
  if (album_cancelled(job)) {out.error=-ECANCELED;goto done;}
  out.error=qpk_mjpeg_decode(decoder,input,size,size+512,decoded,dw,dh,w,h);
  if (out.error || album_cancelled(job)) {if (!out.error) out.error=-ECANCELED;goto done;}
  qpk_mjpeg_destroy(decoder);decoder=NULL;free(input);input=NULL;
  out.width=dw;out.height=dh;
  if (out.width>job->width) {out.width=job->width;out.height=(uint64_t)dh*out.width/dw;}
  if (out.height>job->height) {out.height=job->height;out.width=(uint64_t)dw*out.height/dh;}
  if (!out.width) out.width=1;
  if (!out.height) out.height=1;
  if (dw==out.width && dh==out.height) {out.pixels=decoded;decoded=NULL;}
  else {
    out.pixels=malloc((size_t)out.width*out.height*2);
    if (!out.pixels) {out.error=-ENOMEM;goto done;}
    for (unsigned y=0;y<out.height;y++) for (unsigned x=0;x<out.width;x++)
      out.pixels[y*out.width+x]=decoded[(uint64_t)y*dh/out.height*dw+(uint64_t)x*dw/out.width];
  }
done:
  if (file) fclose(file);
  qpk_mjpeg_destroy(decoder);free(decoded);free(input);
  if (out.error) {free(out.pixels);out.pixels=NULL;} return out;
}
static void *album_worker(void *opaque)
{
  struct album_job *job=opaque;struct album_listing *list=NULL;struct album_image image={0};
  if (job->width) image=album_read_image(job); else list=album_read_directory(job);
  pthread_mutex_lock(&album_lock);
  if (job->generation==album_generation) {
    if (job->width) {album_image_result=image;image.pixels=NULL;album_image_ready=true;}
    else {album_list_result=list;list=NULL;}
  }
  album_busy=false;pthread_mutex_unlock(&album_lock);
  free(image.pixels);free(list);free(job);return NULL;
}
static int album_request(const char *path,unsigned width,unsigned height,int kind)
{
  if (!local_album_valid_path(path) || (width && (width>1024 || !height || height>600))) return -EINVAL;
  pthread_mutex_lock(&album_lock);
  if (album_busy) {pthread_mutex_unlock(&album_lock);return -EBUSY;}
  struct album_job *job=calloc(1,sizeof(*job));
  if (!job) {pthread_mutex_unlock(&album_lock);return -ENOMEM;}
  if (!width) {job->listing=calloc(1,sizeof(*job->listing));if(!job->listing){free(job);pthread_mutex_unlock(&album_lock);return -ENOMEM;}}
  free(album_list_result);album_list_result=NULL;free(album_image_result.pixels);
  album_image_result=(struct album_image){0};album_image_ready=false;
  strcpy(job->path,path);job->width=width;job->height=height;job->kind=kind;job->generation=++album_generation;
  pthread_attr_t attr;pthread_t thread;pthread_attr_init(&attr);
  int ret=pthread_attr_setstacksize(&attr,32768);album_busy=true;
  if (!ret) ret=pthread_create(&thread,&attr,album_worker,job);
  pthread_attr_destroy(&attr);if (ret) {album_busy=false;free(job->listing);free(job);} else pthread_detach(thread);
  pthread_mutex_unlock(&album_lock);return ret?-ret:0;
}
int local_album_scan(const char *path) {return album_request(path,0,0,0);}
int local_album_search(const char *path,enum album_kind kind)
{return kind==ALBUM_PHOTO||kind==ALBUM_VIDEO?album_request(path,0,0,kind):-EINVAL;}
int local_album_decode(const char *path,unsigned width,unsigned height)
{return width?album_request(path,width,height,0):-EINVAL;}
void local_album_cancel(void)
{
  pthread_mutex_lock(&album_lock);album_generation++;
  free(album_list_result);album_list_result=NULL;free(album_image_result.pixels);
  album_image_result=(struct album_image){0};album_image_ready=false;
  pthread_mutex_unlock(&album_lock);
}
struct album_listing *local_album_take_listing(void)
{
  pthread_mutex_lock(&album_lock);struct album_listing *out=album_list_result;album_list_result=NULL;
  pthread_mutex_unlock(&album_lock);return out;
}
bool local_album_take_image(struct album_image *out)
{
  pthread_mutex_lock(&album_lock);bool ready=album_image_ready;
  if (ready) {*out=album_image_result;album_image_result=(struct album_image){0};album_image_ready=false;}
  pthread_mutex_unlock(&album_lock);return ready;
}
