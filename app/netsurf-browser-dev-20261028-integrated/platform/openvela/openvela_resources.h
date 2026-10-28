/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_RESOURCES_H
#define NETSURF_OPENVELA_RESOURCES_H
#include "utils/errors.h"
#include "netsurf/fetch.h"
struct gui_fetch_table *ov_resource_fetch_table(void);
nserror ov_core_options_init(void);
void ov_core_options_fini(void);
#endif
