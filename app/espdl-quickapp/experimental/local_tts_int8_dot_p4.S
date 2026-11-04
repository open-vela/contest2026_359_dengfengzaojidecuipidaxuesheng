/* SPDX-License-Identifier: Apache-2.0 */
/* Signed-8 PIE dot, pipelined MAC + next-weight load.
 * Inputs aligned to 16 bytes; n positive, multiple of sixteen, <= 256.
 * Caller restricts weights to [-127,127] and activations to [-127,127].
 * Preload the first pair; fused steps consume the old q0 while loading the
 * next weights into q0. The final dot does not load, so no tail overread.
 * Keep the original ascending sum order, 256-item boundaries and q0/q1 set. */
    .section .text.glv_tts_dot_int8_pie,"ax",@progbits
    .option arch, +xespv
    .align 2
    .global glv_tts_dot_int8_pie
    .type glv_tts_dot_int8_pie,@function
glv_tts_dot_int8_pie:
    esp.zero.xacc
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    addi a2,a2,-16
    li t0,64
    bltu a2,t0,2f
1:
    .rept 4
    esp.vmulas.s8.xacc.ld.ip q0, a0, 16, q0, q1
    esp.vld.128.ip q1, a1, 16
    .endr
    addi a2,a2,-64
    bgeu a2,t0,1b
2:
    beqz a2,4f
3:
    esp.vmulas.s8.xacc.ld.ip q0, a0, 16, q0, q1
    esp.vld.128.ip q1, a1, 16
    addi a2, a2, -16
    bgtz a2, 3b
4:
    esp.vmulas.s8.xacc q0,q1
    li a2, 0
    esp.srs.s.xacc a0, a2
    ret
    .size glv_tts_dot_int8_pie,.-glv_tts_dot_int8_pie

/* w[rows,stride], x[patches,stride], result[rows,patches,ceil(stride/256)].
 * All dot products retain the exact original 256-element reduction boundary.
 * One call computes the whole tile; IRQ/task switching remains enabled. */
    .section .text.glv_tts_dot_batch_int8_pie,"ax",@progbits
    .align 2
    .global glv_tts_dot_batch_int8_pie
    .type glv_tts_dot_batch_int8_pie,@function
glv_tts_dot_batch_int8_pie:
    addi sp,sp,-32
    sw s0,0(sp)
    sw s1,4(sp)
    sw s2,8(sp)
    sw s3,12(sp)
    sw s4,16(sp)
    sw s5,20(sp)
    sw s6,24(sp)
    mv s0,a0
    mv s1,a1
    mv s2,a2
    mv s3,a3
    mv s4,a4
    mv s5,a5
    mv s6,a2
3:
    mv t0,s1
    mv t1,s4
4:
    mv a0,s0
    mv a1,t0
    mv t4,s6
5:
    li t5,256
    bgeu t4,t5,6f
    mv t5,t4
6:
    mv t6,t5
    esp.zero.xacc
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    addi t6,t6,-16
    li a2,64
    bltu t6,a2,8f
7:
    .rept 4
    esp.vmulas.s8.xacc.ld.ip q0,a0,16,q0,q1
    esp.vld.128.ip q1,a1,16
    .endr
    addi t6,t6,-64
    bgeu t6,a2,7b
8:
    beqz t6,10f
9:
    esp.vmulas.s8.xacc.ld.ip q0,a0,16,q0,q1
    esp.vld.128.ip q1,a1,16
    addi t6,t6,-16
    bgtz t6,9b
10:
    esp.vmulas.s8.xacc q0,q1
    li a2,0
    esp.srs.s.xacc a3,a2
    sw a3,0(s5)
    addi s5,s5,4
    sub t4,t4,t5
    bnez t4,5b
    add t0,t0,s2
    addi t1,t1,-1
    bnez t1,4b
    add s0,s0,s2
    addi s3,s3,-1
    bnez s3,3b
    lw s0,0(sp)
    lw s1,4(sp)
    lw s2,8(sp)
    lw s3,12(sp)
    lw s4,16(sp)
    lw s5,20(sp)
    lw s6,24(sp)
    addi sp,sp,32
    ret
    .size glv_tts_dot_batch_int8_pie,.-glv_tts_dot_batch_int8_pie

/* w[rows,stride], x[patches,stride], result[rows,patches].
 * One full-row signed-8 accumulator: stride <= 2288, abs(sum) <= 37486592 < INT32_MAX.
 * One call computes the whole tile; IRQ/task switching remains enabled. */
    .section .text.glv_tts_dot_batch_full_int8_pie,"ax",@progbits
    .align 2
    .global glv_tts_dot_batch_full_int8_pie
    .type glv_tts_dot_batch_full_int8_pie,@function
