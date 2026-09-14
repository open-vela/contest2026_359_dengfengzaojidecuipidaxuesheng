/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#if (!defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3) && \
     !defined(CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL)) || !defined(CONFIG_UART_BTH4)
#error "C6 BLE registration requires a supported board and UART_BTH4"
#endif
#include <nuttx/serial/uart_bth4.h>
#include <errno.h>
#include <string.h>
#include "ble_hosted.h"
#include "ble_register.h"

int c6_ble_register(const char *path)
{
  if (!path || strncmp(path, "/dev/ttyHCI", 11) || !path[11])
    return -EINVAL;
  for (const char *p = path + 11; *p; p++)
    if (*p < '0' || *p > '9') return -EINVAL;

  struct c6_ble_transport *transport = c6_ble_hosted_create();
  if (!transport) return -errno;
  struct bt_driver_s *driver = c6_ble_driver_create(transport);
  if (!driver)
    {
      int error = errno;
      c6_ble_hosted_destroy(transport);
      return -error;
    }

  int ret = uart_bth4_register(path, driver);
  if (ret < 0)
    {
      c6_ble_driver_destroy(driver);
      c6_ble_hosted_destroy(transport);
    }
  return ret;
}
