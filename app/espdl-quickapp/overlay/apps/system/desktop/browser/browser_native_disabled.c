/* SPDX-License-Identifier: GPL-2.0-only */
#include "browser_native_host.h"
#include <string.h>

/* No native module, executable allocation, worker or retained page state. */
bool ov_browser_native_install(JSContext *context, JSValue system,
                               lv_obj_t *root, const char *package,
                               const char *entry)
{
  (void)root;
  (void)entry;
  if (!package || strcmp(package, "com.netsurf.browser"))
    {
      return true;
    }

  JS_SetPropertyStr(context, system, "browserLoading", JS_FALSE);
  JS_SetPropertyStr(context, system, "browserError",
                    JS_NewString(context, "此固件已停用原生浏览器"));
  return false;
}

void ov_browser_native_stop(void)
{
}
