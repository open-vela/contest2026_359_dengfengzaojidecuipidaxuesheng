/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <nuttx/video/p4_ppa.h>
#include "bili_ppa.h"
#include "p4_yuv_pack.h"

static atomic_bool enabled=ATOMIC_VAR_INIT(true);
#ifdef __NuttX__
extern int64_t esp_timer_get_time(void);
#endif
static uint64_t now_us(void)
{
#ifdef __NuttX__
  return (uint64_t)esp_timer_get_time();
#else
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
#endif
}
static size_t aligned_size(size_t n){return (n+127)&~(size_t)127;}
void bili_ppa_enable(unsigned value)
{
  atomic_store(&enabled,value!=0);
  printf("[ppa] enabled=%u\n",value!=0);
}
uint16_t *bili_ppa_pixels(size_t pixels)
{
  if(pixels>1280u*720u)return NULL;
  size_t bytes=aligned_size(pixels*2);
  uint16_t *p=memalign(128,bytes);
  if(p)memset(p,0,bytes);return p;
}
int bili_ppa_frame(struct bili_ppa *s,uint16_t *out,
                   const uint8_t *const p[3],const int strides[2],
                   int w,int h,unsigned dw,unsigned dh)
{
  if(!atomic_load(&enabled)||s->failed)return -ENOTSUP;
  if(!p||!p[0]||!p[1]||!p[2]||w<2||w>1280||h<2||h>720||
     (w&1)||strides[0]<w||strides[1]<(w+1)/2||!dw||!dh)return -EINVAL;
  if(!s->fd_plus_one){
    int fd=open("/dev/ppa0",O_RDWR);
    if(fd<0){s->failed=1;return -errno;}s->fd_plus_one=fd+1;
  }
  unsigned scale=dw*256/(unsigned)w,vertical=dh*256/(unsigned)h;
  if(vertical<scale)scale=vertical;if(scale>256)scale=256;
  scale&=~15u;
  unsigned ow=w*scale/256,oh=h*scale/256;
  unsigned fitw=dw,fith=(unsigned)h*dw/(unsigned)w;
  if(fith>dh){fith=dh;fitw=(unsigned)w*dh/(unsigned)h;}
  unsigned pw=w,ph=h;
  /* For ratios not representable in hardware, resize while packing so the
   * viewport remains filled. PPA still performs the color conversion.
   */
  bool mapped=fitw>ow+2||fith>oh+2;
  if(mapped){pw=fitw&~1u;ph=fith;ow=pw;oh=ph;scale=256;}
  if(pw<2||ph<2||pw>1280||ph>720)return -ENOTSUP;
  size_t bytes=aligned_size((size_t)pw*ph*2);
  if(s->capacity<bytes){
    uint8_t *buffer=memalign(128,bytes);
    if(!buffer){s->failed=1;return -ENOMEM;}
    free(s->packed);s->packed=buffer;s->capacity=bytes;
  }
  uint64_t start=now_us();
  unsigned xs[1280];
  if(mapped)for(unsigned x=0;x<pw;x++)xs[x]=x*(unsigned)w/pw;
  bool packed=!mapped && bili_p4_pack_i420(s->packed,p,strides,pw,ph);
  if(packed)s->pack_fast_frames++;else s->pack_scalar_frames++;
  for(unsigned y=0;!packed && y<ph;y++){
    unsigned sy=mapped?y*(unsigned)h/ph:y;
    const uint8_t *Y=p[0]+sy*strides[0],*U=p[1]+(sy/2)*strides[1],*V=p[2]+(sy/2)*strides[1];
    uint32_t *d=(uint32_t*)(s->packed+(size_t)y*pw*2);
    for(unsigned x=0;x<pw;x+=2){
      unsigned sx=mapped?xs[x]:x,sx1=mapped?xs[x+1]:x+1;
      d[x/2]=(uint32_t)U[sx/2]|((uint32_t)Y[sx]<<8)|
        ((uint32_t)V[sx/2]<<16)|((uint32_t)Y[sx1]<<24);
    }
  }
  unsigned ox=(dw-ow)/2,oy=(dh-oh)/2;
  if(oy)memset(out,0,oy*dw*2);
  if(oy+oh<dh)memset(out+(oy+oh)*dw,0,(dh-oy-oh)*dw*2);
  if(ow<dw)for(unsigned y=oy;y<oy+oh;y++){
    if(ox)memset(out+y*dw,0,ox*2);
    if(ox+ow<dw)memset(out+y*dw+ox+ow,0,(dw-ox-ow)*2);
  }
  s->pack_us+=now_us()-start;
  struct p4_ppa_job_s j={.input=s->packed,.output=out,.input_size=bytes,
    .output_size=aligned_size((size_t)dw*dh*2),.width=pw,.height=ph,
    .out_width=dw,.out_height=dh,.out_x=ox,.out_y=oy,.scale256=scale};
  start=now_us();int ret=ioctl(s->fd_plus_one-1,P4_PPA_SRM,(unsigned long)&j);
  s->dma_us+=now_us()-start;
  if(ret<0){ret=-errno;s->failed=1;printf("[ppa] software fallback error=%d\n",ret);return ret;}
  s->frames++;return 0;
}
int bili_ppa_rgb565_fit(struct bili_ppa *s, uint16_t *output,
                       const uint16_t *input, unsigned width, unsigned height,
                       unsigned out_width, unsigned out_height)
{
  unsigned scale;
  unsigned vertical;
  unsigned scaled_width;
  unsigned scaled_height;
  unsigned left;
  unsigned top;
  size_t input_size;
  size_t output_size;
  uint64_t start;
  int ret;

  if (!s || !input || !output || width < 2 || width > 1280 ||
      height < 2 || height > 720 || out_width < 2 || out_width > 1280 ||
      out_height < 2 || out_height > 720)
    return -EINVAL;

  if (!atomic_load(&enabled) || s->failed) return -ENOTSUP;

  input_size = aligned_size((size_t)width * height * 2);
  output_size = aligned_size((size_t)out_width * out_height * 2);
  if (((uintptr_t)input & 127) || ((uintptr_t)output & 127) ||
      (uintptr_t)input > UINTPTR_MAX - input_size ||
      (uintptr_t)output > UINTPTR_MAX - output_size ||
      ((uintptr_t)input < (uintptr_t)output + output_size &&
       (uintptr_t)output < (uintptr_t)input + input_size))
    return -EINVAL;

  scale = out_width * 256 / width;
  vertical = out_height * 256 / height;
  if (vertical < scale) scale = vertical;
  if (scale > 1024) scale = 1024;
  scale &= ~15u;
  if (scale < 16) return -ENOTSUP;

  scaled_width = width * scale / 256;
  scaled_height = height * scale / 256;
  if (!scaled_width || !scaled_height) return -ENOTSUP;
  left = (out_width - scaled_width) / 2;
  top = (out_height - scaled_height) / 2;

  if (!s->fd_plus_one)
    {
      int fd = open("/dev/ppa0", O_RDWR);
      if (fd < 0) { s->failed = 1; return -errno; }
      s->fd_plus_one = fd + 1;
    }

  /* The caller owns both buffers until this synchronous job returns.
   * Only clear letterbox pixels; DMA overwrites the complete image block.
   */

  start = now_us();
  if (top) memset(output, 0, (size_t)top * out_width * 2);
  if (top + scaled_height < out_height)
    memset(output + (size_t)(top + scaled_height) * out_width, 0,
           (size_t)(out_height - top - scaled_height) * out_width * 2);
  if (scaled_width < out_width)
    for (unsigned y = top; y < top + scaled_height; y++)
      {
        if (left) memset(output + (size_t)y * out_width, 0, left * 2);
        if (left + scaled_width < out_width)
          memset(output + (size_t)y * out_width + left + scaled_width, 0,
                 (out_width - left - scaled_width) * 2);
      }
  s->pack_us += now_us() - start;

  struct p4_ppa_job_s job = {
    .input = input, .output = output,
    .input_size = input_size, .output_size = output_size,
    .width = width, .height = height,
    .out_width = out_width, .out_height = out_height,
    .out_x = left, .out_y = top, .scale256 = scale
  };
  start = now_us();
  ret = ioctl(s->fd_plus_one - 1, P4_PPA_SRM_RGB565, (unsigned long)&job);
  s->dma_us += now_us() - start;
  if (ret < 0)
    {
      ret = -errno;
      s->failed = 1;
      printf("[ppa-rgb565] software fallback error=%d\n", ret);
      return ret;
    }
  s->frames++;
  return 0;
}

