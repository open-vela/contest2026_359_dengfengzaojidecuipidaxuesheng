/****************************************************************************
 * apps/system/c6probe/c6probe_main.c
 *
 * NSH command that brings up the resident ESP-Hosted transport to the
 * ESP32-C6 companion radio and reports the negotiated capabilities.
 * The transport itself lives in esp_hosted.c and stays up after this
 * command returns; running c6probe again is a no-op probe.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef CONFIG_NETUTILS_DHCPC
#  include <netutils/netlib.h>
#endif

static int c6probe_tcp(FAR const char *host, FAR const char *service)
{
  struct addrinfo hints;
  FAR struct addrinfo *res = NULL;
  struct timeval tv;
  char request[] = "GET / HTTP/1.0\r\nHost: test\r\n\r\n";
  unsigned char buffer[256];
  int fd;
  int ret;
  ssize_t n;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  ret = getaddrinfo(host, service, &hints, &res);
  if (ret != 0 || res == NULL)
    {
      printf("c6probe: tcp DNS failed host=%s ret=%d\n", host, ret);
      return -EHOSTUNREACH;
    }

  fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0)
    {
      ret = -errno;
      freeaddrinfo(res);
      return ret;
    }

  tv.tv_sec = 5;
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  ret = connect(fd, res->ai_addr, res->ai_addrlen);
  printf("c6probe: tcp connect host=%s port=%s ret=%d errno=%d\n",
         host, service, ret, ret < 0 ? errno : 0);
  if (ret == 0)
    {
      n = send(fd, request, sizeof(request) - 1, 0);
      printf("c6probe: tcp send bytes=%ld errno=%d\n",
             (long)n, n < 0 ? errno : 0);
      n = recv(fd, buffer, sizeof(buffer), 0);
      printf("c6probe: tcp recv bytes=%ld errno=%d\n",
             (long)n, n < 0 ? errno : 0);
      if (n > 0)
        {
          printf("c6probe: tcp recv head=%02x %02x %02x %02x\n",
                 buffer[0], buffer[1], buffer[2], buffer[3]);
        }
    }

  close(fd);
  freeaddrinfo(res);
  return ret;
}

#ifdef CONFIG_NETDB_DNSCLIENT
#  include <nuttx/net/dns.h>
#endif

#include "esp_hosted.h"
#include "c6net.h"
#include "c6_ota.h"
#include "c6_rx_burst.h"
#include "c6_rx_bench.inc"

#ifdef CONFIG_NETDB_DNSCLIENT
static int c6probe_dns_print(FAR void *arg, FAR struct sockaddr *addr,
                             socklen_t addrlen)
{
  char text[INET6_ADDRSTRLEN];
  FAR const void *src;

  UNUSED(arg);
  UNUSED(addrlen);

  if (addr->sa_family == AF_INET)
    {
      src = &((FAR struct sockaddr_in *)addr)->sin_addr;
    }
#ifdef CONFIG_NET_IPv6
  else if (addr->sa_family == AF_INET6)
    {
      src = &((FAR struct sockaddr_in6 *)addr)->sin6_addr;
    }
#endif
  else
    {
      return 0;
    }

  if (inet_ntop(addr->sa_family, src, text, sizeof(text)) != NULL)
    {
      printf("c6probe: DNS %s\n", text);
    }

  return 0;
}
#endif

int main(int argc, FAR char *argv[])
{
  FAR struct esp_hosted_caps_s const *caps;
  uint8_t mac[6];
  int polls;
  int ret;
  int i;

  if (argc > 1 && strcmp(argv[1], "rxbench") == 0)
    {
      return c6_rx_bench_command(argc, argv);
    }

  if (argc == 3 && strcmp(argv[1], "txwake") == 0)
    {
      if (strcmp(argv[2], "0") && strcmp(argv[2], "1")) return 1;
      ret = c6net_tx_wake(argv[2][0] == '1');
      printf("TXWAKE result=%d enabled=%c\n", ret, argv[2][0]);
      return ret < 0 ? 1 : 0;
    }

  if (argc == 3 && strcmp(argv[1], "rxblocks") == 0)
    {
      if (strcmp(argv[2], "1") && strcmp(argv[2], "4")) return 1;
      ret = esp_hosted_rx_block_limit((unsigned int)(argv[2][0] - '0'));
      printf("RXBLOCKS result=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  if (argc > 1 && strcmp(argv[1], "ota") == 0)
    {
      return c6_ota_command(argc, argv);
    }

  if (argc == 2 && strcmp(argv[1], "netstat") == 0)
    {
      for (unsigned index = 0; index < 2; index++)
        {
          struct c6net_stats_s stats;
          ret = c6net_stats_read(index != 0, &stats);
          if (ret < 0) return EXIT_FAILURE;
          printf("c6net if=%s notify=%" PRIu32 " polls=%" PRIu32
                 " tx_packets=%" PRIu32 " tx_bytes=%" PRIu32
                 " tx_errors=%" PRIu32 " tx_down=%" PRIu32
                 " send_max_us=%" PRIu32 " last_send_error=%d\n",
                 index ? "ap0" : C6NET_IFNAME, stats.tx_notifications,
                 stats.tx_polls, stats.tx_packets, stats.tx_bytes,
                 stats.tx_errors, stats.tx_down_drops, stats.tx_send_max_us,
                 stats.tx_last_error);
          printf("c6net if=%s rx_packets=%" PRIu32 " rx_bytes=%" PRIu32
                 " rx_invalid=%" PRIu32 " rx_down=%" PRIu32
                 " rx_unsupported=%" PRIu32 "\n",
                 index ? "ap0" : C6NET_IFNAME, stats.rx_packets, stats.rx_bytes,
                 stats.rx_invalid, stats.rx_down_drops, stats.rx_unsupported);
        }
      return EXIT_SUCCESS;
    }

  /* stat reports the low-level link state without touching it, so it can be
   * run straight after a failure to see whether the C6 is still alive.
   */

  if (argc > 1 && strcmp(argv[1], "stat") == 0)
    {
      uint32_t tok = 0;
      uint32_t pend = 0;
      uint32_t intr = 0;
      uint8_t cccr = 0;
      uint8_t iordy = 0;
      int aret;
      int cret;

      aret = esp_hosted_probe_alive(&cccr, &iordy);
      printf("c6probe: CMD52 ret=%d CCCR=0x%02x IORDY=0x%02x\n",
             aret, cccr, iordy);

      cret = esp_hosted_debug_counters(&tok, &pend, &intr);
      printf("c6probe: regs ret=%d TOKEN=%08" PRIx32 " PKTLEN=%08" PRIx32
             " INT=%08" PRIx32 "\n", cret, tok, pend, intr);

      return EXIT_SUCCESS;
    }

  if (argc > 1 && strcmp(argv[1], "net") == 0)
    {
      ret = c6net_initialize(argc > 2 ? argv[2] : "",
                             argc > 3 ? argv[3] : "");
      printf("c6probe: network init ret=%d\n", ret);
      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  if (argc > 1 && strcmp(argv[1], "tcp") == 0)
    {
      ret = c6probe_tcp(argc > 2 ? argv[2] : "uapis.cn",
                        argc > 3 ? argv[3] : "443");
      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

#ifdef CONFIG_NETUTILS_DHCPC
  if (argc > 1 && strcmp(argv[1], "dhcp") == 0)
    {
      ret = netlib_obtain_ipv4addr(argc > 2 ? argv[2] : C6NET_IFNAME);
      printf("c6probe: DHCP ret=%d errno=%d\n", ret,
             ret < 0 ? errno : 0);
      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
#endif

#ifdef CONFIG_NETDB_DNSCLIENT
  if (argc > 1 && strcmp(argv[1], "dns") == 0)
    {
      if (argc > 2)
        {
          struct in_addr addr;

          if (inet_pton(AF_INET, argv[2], &addr) != 1)
            {
              fprintf(stderr, "c6probe: invalid DNS address\n");
              return EXIT_FAILURE;
            }

          ret = netlib_set_ipv4dnsaddr(&addr);
          if (ret < 0)
            {
              fprintf(stderr, "c6probe: set DNS failed: %d\n", ret);
              return EXIT_FAILURE;
            }
        }

      ret = dns_foreach_nameserver(c6probe_dns_print, NULL);
      printf("c6probe: DNS entries=%d\n", ret);
      return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
#endif

  /* connect [<ssid> [<password>]] associates with an AP.  Credentials are
   * given on the command line so they are never compiled into the image.
   * "connect" with no SSID skips SetConfig and associates using whatever
   * configuration the C6 already has stored, which isolates SetConfig.
   */

  if (argc > 1 && strcmp(argv[1], "connect") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (esp_hosted_rpc_wifi_init() < 0 ||
      esp_hosted_rpc_wifi_set_mode(1) < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_start() < 0)
        {
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_wifi_connect(argc > 2 ? argv[2] : "",
                                        argc > 3 ? argv[3] : "");
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: connect failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* Association is asynchronous: keep polling so the C6 can push its
       * connect/disconnect events over the control path.
       */

      printf("c6probe: associating, watching events for 10s\n");
      for (polls = 0; polls < 200; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      return EXIT_SUCCESS;
    }

  /* The wifi subcommand runs the full Wi-Fi bring-up sequence and then
   * reports the STA MAC, mirroring what the reference host does.
   */

  if (argc > 1 && strcmp(argv[1], "wifi") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (esp_hosted_rpc_wifi_init() < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_set_mode(1 /* WIFI_MODE_STA */) < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_start() < 0)
        {
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_get_mac(0 /* STA */, mac);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: GetMacAddress RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("c6probe: STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      /* A scan needs no credentials and proves the radio is really up. */

      ret = esp_hosted_rpc_wifi_scan();
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: scan failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
    }

  /* The rpc subcommand only exercises the control path. */

  if (argc > 1 && strcmp(argv[1], "rpc") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: ESP-Hosted bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* The control path stays silent until the INIT event has been consumed
       * and the host configuration echoed back, so drain it first.
       */

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (!caps->valid)
        {
          fprintf(stderr, "c6probe: no INIT event; transport not ready\n");
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_get_mac(0 /* STA */, mac);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: GetMacAddress RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("c6probe: STA MAC ");
      for (i = 0; i < 6; i++)
        {
          printf("%02x%s", mac[i], i < 5 ? ":" : "");
        }

      printf("\n");
      return EXIT_SUCCESS;
    }

  /* Bring up the resident ESP-Hosted transport (idempotent) and consume
   * the INIT event queued at data-path open, which fills in the C6
   * capability record.
   */

  ret = esp_hosted_initialize(true);
  if (ret < 0)
    {
      fprintf(stderr, "c6probe: ESP-Hosted bring-up failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  caps = esp_hosted_get_caps();
  for (polls = 0; polls < 10 && !caps->valid; polls++)
    {
      ret = esp_hosted_poll();
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: transport poll failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      usleep(50000);
    }

  if (!caps->valid)
    {
      fprintf(stderr, "c6probe: no ESP-Hosted INIT event received\n");
      return EXIT_FAILURE;
    }

  printf("c6probe: C6 chip_id=%u capability=%02x rx_q=%u tx_q=%u\n",
         caps->chip_id, caps->capability, caps->rx_q_size, caps->tx_q_size);

  if ((caps->capability & ESP_HOSTED_CAP_WLAN_SDIO) != 0)
    {
      printf("c6probe: C6 supports WLAN over SDIO\n");
    }

  if ((caps->capability & ESP_HOSTED_CAP_BT_SDIO) != 0)
    {
      printf("c6probe: C6 supports BT (HCI) over SDIO\n");
    }

  if ((caps->capability & ESP_HOSTED_CAP_BLE_ONLY) != 0)
    {
      printf("c6probe: C6 is BLE-only (no BR/EDR)\n");
    }

  /* Drain any further pending traffic so the state after c6probe is
   * clean for the resident transport users.
   */

  for (polls = 0; polls < 20; polls++)
    {
      ret = esp_hosted_poll();
      if (ret <= 0)
        {
          break;
        }
    }

  printf("c6probe: OK ESP-Hosted transport is up\n");
  return EXIT_SUCCESS;
}
