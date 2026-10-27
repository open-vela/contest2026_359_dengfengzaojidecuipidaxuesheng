/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp32p4_jpeg_internal.h"
#include "jpeg_cases.h"

static size_t find_marker(const uint8_t *p,size_t n,unsigned marker)
{
  size_t pos=2;
  while(pos+4<=n){
    assert(p[pos]==255);
    if(p[pos+1]==marker)return pos;
    if(p[pos+1]==0xda)break;
    size_t size=((unsigned)p[pos+2]<<8)|p[pos+3];assert(size>=2&&size<=n-pos-2);
    pos+=2+size;
  }
  return SIZE_MAX;
}
static uint32_t random_state=0x139f7612;
static uint32_t random_value(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
int main(void)
{
  struct p4_jpeg_header_s *h=calloc(1,sizeof(*h));assert(h);
  unsigned valid=0,rejected=0;
  for(size_t i=0;i<sizeof(jpeg_cases)/sizeof(jpeg_cases[0]);i++){
    const struct jpeg_case_s *c=&jpeg_cases[i];FILE *f=fopen(c->path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long raw=ftell(f);assert(raw>0);rewind(f);
    size_t n=(size_t)raw;uint8_t *data=malloc(n),*copy=malloc(n);assert(data&&copy);
    assert(fread(data,1,n,f)==n);fclose(f);
    int ret=esp32p4_jpeg_parse(data,n,h);
    if(!c->supported){assert(ret<0);rejected++;free(data);free(copy);continue;}
    assert(ret==0&&h->width==c->width&&h->height==c->height&&h->components==c->components);
    assert(h->scan_length&&h->scan_offset+h->scan_length<=n);
    assert(h->padded_width>=h->width&&h->padded_height>=h->height);
    unsigned mx=c->sampling==0||c->components==1?1:2,my=c->sampling==2?2:1;
    assert(h->horizontal[0]==mx&&h->vertical[0]==my);
    assert(h->padded_width==(h->width+mx*8-1)/(mx*8)*(mx*8));
    assert(h->padded_height==(h->height+my*8-1)/(my*8)*(my*8));valid++;
    assert(esp32p4_jpeg_parse(data,n-1,h)<0&&esp32p4_jpeg_parse(data,n-2,h)<0);
    size_t prefix=h->scan_offset+12;if(prefix>n-2)prefix=n-2;
    for(size_t k=0;k<prefix;k++)assert(esp32p4_jpeg_parse(data,k,h)<0);
    if(i==0){
      size_t q=find_marker(data,n,0xdb),sof=find_marker(data,n,0xc0),dht=find_marker(data,n,0xc4);
      assert(q!=SIZE_MAX&&sof!=SIZE_MAX&&dht!=SIZE_MAX);
      memcpy(copy,data,n);copy[q+4]=15;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[dht+4]=255;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[dht+5]=255;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[sof+11]=0;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[sof+12]=3;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[sof+13]=copy[sof+10];assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[sof+5]=copy[sof+6]=0;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      memcpy(copy,data,n);copy[sof+1]=0xc2;assert(esp32p4_jpeg_parse(copy,n,h)<0);
      for(unsigned k=0;k<10000;k++){
        memcpy(copy,data,n);size_t offset=random_value()%(n<512?n:512);
        copy[offset]^=(uint8_t)random_value();esp32p4_jpeg_parse(copy,n,h);
      }
    }
    free(data);free(copy);
  }
  uint8_t bytes[512];
  for(unsigned k=0;k<20000;k++){
    size_t n=random_value()%sizeof(bytes);
    for(size_t j=0;j<n;j++)bytes[j]=(uint8_t)random_value();
    assert(esp32p4_jpeg_parse(bytes,n,h)<0);
  }
  assert(esp32p4_jpeg_parse(NULL,4,h)<0);
  assert(esp32p4_jpeg_parse(bytes,SIZE_MAX,h)<0);
  free(h);
  printf("P4_JPEG_PARSE_OK: valid=%u rejected=%u truncation/indices/sampling + 30000 mutations/random streams\n",valid,rejected);
  return 0;
}