void bili_ppa_close(struct bili_ppa *s)
{
  if(s->fd_plus_one)close(s->fd_plus_one-1);
  if(s->frames||s->failed)printf("[ppa] frames=%u pack_ms=%lu dma_ms=%lu failed=%d pack_fast=%u pack_scalar=%u\n",
    s->frames,(unsigned long)(s->pack_us/1000),(unsigned long)(s->dma_us/1000),s->failed,
    s->pack_fast_frames,s->pack_scalar_frames);
  free(s->packed);memset(s,0,sizeof(*s));
}

static int clip(int v){return v<0?0:v>255?255:v;}
static int color_test(unsigned w,unsigned h,unsigned scale)
{
  const unsigned dw=640,dh=360;
  const uint8_t colors[4][3]={{81,90,240},{145,54,34},{41,240,110},{235,128,128}};
  size_t in_size=aligned_size((size_t)w*h*2),out_size=dw*dh*2;
  uint8_t *raw=memalign(128,in_size+256),*out=memalign(128,out_size+256);
  int fd=open("/dev/ppa0",O_RDWR),ret=0;
  if(fd<0||!raw||!out){ret=fd<0?-errno:-ENOMEM;goto done;}
  memset(raw,0xa5,in_size+256);memset(out,0x5a,out_size+256);
  uint8_t *src=raw+128;uint16_t *dst=(uint16_t*)(out+128);
  unsigned stripe=w/8*2;
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x+=2){
    unsigned c=x/stripe;if(c>3)c=3;
    unsigned i=(y*w+x)*2;
    src[i]=colors[c][1];src[i+1]=colors[c][0];src[i+2]=colors[c][2];src[i+3]=colors[c][0];
  }
  memset(dst,0,out_size);
  unsigned ow=w*scale/256,oh=h*scale/256,ox=(dw-ow)/2,oy=(dh-oh)/2;
  struct p4_ppa_job_s j={.input=src,.output=dst,.input_size=in_size,
    .output_size=out_size,.width=w,.height=h,.out_width=dw,.out_height=dh,
    .out_x=ox,.out_y=oy,.scale256=scale};
  uint64_t start=now_us();
  ret=ioctl(fd,P4_PPA_SRM,(unsigned long)&j);
  if(ret<0){ret=-errno;printf("[ppa-test] failed %ux%u scale=%u ret=%d\n",w,h,scale,ret);goto done;}
  unsigned mismatch=0;
  /* Check interiors, avoiding interpolation across stripe boundaries. */
  for(unsigned y=0;y<dh;y++)for(unsigned x=0;x<dw;x++){
    unsigned pix=dst[y*dw+x];
    if(x<ox||x>=ox+ow||y<oy||y>=oy+oh){if(pix)mismatch++;continue;}
    unsigned sx=(x-ox)*256/scale,c=sx/stripe,edge=sx%stripe;
    if(c>3)c=3;if(edge<8||edge>stripe-8)continue;
    int Y=colors[c][0]-16,U=colors[c][1]-128,V=colors[c][2]-128;
    int r=clip((298*Y+409*V+128)>>8),g=clip((298*Y-100*U-208*V+128)>>8),b=clip((298*Y+516*U+128)>>8);
    if(abs((int)(pix>>11)-(r>>3))>1||abs((int)((pix>>5)&63)-(g>>2))>1||abs((int)(pix&31)-(b>>3))>1)mismatch++;
  }
  for(unsigned i=0;i<128;i++){
    if(out[i]!=0x5a||out[out_size+128+i]!=0x5a)mismatch++;
    if(raw[i]!=0xa5||raw[in_size+128+i]!=0xa5)mismatch++;
  }
  printf("[ppa-test] color %ux%u scale=%u mismatch=%u elapsed_us=%lu\n",
    w,h,scale,mismatch,(unsigned long)(now_us()-start));
  ret=mismatch?-EIO:0;
  /* Invalid alignment/shape must fail without starting DMA. */
  if(!ret){j.width=1;if(ioctl(fd,P4_PPA_SRM,(unsigned long)&j)>=0||errno!=EINVAL)ret=-EIO;}
