/* SPDX-License-Identifier: Apache-2.0 */
/* Private NuttX compatibility types for the selected unmodified ESP-DSP C
 * sources. This directory is used only for those objects and their adapter. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int32_t esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
