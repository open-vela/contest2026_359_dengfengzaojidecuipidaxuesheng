/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <nuttx/video/p4_jpeg.h>
#include "qpk_jpeg_hw.h"
static int mode,opens,closes;
int jpeg_test_open(const char *path,int flags,...)
{assert(!strcmp(path,"/dev/jpeg0")&&(flags&O_CLOEXEC));opens++;return 8;}
int jpeg_test_close(int fd){assert(fd==8);closes++;return 0;}
int jpeg_test_ioctl(int fd,unsigned long cmd,...)
{
 va_list ap;va_start(ap,cmd);struct p4_jpeg_encode_s *job=(void *)va_arg(ap,unsigned long);va_end(ap);
 assert(fd==8&&cmd==P4_JPEG_ENCODE&&job->width==30&&job->height==24);
 assert(job->format==P4_JPEG_INPUT_RGB565&&job->sampling==P4_JPEG_SAMPLE_420&&job->quality==90);
 assert(job->input_size==32*32*2&&!((uintptr_t)job->output&127)&&!(job->output_capacity&127));
 const uint16_t *p=(void *)job->input;
 for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++)
   assert(p[y*32+x]==(y<24?y:23)*100+(x<30?x:29));
 if(mode){errno=ENOSPC;return -1;}
 memset(job->output,0x42,16);job->output_size=16;return 0;
}
int main(void)
{
 uint16_t input[40*24];for(unsigned y=0;y<24;y++)for(unsigned x=0;x<40;x++)input[y*40+x]=y*100+x;
 uint16_t before[40*24];memcpy(before,input,sizeof(input));
 for(mode=0;mode<2;mode++){
  uint8_t *out=(void *)1;size_t size=123;
  int ret=qpk_jpeg_hw_encode565(input,30,24,40,90,&out,&size);
  assert(ret==(mode?-ENOSPC:0));
  if(mode)assert(out==NULL&&size==0);else{assert(size==16&&out[0]==0x42);free(out);}
  assert(!memcmp(before,input,sizeof(input))&&opens==closes);
 }
 uint8_t *out=(void *)1;size_t size=123;
 assert(qpk_jpeg_hw_encode565(input,30,24,40,0,&out,&size)==-EINVAL&&out==NULL&&size==0);
 puts("JPEG_ENCODE_ADAPTER_OK: copied padded input, edge repeat, stride, owned output, ENOSPC cleanup and FD closure");
}
