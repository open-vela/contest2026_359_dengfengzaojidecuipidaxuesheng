"""Bound C6 RX draining between completed polls; no deployment or reset.

The demonstrated issue is an unbounded high-priority drain loop, not attribution
of a historical SD or Wi-Fi timeout. Apply only to the supported c6net source.
"""


def _once(text, before, after):
    if text.count(before) != 1:
        raise ValueError("Unsupported c6net source: " + before[:100])
    return text.replace(before, after)


def patch_c6_rx_budget(text):
    text = _once(text, "#include <nuttx/mutex.h>",
                 "#include <nuttx/clock.h>\n#include <nuttx/mutex.h>")
    text = _once(text, "#define C6NET_TASK_STACK    4096", """#define C6NET_TASK_STACK    4096
#define C6NET_RX_BATCH_LIMIT 16
#define C6NET_RX_BUDGET_TICKS 1""")
    text = _once(text, """  while (c6net_is_initialized())
    {
      c6net_sync_carrier(&priv->dev);

      while (!esp_hosted_rpc_is_waiting() && esp_hosted_poll() > 0)
        {
        }

      usleep(C6NET_POLL_US);
    }""", """  while (c6net_is_initialized())
    {
      clock_t started;
      unsigned int batches = 0;

      c6net_sync_carrier(&priv->dev);
      started = clock_systime_ticks();

      while (!esp_hosted_rpc_is_waiting() && esp_hosted_poll() > 0)
        {
          /* A steady RX stream need not become empty.  Limit this drain
           * turn even when the clock does not advance between polls.
           * clock_t is unsigned, so elapsed ticks tolerate counter wrap.
           */

          if (++batches >= C6NET_RX_BATCH_LIMIT ||
              (clock_t)(clock_systime_ticks() - started) >=
              C6NET_RX_BUDGET_TICKS)
            {
              break;
            }
        }

      /* Each poll has completed its SDIO transfer and released its bus
       * and dispatch locks.  Block here so lower-priority tasks can run;
       * yielding at priority 110 alone does not give them that chance.
       * Keep the existing idle/error/RPC-wait polling interval.
       */

      usleep(C6NET_POLL_US);
    }""")
    return text
