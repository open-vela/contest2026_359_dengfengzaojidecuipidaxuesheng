/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Select the official portable vector routines explicitly. Do not select
 * Xtensa kernels, unqualified P4 HWLP code or FreeRTOS dependencies. */
#define CONFIG_DSP_OPTIMIZED 0
