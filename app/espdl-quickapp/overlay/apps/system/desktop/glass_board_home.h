/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
uint32_t glass_board_home_revision(void *);
char *glass_board_home_local_checked(void *,const char *,const char *,bool,uint32_t,atomic_bool *);
/* Registers the in-process smart home MCP tools on the board server.
 * Compiled only with CONFIG_SYSTEM_HASS; called once at desktop startup. */
void glass_board_home_register(void);
