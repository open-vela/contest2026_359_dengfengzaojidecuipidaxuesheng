/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLV_EXPERIMENTAL_ASR_OPS_H
#define GLV_EXPERIMENTAL_ASR_OPS_H
#include "glass_local_model.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The graph
 * runner owns/counts input/output tensors; these functions count scratch only.
 * Weight rows are [output_channel, flattened_input_kernel], allowing SD/Q4/Q8
 * readers without a resident matrix. A callback error aborts, never retries. */
struct glv_asr_rows {
  int (*read)(void *,unsigned,float *,unsigned);
  void *opaque;
#ifdef GLV_ASR_ACCEL_ENABLED
  /* Owner-only: read once, decode original floats and optional raw Q4/Q8
   * codes/scales. Returns 1 for packed, 0 for FP32, negative on error. */
  int (*prepare)(void *,unsigned,float *,int16_t *,float *,float *,unsigned);
#endif
};
struct glv_asr_conv {
  unsigned channels,height,width,outputs,kernel_h,kernel_w,groups;
  unsigned stride_h,stride_w,dilation_h,dilation_w;
  unsigned top,left,bottom,right;
};
struct glv_asr_shape {unsigned rank;uint32_t dims[8];};
enum glv_asr_unary {GLV_ASR_NEG,GLV_ASR_RELU,GLV_ASR_SIGMOID,GLV_ASR_TANH,GLV_ASR_RECIPROCAL};
enum glv_asr_binary {GLV_ASR_ADD,GLV_ASR_SUB,GLV_ASR_MUL,GLV_ASR_DIV,GLV_ASR_POW};
int glv_asr_matmul(struct glv_memory *,const float *,size_t,float *,size_t,
                   unsigned,unsigned,unsigned,const struct glv_asr_rows *);
/* N=1 NCHW. Conv1d is represented by H=kernel_h=stride_h=dilation_h=1. */
int glv_asr_conv(struct glv_memory *,const float *,size_t,const float *,size_t,
                 float *,size_t,const struct glv_asr_conv *,const struct glv_asr_rows *);
int glv_asr_unary(struct glv_memory *,enum glv_asr_unary,const float *,float *,size_t);
int glv_asr_binary(struct glv_memory *,enum glv_asr_binary,const float *,size_t,const struct glv_asr_shape *,
                   const float *,size_t,const struct glv_asr_shape *,float *,size_t);
int glv_asr_softmax(struct glv_memory *,const float *,size_t,const struct glv_asr_shape *,unsigned,float *,size_t);
/* axes is a bit mask over rank; keepdims changes shape only, not flat order. */
int glv_asr_reduce(struct glv_memory *,const float *,size_t,const struct glv_asr_shape *,unsigned,bool,float *,size_t);
/* GEMM operands normalized to A[M,K], weight rows [N,K] by the graph loader. */
int glv_asr_gemm(struct glv_memory *,const float *,size_t,float *,size_t,unsigned,unsigned,unsigned,
                 const struct glv_asr_rows *,float,float,const float *,size_t,const struct glv_asr_shape *);
#ifdef __cplusplus
}
#endif
#endif