done:
  if(fd>=0)close(fd);free(raw);free(out);return ret;
}
static int planar_test(unsigned w,unsigned h)
{
  const uint8_t colors[4][3]={{81,90,240},{145,54,34},{41,240,110},{235,128,128}};
  int strides[2]={(int)w+16,(int)w/2+8};
  size_t yn=(size_t)strides[0]*h,cn=(size_t)strides[1]*((h+1)/2);
  uint8_t *input=calloc(1,yn+cn*2);
  uint8_t *raw=memalign(128,640*360*2+256);
  struct bili_ppa context={0};int ret=0;
  bool saved=atomic_exchange(&enabled,true);
  if(!input||!raw){ret=-ENOMEM;goto done;}
  const uint8_t *p[3]={input,input+yn,input+yn+cn};
  unsigned stripe=w/8*2;
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
    unsigned c=x/stripe;if(c>3)c=3;
    input[y*strides[0]+x]=colors[c][0];
    input[yn+(y/2)*strides[1]+x/2]=colors[c][1];
    input[yn+cn+(y/2)*strides[1]+x/2]=colors[c][2];
  }
  memset(raw,0x5a,640*360*2+256);
  uint16_t *dst=(uint16_t*)(raw+128);
  ret=bili_ppa_frame(&context,dst,p,strides,w,h,640,360);
  if(ret)goto done;
  unsigned fw=640,fh=h*640/w;
  if(fh>360){fh=360;fw=w*360/h;}fw&=~1u;
  unsigned ox=(640-fw)/2,oy=(360-fh)/2,mismatch=0;
  for(unsigned y=0;y<360;y++)for(unsigned x=0;x<640;x++){
    unsigned pix=dst[y*640+x];
    if(x<ox||x>=ox+fw||y<oy||y>=oy+fh){if(pix)mismatch++;continue;}
    unsigned sx=(x-ox)*w/fw,c=sx/stripe,edge=sx%stripe;
    if(c>3)c=3;if(edge<8||edge>stripe-8)continue;
    int Y=colors[c][0]-16,U=colors[c][1]-128,V=colors[c][2]-128;
    int r=clip((298*Y+409*V+128)>>8),g=clip((298*Y-100*U-208*V+128)>>8),b=clip((298*Y+516*U+128)>>8);
    if(abs((int)(pix>>11)-(r>>3))>1||abs((int)((pix>>5)&63)-(g>>2))>1||abs((int)(pix&31)-(b>>3))>1)mismatch++;
  }
  for(unsigned i=0;i<128;i++)if(raw[i]!=0x5a||raw[640*360*2+128+i]!=0x5a)mismatch++;
  printf("[ppa-test] planar %ux%u padded strides mismatch=%u\n",w,h,mismatch);
  ret=mismatch?-EIO:0;
