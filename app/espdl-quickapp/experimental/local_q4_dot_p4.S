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

/* w[groups,32], x[groups,batch,32], result[groups,batch].
 * Keep each group's four weight vectors live across the batch. Fused loads
 * consume the current activation before fetching the next; final MAC never
 * overreads. Q0-Q7/XACC are covered by the existing preemptible context hooks.
 * Only caller-clobbered scalar registers, no HWLP, FMA or IRQ masking. */
.section .text.glv_q4_dot_batch_pie,"ax",@progbits
.global glv_q4_dot_batch_pie
.type glv_q4_dot_batch_pie,@function
.balign 4
glv_q4_dot_batch_pie:
  mv t3,a2
  mv t4,a3
  li a2,0
1:
  esp.vld.128.ip q0,a0,16
  esp.vld.128.ip q2,a0,16
  esp.vld.128.ip q4,a0,16
  esp.vld.128.ip q6,a0,16
  mv t0,a4
2:
  esp.zero.xacc
  esp.vld.128.ip q1,a1,16
  esp.vmulas.s16.xacc.ld.ip q1,a1,16,q0,q1
  esp.vmulas.s16.xacc.ld.ip q1,a1,16,q2,q1
  esp.vmulas.s16.xacc.ld.ip q1,a1,16,q4,q1
  esp.vmulas.s16.xacc q6,q1
  esp.srs.s.xacc a3,a2
  sw a3,0(t3)
  addi t3,t3,4
  addi t0,t0,-1
  bnez t0,2b
  addi t4,t4,-1
  bnez t4,1b
  ret
.size glv_q4_dot_batch_pie,.-glv_q4_dot_batch_pie
