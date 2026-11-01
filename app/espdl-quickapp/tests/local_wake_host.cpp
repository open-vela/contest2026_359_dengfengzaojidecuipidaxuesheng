/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_wake.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
static bool card=true;
extern "C" bool glv_platform_storage_ready(void){return card;}
int main()
{
  int16_t zero[2000]={0};
  for(unsigned loop=0;loop<20;loop++){
    glv_wake_test_skew(loop%16);
    glv_wake *wake=nullptr,*other=nullptr;assert(!glv_wake_create(&wake));assert(glv_wake_create(&other)==-EBUSY);
    for(unsigned i=0;i<24;i++)assert(!glv_wake_feed(wake,zero,2000));
    assert(glv_wake_evaluations(wake)==3);assert(glv_wake_score(wake)<.6f);
    glv_wake_reset(wake);card=false;assert(glv_wake_feed(wake,zero,2000)==-ENODEV);card=true;glv_wake_destroy(wake);
  }
  puts("LOCAL_WAKE_OK original MFCC/EON; sole detector; reset/card removal/20 cleanup cycles");
}