done:
  atomic_store(&enabled,saved);bili_ppa_close(&context);free(input);free(raw);return ret;
}
static int rgb565_scale_test(unsigned width, unsigned height)
{
  static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
  const unsigned dw = 1024, dh = 600;
  size_t input_size = aligned_size((size_t)width * height * 2);
  size_t output_size = aligned_size((size_t)dw * dh * 2);
  uint8_t *raw = memalign(128, input_size + 256);
  uint8_t *out = memalign(128, output_size + 256);
  struct bili_ppa context = {0};
  bool saved = atomic_exchange(&enabled, true);
  int ret = -ENOMEM;
  if (!raw || !out) goto done;
  memset(raw, 0xa5, input_size + 256);
  memset(out, 0x5a, output_size + 256);
  uint16_t *src = (uint16_t *)(raw + 128);
  uint16_t *dst = (uint16_t *)(out + 128);
  unsigned stripe = width / 4;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++)
      src[y * width + x] = colors[x / stripe];
  ret = bili_ppa_rgb565_fit(&context, dst, src, width, height, dw, dh);
  if (ret) goto done;
  unsigned scale = dw * 256 / width, vertical = dh * 256 / height;
  if (vertical < scale) scale = vertical;
  scale &= ~15u;
  unsigned ow = width * scale / 256, oh = height * scale / 256;
  unsigned ox = (dw - ow) / 2, oy = (dh - oh) / 2, mismatch = 0;
  for (unsigned y = 0; y < dh; y++)
    for (unsigned x = 0; x < dw; x++)
      {
        unsigned actual = dst[y * dw + x];
        if (x < ox || x >= ox + ow || y < oy || y >= oy + oh)
          { if (actual) mismatch++; continue; }
        unsigned sx = (x - ox) * 256 / scale, edge = sx % stripe;
        if (edge < 8 || edge > stripe - 8) continue;
        unsigned expected = colors[sx / stripe];
        if (abs((int)(actual >> 11) - (int)(expected >> 11)) > 1 ||
            abs((int)((actual >> 5) & 63) - (int)((expected >> 5) & 63)) > 1 ||
            abs((int)(actual & 31) - (int)(expected & 31)) > 1) mismatch++;
      }
  for (unsigned i = 0; i < 128; i++)
    if (raw[i] != 0xa5 || raw[input_size + 128 + i] != 0xa5 ||
        out[i] != 0x5a || out[output_size + 128 + i] != 0x5a) mismatch++;
  printf("[ppa-test] RGB565 %ux%u scale=%u block=%ux%u mismatch=%u dma_us=%llu\n",
         width, height, scale, ow, oh, mismatch,
         (unsigned long long)context.dma_us);
  ret = mismatch ? -EIO : 0;
