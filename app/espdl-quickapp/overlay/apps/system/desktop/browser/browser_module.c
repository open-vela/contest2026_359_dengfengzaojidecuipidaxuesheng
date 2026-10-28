/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_module.h"
#include "browser_host_abi.h"
#include "openvela_desktop.h"

const struct ov_browser_module *ov_browser_module_query(void)
{
	static const struct ov_browser_module api = {
		OV_BROWSER_MODULE_ABI, sizeof(struct ov_browser_module),
		OV_BROWSER_HOST_ABI, ov_desktop_netsurf_install, ov_desktop_netsurf_stop
	};
	return &api;
}
