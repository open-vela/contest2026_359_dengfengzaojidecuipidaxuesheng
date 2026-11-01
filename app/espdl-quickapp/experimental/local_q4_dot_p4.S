/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed 32-element signed dot. Both pointers are 16-byte aligned.
 * Weights are [-8,7], activations [-32767,32767], so signed int32 is ample.
 * No speculative next-block load, hardware loop, stack or interrupt masking.
 * Caller enables PIE; the existing NuttX context hooks preserve XACC/Q state. */
.option arch, +xespv
.section .text.glv_q4_dot32_pie,"ax",@progbits
.global glv_q4_dot32_pie
.type glv_q4_dot32_pie,@function
.balign 4
glv_q4_dot32_pie:
  esp.zero.xacc
  .rept 4
  esp.vld.128.ip q0,a0,16
  esp.vld.128.ip q1,a1,16
  esp.vmulas.s16.xacc q0,q1
  .endr
  li a2,0
  esp.srs.s.xacc a0,a2
  ret
.size glv_q4_dot32_pie,.-glv_q4_dot32_pie
