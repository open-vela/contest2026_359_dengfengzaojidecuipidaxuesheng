/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_wake_features.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
  float *bank=malloc(GLASS_WAKE_WEIGHTS*sizeof(float));assert(bank);
  float feature[GLASS_WAKE_INPUTS]={0},distance;
  for(unsigned i=0;i<GLASS_WAKE_WEIGHTS;i++)bank[i]=i<GLASS_WAKE_POSITIVE*GLASS_WAKE_INPUTS?1:2;
  float ratio=glass_wake_ratio_distance(bank,feature,&distance);
  assert(fabsf(distance-1)<.00001f&&fabsf(ratio-.25f)<.00001f);
  assert(ratio==glass_wake_ratio(bank,feature));
  /* A ratio can look decisive even when every positive template is far away. */
  for(unsigned i=GLASS_WAKE_POSITIVE*GLASS_WAKE_INPUTS;i<GLASS_WAKE_WEIGHTS;i++)bank[i]=1.1f;
  ratio=glass_wake_ratio_distance(bank,feature,&distance);
  assert(fabsf(distance-1)<.00001f&&ratio<.91647577f);
  assert(ratio==glass_wake_ratio_distance(bank,feature,NULL));
  /* Absolute evidence uses the same three-nearest positive mean as the ratio. */
  for(unsigned t=0;t<3;t++)for(unsigned k=0;k<GLASS_WAKE_INPUTS;k++)bank[t*GLASS_WAKE_INPUTS+k]=.1f*(t+1);
  ratio=glass_wake_ratio_distance(bank,feature,&distance);
  assert(fabsf(distance-(.01f+.04f+.09f)/3)<.00001f);
  assert(ratio==glass_wake_ratio(bank,feature));free(bank);
  puts("WAKE_MATCH_DISTANCE_OK: actual DTW distance, relative-only counterexample, three-nearest mean and legacy ratio compatibility; ASan/UBSan");
}
