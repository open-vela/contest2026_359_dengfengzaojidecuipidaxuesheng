/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_assets.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

struct glv_asset_lease {
  char root[384];FILE *held[4];struct stat identity[4];
};
static const char *lease_names[4]={"manifest.bin","edgeidiot/model.bin","edgeidiot/tokenizer.bin","edgeidiot/weights.bin"};
void glv_asset_lease_close(struct glv_asset_lease *lease)
{
  if(!lease)return;for(unsigned i=0;i<4;i++)if(lease->held[i])fclose(lease->held[i]);glv_free(lease);
}
static bool same_identity(const struct stat *a,const struct stat *b)
{
  return S_ISREG(a->st_mode)&&S_ISREG(b->st_mode)&&a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&
    a->st_size==b->st_size&&a->st_mtime==b->st_mtime&&a->st_ctime==b->st_ctime;
}
int glv_asset_lease_validate(struct glv_asset_lease *lease)
{
  if(!lease)return -EINVAL;char path[512];
  for(unsigned i=0;i<4;i++){
    struct stat held,named;
    if(snprintf(path,sizeof(path),"%s/%s",lease->root,lease_names[i])>=(int)sizeof(path))return -ENAMETOOLONG;
    if(fstat(fileno(lease->held[i]),&held)||lstat(path,&named))return -ENODEV;
    if(S_ISLNK(named.st_mode)||!same_identity(&held,&lease->identity[i])||!same_identity(&named,&held))return -ESTALE;
  }return 0;
}
int glv_asset_lease_open(const char *root,struct glv_memory *memory,struct glv_asset_lease **out)
{
  *out=NULL;if(strlen(root)>=384)return -ENAMETOOLONG;
  struct glv_asset_lease *lease=glv_alloc(memory,sizeof(*lease));if(!lease)return -ENOMEM;
  strcpy(lease->root,root);int ret=0;char path[512];
  for(unsigned i=0;i<4&&!ret;i++){
    if((ret=glv_cancel(memory)))break;size_t bytes;
    if(snprintf(path,sizeof(path),"%s/%s",root,lease_names[i])>=(int)sizeof(path)){ret=-ENAMETOOLONG;break;}
    ret=glv_open_asset(path,&lease->held[i],&bytes);
    if(!ret&&fstat(fileno(lease->held[i]),&lease->identity[i]))ret=-EIO;
  }
  if(!ret)ret=glv_asset_lease_validate(lease);
  if(ret)glv_asset_lease_close(lease);else *out=lease;return ret;
}