done:
  atomic_store(&enabled, saved);
  bili_ppa_close(&context);
  free(raw);
  free(out);
  return ret;
}

int bili_ppa_test(void)
{
  const unsigned w=64,h=32;
  uint8_t *src=memalign(128,w*h*2);
  uint16_t *dst=memalign(128,w*h*2+128);
  int fd=open("/dev/ppa0",O_RDWR),ret=0;
  if(fd<0||!src||!dst){ret=fd<0?-errno:-ENOMEM;goto done;}
  /* Neutral chroma; grey levels form vertical stripes. */
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x+=2){
    unsigned i=(y*w+x)*2;
    uint8_t luma=16+((x/16)*73);
    src[i]=128;src[i+1]=luma;src[i+2]=128;src[i+3]=luma;
  }
  memset(dst,0x5a,w*h*2+128);
  struct p4_ppa_job_s j={.input=src,.output=dst,.input_size=w*h*2,
    .output_size=w*h*2,.width=w,.height=h,.out_width=w,.out_height=h,.scale256=256};
  ret=ioctl(fd,P4_PPA_SRM,(unsigned long)&j);
  if(ret<0){ret=-errno;goto done;}
  unsigned mismatch=0;
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
    unsigned v=((16+(x/16)*73)-16)*255/219;
    if(v>255)v=255;
    unsigned p=dst[y*w+x];
    if(abs((int)(p>>11)-(int)(v>>3))>1 ||
       abs((int)((p>>5)&63)-(int)(v>>2))>1 ||
       abs((int)(p&31)-(int)(v>>3))>1)mismatch++;
  }
  for(unsigned i=0;i<128;i++)if(((uint8_t*)dst)[w*h*2+i]!=0x5a)mismatch++;
  printf("[ppa-test] pixels=%u,%u,%u,%u mismatch=%u\n",
    dst[0],dst[16],dst[32],dst[48],mismatch);
  ret=mismatch?-EIO:0;
done:
  if(fd>=0)close(fd);
  free(src);free(dst);
  if(!ret){
    int results[]={color_test(640,360,256),color_test(848,480,128),
      color_test(848,480,192),color_test(854,480,176),color_test(1280,720,128),
      color_test(320,180,512),color_test(160,90,1024)};
    for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);i++)if(results[i])ret=results[i];
  }
  if(!ret)ret=planar_test(640,360);
  if(!ret)ret=planar_test(848,480);
  if(!ret)ret=planar_test(854,480);
  if(!ret)ret=planar_test(320,180);
  if(!ret)ret=rgb565_scale_test(320,180);
  if(!ret)ret=rgb565_scale_test(640,360);
  printf("[ppa-test] result=%d\n",ret);return ret;
}
