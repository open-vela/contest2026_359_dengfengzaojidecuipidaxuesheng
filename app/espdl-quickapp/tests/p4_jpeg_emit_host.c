/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp32p4_jpeg_emit.h"
#include "esp32p4_jpeg_internal.h"

int main(int argc, char **argv)
{
  assert(argc == 2);
  uint8_t out[2048];
  uint32_t quant[2][64];
  const unsigned qualities[] = {1,25,50,75,90,100};
  struct p4_jpeg_header_s parsed;
  for (unsigned sampling = 0; sampling < 4; sampling++)
    for (unsigned q = 0; q < sizeof(qualities)/sizeof(qualities[0]); q++)
      {
        struct p4_jpeg_encode_s job = {0};
        job.width = 30; job.height = 24;
        job.sampling = sampling; job.quality = qualities[q];
        job.output = out; job.output_capacity = sizeof(out);
        size_t length = SIZE_MAX;
        assert(!esp32p4_jpeg_emit(&job, quant, &length));
        assert(length <= 768 && !(length & 127));
        out[length] = 255; out[length+1] = 217;
        assert(!esp32p4_jpeg_parse(out,length+2,&parsed));
        assert(parsed.width==30 && parsed.height==24);
        assert(parsed.components==(sampling==3?1:3));
        for (unsigned table = 0; table < (sampling==3?1u:2u); table++)
          for (unsigned i = 0; i < 64; i++) assert(parsed.quant[table][i]==quant[table][i]);
        char name[512];
        snprintf(name,sizeof(name),"%s/header-s%u-q%u.jpg",argv[1],sampling,qualities[q]);
        FILE *f=fopen(name,"wb");assert(f);assert(fwrite(out,1,length+2,f)==length+2);assert(!fclose(f));
        for (size_t cap = 0; cap < length; cap++)
          {
            memset(out,0xa5,sizeof(out));
            job.output_capacity=cap;
            size_t n=SIZE_MAX;
            assert(esp32p4_jpeg_emit(&job,quant,&n)==-ENOSPC && n==0);
            for (size_t i=cap;i<sizeof(out);i++) assert(out[i]==0xa5);
          }
      }
  struct p4_jpeg_encode_s invalid={.output=out,.output_capacity=sizeof(out),.width=32,.height=32};
  size_t length=1;
  assert(esp32p4_jpeg_emit(&invalid,quant,&length)==-EINVAL && length==0);
  invalid.quality=101;
  assert(esp32p4_jpeg_emit(&invalid,quant,&length)==-EINVAL);
  assert(esp32p4_jpeg_emit(NULL,quant,&length)==-EINVAL);
  puts("JPEG_EMIT_OK: 24 headers; marker/quantization roundtrip; every short capacity preserved guards; invalid quality rejected");
}
