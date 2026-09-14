/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#if !defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3) && \
    !defined(CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL)
#error "Hosted BLE requires an explicitly enabled board integration"
#endif
#include "ble_hosted.h"
#include "esp_hosted.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

int esp_hosted_hci_claim(esp_hosted_rx_cb_t cb, void *arg);
int esp_hosted_hci_release(esp_hosted_rx_cb_t cb, void *arg);

struct c6_ble_hosted_s
{
  struct c6_ble_transport ops;
  pthread_mutex_t lock;
  pthread_t thread;
  bool running;
  bool started;
  c6_ble_rx_t receive;
  void *arg;
};

/* Impossible lifecycle errors cannot be recovered through the void close
 * API. Fail rather than return a false drain guarantee and allow use-after-free.
 */
static void require_success(int ret)
{
  if (ret != 0) abort();
}

static void receive_hci(void *arg, uint8_t interface, const uint8_t *data,
                        uint16_t length, uint8_t type)
{
  struct c6_ble_hosted_s *priv = arg;
  (void)interface;
  /* Callback/arg remain immutable from claim until release completes. */
  (void)priv->receive(priv->arg, type, data, length);
}

static void *poll_hci(void *arg)
{
  struct c6_ble_hosted_s *priv = arg;
  for (;;)
    {
      require_success(pthread_mutex_lock(&priv->lock));
      bool running = priv->running;
      require_success(pthread_mutex_unlock(&priv->lock));
      if (!running) break;
      int ret = esp_hosted_poll();
      if (ret < 0)
        {
          require_success(pthread_mutex_lock(&priv->lock));
          priv->running = false;
          require_success(pthread_mutex_unlock(&priv->lock));
          break;
        }
      usleep(10000);
    }
  return NULL;
}

static int start_hci(void *context, c6_ble_rx_t receive, void *arg)
{
  struct c6_ble_hosted_s *priv = context;
  if (!receive) return -EINVAL;
  if (priv->started) return -EBUSY;
  /* Capability memory must be stable: owner prepares Hosted before opening
   * BLE, and must not reset/reinitialize it while this transport is alive.
   */
  const struct esp_hosted_caps_s *caps = esp_hosted_get_caps();
  if (!caps || !caps->valid) return -ENETDOWN;
  if (!(caps->capability & ESP_HOSTED_CAP_BT_SDIO) ||
      !(caps->capability & ESP_HOSTED_CAP_BLE_ONLY)) return -ENOTSUP;
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);
  if (ret) return -ret;
  size_t stack = 8192;
#ifdef PTHREAD_STACK_MIN
  if (stack < PTHREAD_STACK_MIN) stack = PTHREAD_STACK_MIN;
#endif
  ret = pthread_attr_setstacksize(&attr, stack);
  if (ret) { pthread_attr_destroy(&attr); return -ret; }
  priv->receive = receive;
  priv->arg = arg;
  ret = esp_hosted_hci_claim(receive_hci, priv);
  if (ret < 0) { pthread_attr_destroy(&attr); return ret; }
  priv->running = true;
  ret = pthread_create(&priv->thread, &attr, poll_hci, priv);
  pthread_attr_destroy(&attr);
  if (ret)
    {
      priv->running = false;
      require_success(esp_hosted_hci_release(receive_hci, priv));
      priv->receive = NULL;
      priv->arg = NULL;
      return -ret;
    }
  priv->started = true;
  return 0;
}

static int send_hci(void *context, const uint8_t *data, size_t length)
{
  struct c6_ble_hosted_s *priv = context;
  if (!data || !length || length > 1029) return -EINVAL;
  int ret = pthread_mutex_lock(&priv->lock);
  if (ret) return -ret;
  ret = priv->running ? esp_hosted_send(ESP_HOSTED_IF_HCI, 0, data, length) : -ENOTCONN;
  pthread_mutex_unlock(&priv->lock);
  return ret;
}

static void stop_hci(void *context)
{
  struct c6_ble_hosted_s *priv = context;
  if (!priv->started) return;
  require_success(pthread_mutex_lock(&priv->lock));
  priv->running = false;
  require_success(pthread_mutex_unlock(&priv->lock));
  require_success(pthread_join(priv->thread, NULL));
  require_success(esp_hosted_hci_release(receive_hci, priv));
  priv->started = false;
  priv->receive = NULL;
  priv->arg = NULL;
}

struct c6_ble_transport *c6_ble_hosted_create(void)
{
  struct c6_ble_hosted_s *priv = calloc(1, sizeof(*priv));
  if (!priv) return NULL;
  int ret = pthread_mutex_init(&priv->lock, NULL);
  if (ret) { free(priv); errno = ret; return NULL; }
  priv->ops.context = priv;
  priv->ops.start = start_hci;
  priv->ops.stop = stop_hci;
  priv->ops.send = send_hci;
  return &priv->ops;
}

void c6_ble_hosted_destroy(struct c6_ble_transport *transport)
{
  if (!transport) return;
  struct c6_ble_hosted_s *priv = transport->context;
  stop_hci(priv);
  require_success(pthread_mutex_destroy(&priv->lock));
  free(priv);
}