glv_tts_dot_batch_full_int8_pie:
    addi sp,sp,-32
    sw s0,0(sp)
    sw s1,4(sp)
    sw s2,8(sp)
    sw s3,12(sp)
    sw s4,16(sp)
    sw s5,20(sp)
    sw s6,24(sp)
    mv s0,a0
    mv s1,a1
    mv s2,a2
    mv s3,a3
    mv s4,a4
    mv s5,a5
    mv s6,a2
3:
    mv t0,s1
    mv t1,s4
4:
    mv a0,s0
    mv a1,t0
    mv t4,s6
5:
    mv t5,t4
    bgeu t4,t5,6f
    mv t5,t4
6:
    mv t6,t5
    esp.zero.xacc
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    addi t6,t6,-16
    li a2,64
    bltu t6,a2,8f
7:
    .rept 4
    esp.vmulas.s8.xacc.ld.ip q0,a0,16,q0,q1
    esp.vld.128.ip q1,a1,16
    .endr
    addi t6,t6,-64
    bgeu t6,a2,7b
8:
    beqz t6,10f
9:
    esp.vmulas.s8.xacc.ld.ip q0,a0,16,q0,q1
    esp.vld.128.ip q1,a1,16
    addi t6,t6,-16
    bgtz t6,9b
10:
    esp.vmulas.s8.xacc q0,q1
    li a2,0
    esp.srs.s.xacc a3,a2
    sw a3,0(s5)
    addi s5,s5,4
    sub t4,t4,t5
    bnez t4,5b
    add t0,t0,s2
    addi t1,t1,-1
    bnez t1,4b
    add s0,s0,s2
    addi s3,s3,-1
    bnez s3,3b
    lw s0,0(sp)
    lw s1,4(sp)
    lw s2,8(sp)
    lw s3,12(sp)
    lw s4,16(sp)
    lw s5,20(sp)
    lw s6,24(sp)
    addi sp,sp,32
    ret
    .size glv_tts_dot_batch_full_int8_pie,.-glv_tts_dot_batch_full_int8_pie

/* Both lanes use different vectors. Keep Q0/Q1 and a live XACC across a
 * voluntary scheduler boundary, then consume them again. No HWLP state. */
    .align 4
    .global glv_tts_int8_context_probe
    .type glv_tts_int8_context_probe,@function
glv_tts_int8_context_probe:
    addi sp,sp,-32
    sw ra,0(sp)
    sw s0,4(sp)
    sw s1,8(sp)
    sw s2,12(sp)
    sw s3,16(sp)
    mv s0,a0
    mv s1,a1
    mv s2,a2
    mv s3,a3
11:
    mv a0,s0
    mv a1,s1
    esp.zero.xacc
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    esp.vmulas.s8.xacc q0,q1
    call sched_yield
    esp.vmulas.s8.xacc q0,q1
    li a2,0
    esp.srs.s.xacc a3,a2
    bne a3,s2,12f
    addi s3,s3,-1
    bnez s3,11b
    li a0,0
    j 13f
12:
    li a0,1
13:
    lw ra,0(sp)
    lw s0,4(sp)
    lw s1,8(sp)
    lw s2,12(sp)
    lw s3,16(sp)
    addi sp,sp,32
    ret
    .size glv_tts_int8_context_probe,.-glv_tts_int8_context_probe

/* LLM-only, aligned vectors; one exact signed32 group per32 codes.
 * FP32 integer*activation_scale, then *weight_scale, then ascending add.
 * Caller owns/enables PIE; q0/q1 caller-saved, no overread or stack use. */
    .section .text.glv_llm_dot_groups_pie,"ax",@progbits
    .align 2
    .global glv_llm_dot_groups_pie
    .type glv_llm_dot_groups_pie,@function
glv_llm_dot_groups_pie:
    fmv.s ft0,fa0
    fmv.w.x fa0,zero
1:
    esp.zero.xacc
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    esp.vmulas.s8.xacc.ld.ip q0,a0,16,q0,q1
    esp.vld.128.ip q1,a1,16
    esp.vmulas.s8.xacc q0,q1
    li a4,0
    esp.srs.s.xacc a5,a4
    fcvt.s.w ft1,a5
    flw ft2,0(a2)
    fmul.s ft1,ft1,ft2
    fmul.s ft1,ft1,ft0
    fadd.s fa0,fa0,ft1
    addi a2,a2,4
    addi a3,a3,-1
    bnez a3,1b
    ret
    .size glv_llm_dot_groups_pie,.-glv_llm_dot_groups_pie
