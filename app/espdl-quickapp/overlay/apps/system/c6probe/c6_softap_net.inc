/****************************************************************************
 * apps/system/c6probe/c6_softap_net.inc
 *
 * Included by c6net.c: AP/STA transitions share its connection mutex.  Large
 * packet storage is allocated once, not added to the internal boot BSS.
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct c6_softap_private_s
{
  struct c6net_state_s net;
  struct c6_softap_snapshot_s state;
  struct c6_wifi_phy_snapshot_s phy;
  bool phy_changed;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct c6_softap_private_s *g_c6ap;

/* Readers on the RPC/RX/network paths cannot acquire the connection mutex:
 * synchronous transactions invoke RX while already holding that mutex.
 */

static bool g_c6ap_requested;

/* A new P4 lifetime has no proof of the companion's mode. This also covers
 * APs retained by a P4-only reset before an AP netdev was allocated locally.
 * Protected by the connection mutex; no network operation from status().
 */

static int g_c6ap_mode_error = -EAGAIN;

#include "c6_wifi_phy_net.inc"

static void c6_softap_poll_pending(void)
{
  FAR struct c6_softap_private_s *ap;

  ap = __atomic_load_n(&g_c6ap, __ATOMIC_ACQUIRE);
  if (ap != NULL && __atomic_load_n(&ap->net.registered, __ATOMIC_ACQUIRE))
    {
      c6net_poll_pending(&ap->net);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int c6_softap_preserve_mode(int mode)
{
  return mode == 1 && __atomic_load_n(&g_c6ap_requested, __ATOMIC_ACQUIRE) ?
         3 : mode;
}

static void c6_softap_sync_carrier(void)
{
  FAR struct c6_softap_private_s *ap;

  ap = __atomic_load_n(&g_c6ap, __ATOMIC_ACQUIRE);
  if (ap != NULL && __atomic_load_n(&ap->net.registered, __ATOMIC_ACQUIRE))
    {
      netdev_lock(&ap->net.dev);
      if (__atomic_load_n(&g_c6ap_requested, __ATOMIC_ACQUIRE) &&
          (ap->net.dev.d_flags & IFF_UP) != 0)
        {
          netdev_carrier_on(&ap->net.dev);
        }
      else
        {
          netdev_carrier_off(&ap->net.dev);
        }

      netdev_unlock(&ap->net.dev);
    }
}

static void c6_softap_offline(FAR struct c6_softap_private_s *ap)
{
  __atomic_store_n(&g_c6ap_requested, false, __ATOMIC_RELEASE);
  ap->state.active = false;
  ap->phy.valid = false;
  ap->phy.clients_valid = false;
  ap->phy.sta_valid = false;
  if (ap->net.registered)
    {
      netdev_lock(&ap->net.dev);
      /* Notify pending forwarding/socket callbacks as well as the driver.
       * Directly clearing flags leaves their shared callback slots held.
       */

      (void)netdev_ifdown(&ap->net.dev);
      netdev_unlock(&ap->net.dev);
    }
}

/* Startup owns the normal STA mode unless this P4 lifetime owns an AP. The
 * caller already initialized Wi-Fi as part of its ordinary setup. Confirm
 * the applied mode before it publishes an initialized host interface.
 */

static int c6_softap_setup_mode(void)
{
  int expected = c6_softap_preserve_mode(1);
  int observed = -1;
  int ret = esp_hosted_rpc_wifi_set_mode(expected);

  if (ret == 0)
    {
      ret = esp_hosted_rpc_wifi_get_mode(&observed);
      if (ret == 0 && observed != expected)
        {
          ret = -EPROTO;
        }
    }

  g_c6ap_mode_error = ret;
  if (g_c6ap != NULL)
    {
      g_c6ap->state.state_unknown = ret != 0;
      g_c6ap->state.error = ret;
    }

  return ret;
}

static int c6_softap_register(FAR struct c6_softap_private_s *ap)
{
  FAR struct c6net_state_s *priv = &ap->net;
  uint8_t mac[6];
  int ret;

  if (priv->registered)
    {
      return 0;
    }

  ret = esp_hosted_rpc_get_mac(1, mac);
  if (ret < 0)
    {
      return ret;
    }

  if ((mac[0] & 1) != 0 ||
      memcmp(mac, "\0\0\0\0\0\0", 6) == 0)
    {
      return -EPROTO;
    }

  priv->if_type = ESP_HOSTED_IF_AP;
  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, 6);
  strlcpy(priv->dev.d_ifname, C6_SOFTAP_IFNAME_FORMAT,
          sizeof(priv->dev.d_ifname));
  priv->dev.d_llhdrlen = 14;
  priv->dev.d_pktsize = CONFIG_NET_ETH_PKTSIZE;
  priv->dev.d_buf = priv->buf;
  priv->dev.d_ifup = c6net_ifup;
  priv->dev.d_ifdown = c6net_ifdown;
  priv->dev.d_txavail = c6net_txavail;
  priv->dev.d_private = priv;

  ret = esp_hosted_register(ESP_HOSTED_IF_AP, c6net_rx, priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      (void)esp_hosted_register(ESP_HOSTED_IF_AP, NULL, NULL);
      return ret;
    }

  __atomic_store_n(&priv->registered, true, __ATOMIC_RELEASE);
  strlcpy(ap->state.ifname, priv->dev.d_ifname, sizeof(ap->state.ifname));
  return 0;
}

/* Restore only the Wi-Fi mode and former reconnect policy.  No old/new STA
 * credentials are copied, erased, or committed by the hotspot lifecycle.
 */

static int c6_softap_restore_sta(bool stopped, FAR bool *unknown)
{
  int mode = 0;
  int ret = esp_hosted_rpc_wifi_set_mode(1);

  if (ret == 0 && stopped)
    {
      ret = esp_hosted_rpc_wifi_start();
    }

  if (ret == 0)
    {
      ret = esp_hosted_rpc_wifi_get_mode(&mode);
      if (ret == 0 && mode != 1)
        {
          ret = -EPROTO;
        }
    }

  *unknown = ret != 0;
  if (ret == 0 && g_c6net_reconnect)
    {
      ret = esp_hosted_rpc_wifi_connect("", "");
    }

  return ret;
}

int c6_softap_start(FAR const char *ssid, FAR const char *password)
{
  FAR struct c6_softap_private_s *ap;
  bool stopped = false;
  bool changed = false;
  size_t ssidlen;
  size_t passlen;
  int mode = 0;
  int ret;

  if (ssid == NULL || password == NULL)
    {
      return -EINVAL;
    }

  ssidlen = strnlen(ssid, C6_SOFTAP_SSID_MAX + 1);
  passlen = strnlen(password, C6_SOFTAP_PASSWORD_MAX + 1);
  if (ssidlen == 0 || ssidlen > C6_SOFTAP_SSID_MAX ||
      passlen < C6_SOFTAP_PASSWORD_MIN || passlen > C6_SOFTAP_PASSWORD_MAX)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < passlen; i++)
    {
      if ((unsigned char)password[i] < 32 ||
          (unsigned char)password[i] > 126)
        {
          return -EINVAL;
        }
    }

  ret = c6net_prepare();
  if (ret < 0)
    {
      return ret;
    }

  ret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (c6_wifi_trial_pending())
    {
      ret = -EBUSY;
      goto out;
    }

  ap = g_c6ap;
  if (ap == NULL)
    {
      ap = calloc(1, sizeof(*ap));
      if (ap == NULL)
        {
          ret = -ENOMEM;
          goto out;
        }

      __atomic_store_n(&g_c6ap, ap, __ATOMIC_RELEASE);
    }

  if (ap->state.active || ap->state.state_unknown)
    {
      ret = ap->state.active ? -EALREADY : -EBUSY;
      goto out;
    }

  memset(&ap->state, 0, sizeof(ap->state));
  uint32_t generation = ap->phy.generation + 1;
  memset(&ap->phy, 0, sizeof(ap->phy));
  ap->phy.generation = generation;
  ap->phy.preferred = g_c6ap_he_preferred;
  ap->phy_changed = false;
  if (ap->net.registered)
    {
      strlcpy(ap->state.ifname, ap->net.dev.d_ifname,
              sizeof(ap->state.ifname));
    }

  stopped = true;             /* Stop timeout can still have stopped Wi-Fi. */
  ret = esp_hosted_rpc_wifi_stop();
  changed = true;             /* Even a lost reply may have changed the C6. */
  ap->state.sta_interrupted = true;
  (void)c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
  if (ret < 0)
    {
      goto failed;
    }

  ret = esp_hosted_rpc_wifi_set_mode(3);
  if (ret < 0)
    {
      goto failed;
    }

  ret = esp_hosted_rpc_softap_configure(ssid, password);
  if (ret < 0)
    {
      goto failed;
    }

  ret = esp_hosted_rpc_softap_verify(ssid, password);
  if (ret < 0)
    {
      goto failed;
    }

  ret = c6_softap_phy_apply(ap);
  if (ret < 0)
    {
      goto failed;
    }

  ret = c6_softap_register(ap);
  if (ret < 0)
    {
      goto failed;
    }

  ret = esp_hosted_rpc_wifi_start();
  if (ret < 0)
    {
      goto failed;
    }

  stopped = false;
  ret = esp_hosted_rpc_wifi_get_mode(&mode);
  if (ret == 0 && mode != 3)
    {
      ret = -EPROTO;
    }

  if (ret < 0)
    {
      goto failed;
    }

  ret = c6_softap_phy_verify(ap);
  if (ret < 0)
    {
      goto failed;
    }

  __atomic_store_n(&g_c6ap_requested, true, __ATOMIC_RELEASE);
  netdev_lock(&ap->net.dev);
  (void)c6net_ifup(&ap->net.dev);
  netdev_carrier_on(&ap->net.dev);
  netdev_unlock(&ap->net.dev);
  ap->state.active = true;
  memcpy(ap->state.ssid, ssid, ssidlen + 1);

  if (g_c6net_reconnect)
    {
      /* AP operation is already confirmed. A STA reconnect error must not
       * turn a functioning local recovery network into a failed start.
       */

      ap->state.restore_error = esp_hosted_rpc_wifi_connect("", "");
    }

  ret = 0;
  goto out;

failed:
  c6_softap_offline(ap);
  if (ap->phy_changed && ap->phy.restore_error == 0)
    {
      ap->phy.restore_error = c6_softap_phy_restore(ap);
    }
  ap->state.error = ret;
  ap->state.restore_error = changed ?
    c6_softap_restore_sta(stopped, &ap->state.state_unknown) : 0;
  ap->state.state_unknown |= ap->phy.restore_error != 0;
out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6_softap_stop(void)
{
  FAR struct c6_softap_private_s *ap;
  int mode = 0;
  int ret = pthread_mutex_lock(&g_c6net_connect_lock);

  if (ret != 0)
    {
      return -ret;
    }

  ap = g_c6ap;
  if (c6_wifi_trial_pending())
    {
      ret = -EBUSY;
      goto out;
    }

  if (ap != NULL)
    {
      c6_softap_offline(ap);
      if (ap->phy.restore_error == -EUCLEAN)
        {
          ret = esp_hosted_rpc_phy_storage_restore();
          if (ret != 0)
            {
              ap->state.state_unknown = true;
              ap->state.error = ret;
              goto out;
            }
        }
    }

  /* Never infer the companion's state from this P4's heap object. In
   * particular, do not call prepare()/WifiStart to close an unowned AP.
   * Preserve the STA bit: AP+STA -> STA, AP-only -> NULL.
   */

  ret = esp_hosted_rpc_wifi_get_mode(&mode);
  if (ret == 0)
    {
      if (mode < 0 || mode > 3)
        {
          ret = -EPROTO;
        }
      else if ((mode & 2) != 0)
        {
          int expected = mode & 1;
          ret = esp_hosted_rpc_wifi_set_mode(expected);
          if (ret == 0)
            {
              ret = esp_hosted_rpc_wifi_get_mode(&mode);
              if (ret == 0 && mode != expected)
                {
                  ret = -EPROTO;
                }
            }
        }
    }

  g_c6ap_mode_error = ret;
  if (ap != NULL)
    {
      ap->state.error = ret;
      ap->state.state_unknown = ret != 0;
      if (ret == 0)
        {
          ap->state.ssid[0] = '\0';
        }
    }

out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6_softap_status(FAR struct c6_softap_snapshot_s *out)
{
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  ret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_c6ap == NULL)
    {
      memset(out, 0, sizeof(*out));
      out->state_unknown = g_c6ap_mode_error != 0;
      out->error = g_c6ap_mode_error;
    }
  else
    {
      *out = g_c6ap->state;
    }

  pthread_mutex_unlock(&g_c6net_connect_lock);
  return 0;
}
