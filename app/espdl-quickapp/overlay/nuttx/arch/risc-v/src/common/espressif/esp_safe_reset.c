/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_safe_reset.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The P4 SDK reset disables cache. Neither its code nor its stack may remain
 * in external memory, even when the running application uses PSRAM XIP.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>

#include "esp32p4/rom/rtc.h"
#include "esp_private/system_internal.h"

#define SAFE_RESET_STACK_BYTES 2048

/* A shared reset stack is safe only after a hart atomically claims it. Keep
 * the claim in cleared internal BSS so every subsequent boot can reset again.
 * The assembly references are invisible to the C optimizer.
 */

static uint32_t g_esp_safe_reset_claim
  __attribute__((used, aligned(4)));
static uint8_t g_esp_safe_reset_stack[SAFE_RESET_STACK_BYTES]
  __attribute__((used, aligned(16)));

void esp_safe_reset_entry(void)
  __attribute__((naked, noinline, noreturn, section(".iram1.safe_reset")));

void esp_safe_reset_entry(void)
{
  __asm__ __volatile__(
    "csrci mstatus, 8\n"
    ".option push\n"
    ".option norelax\n"
    "la t0, g_esp_safe_reset_claim\n"
    "li t1, 1\n"
    "amoswap.w.aq t1, t1, (t0)\n"
    "bnez t1, 1f\n"
    "la sp, g_esp_safe_reset_stack + 2048\n"
    "tail esp_restart_noos\n"
    ".option pop\n"
    "1: wfi\n"
    "j 1b\n");
}

/* Ordinary cache-on diagnostic, never called from the cache-off closure.
 * Raw ROM reset reasons distinguish a direct software reset from WDT rescue.
 */

void esp_safe_reset_status(void)
{
  printf("SAFE_RESET cause0=%u cause1=%u entry=%p stack_low=%p "
         "stack_high=%p stack_bytes=%u\n",
         (unsigned)rtc_get_reset_reason(0),
         (unsigned)rtc_get_reset_reason(1),
         (void *)esp_safe_reset_entry, g_esp_safe_reset_stack,
         g_esp_safe_reset_stack + SAFE_RESET_STACK_BYTES,
         SAFE_RESET_STACK_BYTES);
}