static bool safe_name(const char *p)
{
  if(!p[0]||p[0]=='/'||strchr(p,'\\')||strchr(p,':'))return false;
  const char *part=p;
  for(;;p++)if(*p=='/'||!*p){size_t n=p-part;
    if(!n||(n==1&&part[0]=='.')||(n==2&&!memcmp(part,"..",2)))return false;
    if(!*p)break;part=p+1;
  }
  return true;
}
struct small_asset {void *data;size_t bytes,capacity;};
static int check_entry(const char *root,const unsigned char e[132],struct glv_memory *m,struct small_asset *small)
{
  char path[384];FILE *f=NULL;size_t size;unsigned char digest[32],*io=NULL;
  if(!memchr(e,0,96)||!safe_name((const char *)e))return -EBADMSG;
  if(snprintf(path,sizeof(path),"%s/%s",root,e)>=(int)sizeof(path))return -ENAMETOOLONG;
  int ret=glv_open_asset(path,&f,&size);if(ret)return ret;
  bool keep=!small&&m->retain_verified&&(!strcmp((const char *)e,"edgeidiot/weights.bin")||!strcmp((const char *)e,"tts-v2/weights.bin"));
  struct stat before,after;
  if(fstat(fileno(f),&before)){ret=-EIO;goto done;}
  if(keep){
    io=glv_alloc(m,16384);if(!io){ret=-ENOMEM;goto done;}
    if(setvbuf(f,(char *)io,_IOFBF,16384)){ret=-EIO;goto done;}
  }
  if(size!=glv_u32(e+96))ret=-EBADMSG;
  else if(small){
    if(!size||size>small->capacity){ret=-E2BIG;goto done;}
    small->data=glv_alloc(m,size);if(!small->data){ret=-ENOMEM;goto done;}
    ret=glv_cancel(m);if(!ret)ret=glv_read_exact(f,small->data,size);
    if(!ret)ret=glv_sha_buffer(small->data,size,digest,m);
    if(!ret&&memcmp(digest,e+100,32))ret=-EBADMSG;
    if(!ret)small->bytes=size;
  }
  else if(!(ret=glv_sha_file(f,size,digest,m))&&memcmp(digest,e+100,32))ret=-EBADMSG;
  if(!ret&&(fstat(fileno(f),&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||
    before.st_size!=after.st_size||before.st_mtime!=after.st_mtime||before.st_ctime!=after.st_ctime))ret=-EIO;
  if(!ret&&keep){ret=glv_verified_keep(m,path,f,io,size,digest);if(!ret)return 0;}
done:
  fclose(f);glv_free(io);return ret;
}
static int check(const char *root,const char *one,unsigned groups,struct glv_memory *m,char *failed,size_t capacity,struct small_asset *small)
{
  static const char *required[]={"edgeidiot/model.bin","edgeidiot/weights.bin","edgeidiot/tokenizer.bin",
    "tts-v2/model.bin","tts-v2/weights.bin","tts-v2/frontend.bin","wake/weights.bin"};
  if(!one&&(!groups||(groups&~GLV_ASSET_ALL)))return -EINVAL;
  /* V2's Chinese frontend shares only the tokenizer's NFKC dictionary, not
   * LLM weights. Keep that small dependency authenticated for TTS too. */
  unsigned required_mask=(groups&GLV_ASSET_LLM?7u:0)|(groups&GLV_ASSET_TTS?60u:0)|(groups&GLV_ASSET_WAKE?64u:0);
  char path[384];FILE *f=NULL;size_t size;unsigned char h[16],e[132];unsigned seen=0;int ret;
  if(!one&&m->verified){bool retain=m->retain_verified;glv_verified_clear(m);m->retain_verified=retain;}
  if(failed&&capacity)failed[0]=0;
  if(snprintf(path,sizeof(path),"%s/manifest.bin",root)>=(int)sizeof(path))return -ENAMETOOLONG;
  if((ret=glv_open_asset(path,&f,&size)))return ret;
  ret=-EBADMSG;
  if(glv_read_exact(f,h,16)||memcmp(h,"GLV1",4)||glv_u32(h+4)!=GLV_ABI||glv_u32(h+12))goto done;
  unsigned count=glv_u32(h+8);if(!count||count>96||size!=16+132u*count)goto done;
  bool found=false;
  for(unsigned i=0;i<count;i++){
    if((ret=glv_cancel(m)))goto done;
    ret=-EBADMSG;if(glv_read_exact(f,e,132)||!memchr(e,0,96)||!safe_name((const char *)e))goto done;
    for(unsigned k=0;k<7;k++)if(!strcmp(required[k],(const char *)e)){
      if(seen&(1u<<k))goto done;seen|=1u<<k;
    }
    if(one&&strcmp(one,(const char *)e))continue;
    if(!one&&groups!=GLV_ASSET_ALL){
      bool selected=((groups&GLV_ASSET_LLM)&&!strncmp((const char *)e,"edgeidiot/",10))||
        ((groups&GLV_ASSET_TTS)&&!strncmp((const char *)e,"tts-v2/",7))||
        ((groups&GLV_ASSET_TTS)&&!strcmp((const char *)e,"edgeidiot/tokenizer.bin"))||
        ((groups&GLV_ASSET_WAKE)&&!strncmp((const char *)e,"wake/",5));
      if(!selected)continue;
    }
    if(found&&one)goto done;
    found=true;if((ret=check_entry(root,e,m,small))){if(failed&&capacity)snprintf(failed,capacity,"%s",e);goto done;}
  }
  ret=one?(found?0:-ENOENT):((seen&required_mask)==required_mask?0:-EBADMSG);
done:
  fclose(f);if(ret)glv_verified_clear(m);return ret;
}
int glv_assets_check(const char *root,struct glv_memory *m,char *failed,size_t size)
{return check(root,NULL,GLV_ASSET_ALL,m,failed,size,NULL);}
int glv_assets_check_group(const char *root,unsigned groups,struct glv_memory *m,char *failed,size_t size)
{return check(root,NULL,groups,m,failed,size,NULL);}
int glv_asset_check(const char *root,const char *name,struct glv_memory *m)
{return check(root,name,0,m,NULL,0,NULL);}

int glv_asset_read_small(const char *root,const char *name,size_t capacity,struct glv_memory *m,void **data,size_t *bytes)
{
 if(!root||!name||!m||!data||!bytes||!capacity)return -EINVAL;
 *data=NULL;*bytes=0;struct small_asset small={.capacity=capacity};
 int ret=check(root,name,0,m,NULL,0,&small);
 if(ret)glv_free(small.data);else {*data=small.data;*bytes=small.bytes;}
 return ret;
}
