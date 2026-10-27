/****************************************************************************
 * apps/system/c6probe/c6_wifi_phy_net.inc
 *
 * Included with the AP private state. All access uses the connection mutex.
 * PHY policy is RAM-only; only the AP interface is written.
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One companion radio, matching the existing connection mutex singleton. */

static bool g_c6ap_he_preferred = true;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int c6_phy_get(int iface, FAR struct c6_wifi_phy_config_s *out)
{
  struct c6_wifi_phy_config_s value;
  int ret = esp_hosted_rpc_phy_get_protocol(iface, &value.protocol);
  if (ret == 0)
    {
      ret = esp_hosted_rpc_phy_get_bandwidth(iface, &value.bandwidth);
    }

  if (ret == 0)
    {
      *out = value;
    }

  return ret;
}

static bool c6_phy_equal(FAR const struct c6_wifi_phy_config_s *a,
                         FAR const struct c6_wifi_phy_config_s *b)
{
  return a->protocol == b->protocol && a->bandwidth == b->bandwidth;
}

static int c6_phy_program(FAR const struct c6_wifi_phy_config_s *value)
{
  struct c6_wifi_phy_config_s observed;
  int ret;

  /* Remove HT40 before enabling HE. Restore a legacy protocol before HT40. */

  if (value->bandwidth == C6_WIFI_BW20)
    {
      ret = esp_hosted_rpc_phy_set_bandwidth(1, value->bandwidth);
      if (ret == 0)
        {
          ret = esp_hosted_rpc_phy_set_protocol(1, value->protocol);
        }
    }
  else
    {
      ret = esp_hosted_rpc_phy_set_protocol(1, value->protocol);
      if (ret == 0)
        {
          ret = esp_hosted_rpc_phy_set_bandwidth(1, value->bandwidth);
        }
    }

  /* A lost setter reply may follow a successful mutation. Readback is the
   * authority; never replay that setter merely because its reply was lost.
   */

  if (ret == -EUCLEAN)
    {
      return ret; /* PHY readback cannot prove the global storage selector. */
    }

  int readret = c6_phy_get(1, &observed);
  if (readret == 0 && c6_phy_equal(value, &observed))
    {
      return 0;
    }

  return ret != 0 ? ret : readret != 0 ? readret : -EPROTO;
}

static int c6_softap_phy_restore(FAR struct c6_softap_private_s *ap)
{
  int ret = c6_phy_program(&ap->phy.before_ap);
  if (ret == 0)
    {
      ap->phy_changed = false;
      ap->phy.ap = ap->phy.before_ap;
    }

  return ret;
}

static int c6_softap_phy_apply(FAR struct c6_softap_private_s *ap)
{
  struct c6_wifi_phy_config_s wanted =
    {
      C6_WIFI_PROTOCOL_BGN |
        (g_c6ap_he_preferred ? C6_WIFI_PROTOCOL_AX : 0),
      C6_WIFI_BW20
    };

  int ret = c6_phy_get(0, &ap->phy.before_sta);
  if (ret == 0)
    {
      ret = c6_phy_get(1, &ap->phy.before_ap);
    }

  if (ret != 0)
    {
      ap->phy.apply_error = ret;
      return ret;
    }

  ap->phy_changed = true; /* The C6 may execute even if the response is lost. */
  ret = c6_phy_program(&wanted);
  ap->phy.apply_error = ret;
  if (ret == 0)
    {
      ap->phy.ap = wanted;
      ap->phy.configured = true;
      return 0;
    }

  ap->phy.restore_error = c6_softap_phy_restore(ap);

  /* Preferred HE may degrade to the verified original config. A requested
   * legacy comparison must not silently continue with HE still configured.
   */

  return g_c6ap_he_preferred && ap->phy.restore_error == 0 ? 0 : ret;
}

static int c6_softap_phy_verify(FAR struct c6_softap_private_s *ap)
{
  struct c6_wifi_phy_config_s observed;
  int ret = c6_phy_get(1, &observed);
  if (ret == 0 && !c6_phy_equal(&observed, &ap->phy.ap))
    {
      ret = -EPROTO;
    }

  if (ret == 0)
    {
      ret = c6_phy_get(0, &observed);
      if (ret == 0 && !c6_phy_equal(&observed, &ap->phy.before_sta))
        {
          ret = -EPROTO;
        }
    }

  ap->phy.valid = ret == 0;
  ap->phy.query_error = ret;
  return ret;
}

int c6_softap_phy_prefer(bool enable)
{
  int ret = pthread_mutex_trylock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_c6ap_mode_error != 0 || c6_wifi_trial_pending() ||
      (g_c6ap != NULL &&
       (g_c6ap->state.active || g_c6ap->state.state_unknown)))
    {
      ret = -EBUSY;
    }
  else
    {
      g_c6ap_he_preferred = enable;
    }

  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6_softap_phy_read(FAR struct c6_wifi_phy_snapshot_s *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  int ret = pthread_mutex_trylock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_c6ap != NULL)
    {
      *out = g_c6ap->phy;
    }
  else
    {
      memset(out, 0, sizeof(*out));
    }

  out->preferred = g_c6ap_he_preferred;
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return 0;
}

int c6_softap_phy_refresh(void)
{
  struct c6_link_snapshot before;
  struct c6_link_snapshot after;
  FAR struct c6_softap_private_s *ap;
  int ret = pthread_mutex_trylock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  ap = g_c6ap;
  if (ap == NULL || !ap->state.active || c6_wifi_trial_pending())
    {
      ret = -EAGAIN;
      goto out;
    }

  ap->phy.clients_valid = false;
  ap->phy.sta_valid = false;
  ret = c6_softap_phy_verify(ap);
  if (ret == 0)
    {
      ret = esp_hosted_rpc_phy_clients(&ap->phy.clients,
                                      &ap->phy.he_clients);
      ap->phy.clients_valid = ret == 0;
    }

  if (ret == 0 && c6net_get_link_snapshot(&before) == 0 && before.associated)
    {
      ret = esp_hosted_rpc_phy_sta(&ap->phy.sta_phy);
      if (ret == 0 && c6net_get_link_snapshot(&after) == 0)
        {
          ap->phy.sta_valid = after.associated &&
                             before.generation == after.generation;
        }
    }

  ap->phy.query_error = ret;
out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}
