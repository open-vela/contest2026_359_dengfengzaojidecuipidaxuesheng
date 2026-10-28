/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BILI_P4_YUV_PACK_H
#define BILI_P4_YUV_PACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Direct-size I420 to UYVY. Reuse each chroma pair across both luma rows.
 * Word accesses are used only when every source row is suitably aligned.
 * Other layouts retain the scalar packing/scaling path in bili_ppa_frame.
 */
static inline bool bili_p4_pack_i420(uint8_t *output,
    const uint8_t *const planes[3],const int strides[2],unsigned w,unsigned h)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  typedef uint32_t alias32 __attribute__((__may_alias__));
  typedef uint16_t alias16 __attribute__((__may_alias__));
  if(!w || !h || (w&3) || (h&1) || strides[0]<(int)w ||
     strides[1]<(int)(w/2) ||
     (((uintptr_t)output|(uintptr_t)planes[0]|(unsigned)strides[0])&3) ||
     (((uintptr_t)planes[1]|(uintptr_t)planes[2]|(unsigned)strides[1])&1))
    return false;
  for(unsigned y=0;y<h;y+=2){
    const alias32 *a=(const alias32 *)(planes[0]+(size_t)y*strides[0]);
    const alias32 *b=(const alias32 *)((const uint8_t *)a+strides[0]);
    const alias16 *u=(const alias16 *)(planes[1]+(size_t)(y/2)*strides[1]);
    const alias16 *v=(const alias16 *)(planes[2]+(size_t)(y/2)*strides[1]);
    alias32 *d0=(alias32 *)(output+(size_t)y*w*2),*d1=d0+w/2;
    for(unsigned x=0;x<w/4;x++){
      uint32_t U=u[x],V=v[x],Y0=a[x],Y1=b[x];
      uint32_t c0=(U&255)|((V&255)<<16);
      uint32_t c1=(U>>8)|((V&0xff00)<<8);
      d0[2*x]=c0|((Y0&255)<<8)|((Y0&0xff00)<<16);
      d0[2*x+1]=c1|((Y0>>8)&0xff00)|(Y0&0xff000000);
      d1[2*x]=c0|((Y1&255)<<8)|((Y1&0xff00)<<16);
      d1[2*x+1]=c1|((Y1>>8)&0xff00)|(Y1&0xff000000);
    }
  }
  return true;
#else
  (void)output;(void)planes;(void)strides;(void)w;(void)h;
  return false;
#endif
}
#endif
