/* SPDX-License-Identifier: Apache-2.0 */
/* Streaming SHA-256 derived from the existing qpk_espdl_sha256.c. */
#include "glass_local_model.h"
#include <errno.h>
#include <string.h>
#ifdef GLV_SHA_HW
#include "local_sha_hw.h"
#endif
static const uint32_t k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
static void block(uint32_t h[8], const uint8_t p[64])
{
  uint32_t w[64];
  for (unsigned i=0;i<16;i++)
    w[i]=(uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
  for (unsigned i=16;i<64;i++) {
    uint32_t a=w[i-15],b=w[i-2];
    w[i]=w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>10));
  }
  uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
  for (unsigned i=0;i<64;i++) {
    uint32_t t1=v+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
    uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
    v=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
  }
  h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
}
int glv_sha_file(FILE *f,size_t expected,unsigned char digest[32],struct glv_memory *memory)
{
#ifdef GLV_SHA_HW
  return glv_sha_hw_file(f,expected,digest,memory);
#else
  uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  unsigned char data[4096],last[128]={0};size_t bytes=0,tail=0;
  while(bytes<expected){
    int ret=glv_cancel(memory);if(ret)return ret;
    size_t n=expected-bytes;if(n>sizeof(data))n=sizeof(data);
    uint64_t began=glv_now_us();size_t got=fread(data,1,n,f);
    memory->profile.sha_read_us+=glv_now_us()-began;memory->profile.sha_bytes+=got;
    if(got!=n)return -EIO;began=glv_now_us();bytes+=n;size_t pos=0;
    if(tail){size_t add=n<64-tail?n:64-tail;memcpy(last+tail,data,add);tail+=add;pos+=add;if(tail==64){block(h,last);tail=0;}}
    while(n-pos>=64){block(h,data+pos);pos+=64;}
    if(n>pos){tail=n-pos;memcpy(last,data+pos,tail);}
    memory->profile.sha_compute_us+=glv_now_us()-began;
  }
  memset(last+tail,0,sizeof(last)-tail);last[tail]=0x80;unsigned padded=tail<56?64:128;
  uint64_t bits=(uint64_t)bytes*8;
  for(unsigned i=0;i<8;i++)last[padded-1-i]=(uint8_t)(bits>>(8*i));
  uint64_t began=glv_now_us();block(h,last);if(padded==128)block(h,last+64);
  for(unsigned i=0;i<32;i++)digest[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
  memory->profile.sha_compute_us+=glv_now_us()-began;
  return 0;
#endif
}

/* Small metadata snapshots are hashed from the exact bytes that will be used.
 * Bulk model files retain their existing hardware/streaming path. */
int glv_sha_buffer(const void *bytes,size_t count,unsigned char digest[32],struct glv_memory *m)
{
 if(!m||!digest||(!bytes&&count))return -EINVAL;
 if(count>UINT64_MAX/8u)return -E2BIG;
 const unsigned char *p=bytes;uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 size_t at=0;uint64_t began=glv_now_us();
 while(count-at>=64){if(!(at%4096)){int ret=glv_cancel(m);if(ret)return ret;}block(h,p+at);at+=64;}
 int ret=glv_cancel(m);if(ret)return ret;
 unsigned char last[128]={0};size_t tail=count-at;if(tail)memcpy(last,p+at,tail);last[tail]=0x80;
 unsigned padded=tail<56?64:128;uint64_t bits=(uint64_t)count*8;
 for(unsigned i=0;i<8;i++)last[padded-1-i]=(uint8_t)(bits>>(8*i));
 block(h,last);if(padded==128)block(h,last+64);
 for(unsigned i=0;i<32;i++)digest[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
 m->profile.sha_bytes+=count;m->profile.sha_compute_us+=glv_now_us()-began;return 0;
}
