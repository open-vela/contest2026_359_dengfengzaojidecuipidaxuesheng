"""Prepare a v1 HCI ownership extension without modifying the source tree."""
import argparse
import difflib
from pathlib import Path

from adapt_c6_rx_dispatch import once


def adapt(source, *, v3=False):
    if 'static pthread_mutex_t g_rx_dispatch_lock' not in source:
        raise ValueError('Serialized RX dispatch prerequisite missing')
    if 'esp_hosted_hci_claim' in source:
        raise ValueError('HCI ownership extension already present')
    source = once(source, 'int esp_hosted_register(uint8_t if_type, esp_hosted_rx_cb_t cb,',
                  '''#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
static bool g_hci_claimed;

/* Claim/release run outside RX callbacks. The existing RX lock also drains
 * callbacks before release returns. The caller must serialize its lifecycle.
 */
int esp_hosted_hci_claim(esp_hosted_rx_cb_t cb, FAR void *arg)
{
  int ret;

  if (cb == NULL || arg == NULL)
    {
      return -EINVAL;
    }

  ret = pthread_mutex_lock(&g_rx_dispatch_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_hci_claimed || g_hosted.rx_cb[ESP_HOSTED_IF_HCI] != NULL)
    {
      ret = -EBUSY;
    }
  else
    {
      g_hosted.rx_cb[ESP_HOSTED_IF_HCI] = cb;
      g_hosted.rx_arg[ESP_HOSTED_IF_HCI] = arg;
      g_hci_claimed = true;
      ret = 0;
    }

  pthread_mutex_unlock(&g_rx_dispatch_lock);
  return ret;
}

int esp_hosted_hci_release(esp_hosted_rx_cb_t cb, FAR void *arg)
{
  int ret = pthread_mutex_lock(&g_rx_dispatch_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (!g_hci_claimed || g_hosted.rx_cb[ESP_HOSTED_IF_HCI] != cb ||
      g_hosted.rx_arg[ESP_HOSTED_IF_HCI] != arg)
    {
      ret = -EPERM;
    }
  else
    {
      g_hosted.rx_cb[ESP_HOSTED_IF_HCI] = NULL;
      g_hosted.rx_arg[ESP_HOSTED_IF_HCI] = NULL;
      g_hci_claimed = false;
      ret = 0;
    }

  pthread_mutex_unlock(&g_rx_dispatch_lock);
  return ret;
}
#endif

int esp_hosted_register(uint8_t if_type, esp_hosted_rx_cb_t cb,''')
    source = once(source, '  g_hosted.rx_cb[if_type]  = cb;', '''#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  if (if_type == ESP_HOSTED_IF_HCI && g_hci_claimed)
    {
      pthread_mutex_unlock(&g_rx_dispatch_lock);
      return -EBUSY;
    }
#endif
  g_hosted.rx_cb[if_type]  = cb;''')
    if v3:
        source = source.replace('#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3\nstatic bool g_hci_claimed;',
                                '#ifdef CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL\nstatic bool g_hci_claimed;', 1)
        source = source.replace('#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3\n  if (if_type == ESP_HOSTED_IF_HCI && g_hci_claimed)',
                                '#ifdef CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL\n  if (if_type == ESP_HOSTED_IF_HCI && g_hci_claimed)', 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    before = args.source.read_text()
    after = adapt(before)
    print(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                     fromfile='a/esp_hosted.c', tofile='b/esp_hosted.c')))


if __name__ == '__main__':
    main()
