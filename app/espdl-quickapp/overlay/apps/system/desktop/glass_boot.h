/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_BOOT_H
#define GLASS_BOOT_H
#include <stdbool.h>
bool glass_boot_enter(void);
bool glass_boot_active(void);
bool glass_boot_ui_ready(void);
void glass_boot_set_ui_ready(void);
void glass_boot_stage(const char *stage,int result);
void glass_boot_report(void);
#endif
