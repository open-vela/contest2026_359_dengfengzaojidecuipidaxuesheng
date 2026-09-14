/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#if !defined(CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL) || \
    !defined(CONFIG_NET_BLUETOOTH) || defined(CONFIG_WIRELESS_BLUETOOTH_HOST)
#error "C6 NimBLE requires v3 raw HCI networking without the internal host"
#endif
#include "ble_hosted.h"
#include "ble_register.h"
#include "c6net.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* NuttX's raw HCI core supports one controller. Keep registration serialized
 * for the process lifetime; successful registration transfers ownership. */
static pthread_mutex_t g_registration_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_registered;

/* The command keeps its existing signature, but this build registers hci0
 * for NimBLE's raw socket transport, not a UART character device. */
int c6_ble_register(const char *path)
{
  if (!path || strcmp(path, "/dev/ttyHCI0")) return -EINVAL;
  int ret = pthread_mutex_lock(&g_registration_lock);
  if (ret != 0) return -ret;
  if (g_registered)
    {
      pthread_mutex_unlock(&g_registration_lock);
      return 0;
    }
  ret = c6net_prepare();
  if (ret < 0) goto out;
  struct c6_ble_transport *transport = c6_ble_hosted_create();
  if (!transport)
    {
      ret = -(errno ? errno : ENOMEM);
      goto out;
    }
  struct bt_driver_s *driver = c6_ble_driver_create(transport);
  if (!driver)
    {
      int error = errno ? errno : ENOMEM;
      c6_ble_hosted_destroy(transport);
      ret = -error;
      goto out;
    }
  ret = bt_netdev_register(driver);
  if (ret < 0)
    {
      c6_ble_driver_destroy(driver);
      c6_ble_hosted_destroy(transport);
    }
  else g_registered = true;
out:
  pthread_mutex_unlock(&g_registration_lock);
  return ret;
}
