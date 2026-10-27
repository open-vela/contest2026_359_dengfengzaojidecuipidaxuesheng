/****************************************************************************
 * apps/system/c6probe/esp_hosted.c
 *
 * Resident ESP-Hosted SDIO transport between the ESP32-P4 (host, NuttX)
 * and the ESP32-C6 companion radio (slave, unmodified esp_hosted 2.12
 * firmware).  The register map, flow control and framing follow the
 * official esp_hosted 2.12.12 host driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sdio.h>

#include "esp_gpio.h"
#include "esp_hosted.h"
#include "c6_rx_burst.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define C6_RESET_GPIO           54
#define C6_RESET_PULSE_MS       10
#define C6_BOOT_DELAY_MS        1500

#define C6_SDIO_FUNCTION        1
#define C6_SDIO_BLOCK_SIZE      512
#define C6_SDIO_OCR_3V3         0x00ff8000u
#define C6_SDIO_READY           (1u << 31)
#define C6_SDIO_READY_TRIES     100

/* Slave registers, accessed via CMD52 on Function 1.  Addresses are the
 * low ten bits of the SLCHOST offsets (SDIO_REG(x) in sdio_reg.h).
 */

#define C6_REG_TOKEN_RDATA      0x44
#define C6_REG_INT_RAW          0x50
#define C6_REG_INT_CLR          0xd4
#define C6_REG_INT_ENA          0xdc
#define C6_REG_PACKET_LEN       0x60
#define C6_REG_HOST_TO_SLAVE    0x8c

#define C6_HOST_TO_SLAVE_OPEN   0x01

#define C6_INT_NEW_PACKET       (1u << 23)

/* Both directions address the slave window so that the real (unpadded) frame
 * ends exactly at this address (sdio_reg.h: ESP_SLAVE_CMD53_END_ADDR).
 */

#define C6_CMD53_END_ADDR       0x1f800u

/* Accumulator widths (sdio_reg.h): RX bytes count modulo 1 MiB, TX buffers
 * modulo 4096.
 */

#define C6_RX_BYTE_MODULO       0x100000u
#define C6_RX_LEN_MASK          0x000fffffu
#define C6_TX_BUFFER_MODULO     0x1000u
#define C6_TX_BUFFER_MASK       0x0fffu
#define C6_TOKEN_SHIFT          16

/* A TX frame consumes ceil(total / slave buffer size) slave buffers. */

#define C6_TX_RETRY_DELAY_MS    50
#define C6_TX_RETRIES           10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_state_s
{
  /* The SDIO link is up and the data path is open.  Guarded by lock. */

  bool up;

  /* Set once the INIT event has been received and decoded. */

  struct esp_hosted_caps_s caps;

  mutex_t lock;

  /* Frame accumulators, mirroring sdio_rx_byte_count / sdio_tx_buf_count
   * in the official driver.  Guarded by lock.
   */

  uint32_t rx_bytes;
  uint32_t tx_buffers;

  /* Sequence numbers per interface, used in the frame header. */

  uint16_t seq_num[ESP_HOSTED_IF_MAX];

  /* Registered receive handler per interface. */

  esp_hosted_rx_cb_t rx_cb[ESP_HOSTED_IF_MAX];
  FAR void *rx_arg[ESP_HOSTED_IF_MAX];

  struct sdio_dev_s *sdio;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The transport is a singleton, matching the single C6 on the board. */

static struct esp_hosted_state_s g_hosted =
{
  .lock = NXMUTEX_INITIALIZER,
};

/* Bounded multi-block RX uses internal, cache-aligned DMA storage. TX and
 * short RX tails still use at most one block. The bus lock owns this buffer.
 */

static uint8_t g_dma_block[C6_RX_DMA_BLOCKS * C6_SDIO_BLOCK_SIZE]
               __attribute__((aligned(64)));

/* Guarded by the transport lock, including diagnostic mode changes. */

static unsigned int g_rx_block_limit = C6_RX_DMA_BLOCKS;

int esp_hosted_rx_block_limit(unsigned int blocks)
{
  int ret;

  if (blocks != 1 && blocks != C6_RX_DMA_BLOCKS)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0)
    {
      return ret;
    }

  g_rx_block_limit = blocks;
  nxmutex_unlock(&g_hosted.lock);
  return (int)blocks;
}

/* One full frame is reassembled here before dispatch. */

/* PACKET_LEN is an aggregate byte count, not the size of one Ethernet frame.
 * The C6's 20-entry TX queue can accumulate more than 16 KiB while the host
 * decodes video.  Keep a bounded batch buffer in the application heap; DMA
 * still uses the internal, cache-aligned bounce buffer above. */

#define C6_RX_BATCH_SIZE (64u * 1024u)
static FAR uint8_t *g_frame_buf;
static uint32_t g_rx_peak;
static uint32_t g_rx_large_batches;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hosted_sdio_read_reg32
 *
 * Description:
 *   Read a 32-bit slave register with four CMD52 byte reads.  This is the
 *   same access the official host uses for occasional control access.
 *
 ****************************************************************************/

static int hosted_sdio_read_reg32(FAR struct sdio_dev_s *sdio,
                                  uint32_t address, FAR uint32_t *value)
{
  uint32_t result = 0;
  uint8_t byte;
  int ret;
  int i;

  for (i = 0; i < 4; i++)
    {
      ret = sdio_io_rw_direct(sdio, false, C6_SDIO_FUNCTION,
                              (address & 0x3ff) + i, 0, &byte);
      if (ret < 0)
        {
          return ret;
        }

      result |= (uint32_t)byte << (i * 8);
    }

  *value = result;
  return OK;
}

/****************************************************************************
 * Name: hosted_sdio_write_reg8
 *
 * Description:
 *   Write one byte to a slave register with CMD52.
 *
 ****************************************************************************/

static int hosted_sdio_write_reg8(FAR struct sdio_dev_s *sdio,
                                  uint32_t address, uint8_t value)
{
  return sdio_io_rw_direct(sdio, true, C6_SDIO_FUNCTION,
                           address & 0x3ff, value, NULL);
}

/****************************************************************************
 * Name: hosted_reset_c6
 *
 * Description:
 *   Pulse the C6 CHIP_EN line with the official enable/reset/enable timing.
 *
 ****************************************************************************/

static int hosted_reset_c6(void)
{
  int ret;

  ret = esp_configgpio(C6_RESET_GPIO, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(C6_RESET_GPIO, true);
  usleep(C6_RESET_PULSE_MS * 1000);
  esp_gpiowrite(C6_RESET_GPIO, false);
  usleep(C6_RESET_PULSE_MS * 1000);
  esp_gpiowrite(C6_RESET_GPIO, true);
  usleep(C6_BOOT_DELAY_MS * 1000);
  return OK;
}

/****************************************************************************
 * Name: hosted_send_cmd
 *
 * Description:
 *   Send one SDIO command and wait for its response.  Only used during
 *   bring-up; the resident path uses CMD52/CMD53 helpers exclusively.
 *
 ****************************************************************************/

static int hosted_send_cmd(FAR struct sdio_dev_s *sdio, uint32_t cmd,
                           uint32_t arg)
{
  int ret;

  ret = SDIO_SENDCMD(sdio, cmd, arg);
  if (ret < 0)
    {
      return ret;
    }

  return SDIO_WAITRESPONSE(sdio, cmd);
}

/****************************************************************************
 * Name: hosted_enumerate
 *
 * Description:
 *   Run the SDIO enumeration sequence verified by c6probe: CMD0, CMD5 with
 *   3.3V OCR polling until ready, CMD3, CMD7, then 4-bit mode at 20 MHz
 *   with Function 1 enabled and a 512-byte block size.
 *
 ****************************************************************************/

static int hosted_enumerate(FAR struct sdio_dev_s *sdio, bool verbose)
{
  uint32_t r4;
  uint32_t response;
  uint32_t ocr;
  uint8_t byte;
  int attempt;
  int ret;

  /* The generic sdio_probe() owns this mutex lifecycle when it performs
   * enumeration.  Doing the sequence by hand requires the same setup.
   */

  nxmutex_init(&sdio->mutex);

  SDIO_CLOCK(sdio, CLOCK_IDMODE);

  ret = hosted_send_cmd(sdio, MMCSD_CMD0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CMD0: %d\n", ret);
      return ret;
    }

  usleep(10000);
  ret = hosted_send_cmd(sdio, SDIO_CMD5, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CMD5(0): %d\n", ret);
      return ret;
    }

  ret = SDIO_RECVR4(sdio, SDIO_CMD5, &r4);
  if (ret < 0)
    {
      syslog(LOG_ERR, "R4: %d\n", ret);
      return ret;
    }

  ocr = r4 & C6_SDIO_OCR_3V3;
  if (ocr == 0)
    {
      syslog(LOG_ERR, "no 3.3V OCR window\n");
      return -EIO;
    }

  response = 0;
  for (attempt = 1; attempt <= C6_SDIO_READY_TRIES; attempt++)
    {
      ret = hosted_send_cmd(sdio, SDIO_CMD5, ocr);
      if (ret == OK)
        {
          ret = SDIO_RECVR4(sdio, SDIO_CMD5, &response);
        }

      if (ret < 0)
        {
          syslog(LOG_ERR, "CMD5(OCR) attempt %d: %d\n", attempt, ret);
          return ret;
        }

      if ((response & C6_SDIO_READY) != 0)
        {
          break;
        }

      usleep(10000);
    }

  if ((response & C6_SDIO_READY) == 0)
    {
      syslog(LOG_ERR, "C6 never became ready\n");
      return -ETIMEDOUT;
    }

  ret = hosted_send_cmd(sdio, SD_CMD3, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CMD3: %d\n", ret);
      return ret;
    }

  ret = SDIO_RECVR6(sdio, SD_CMD3, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "R6: %d\n", ret);
      return ret;
    }

  ret = hosted_send_cmd(sdio, MMCSD_CMD7S, response & 0xffff0000);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CMD7: %d\n", ret);
      return ret;
    }

  ret = SDIO_RECVR1(sdio, MMCSD_CMD7S, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "CMD7 R1: %d\n", ret);
      return ret;
    }

  ret = sdio_set_wide_bus(sdio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "wide bus: %d\n", ret);
      return ret;
    }

  SDIO_CLOCK(sdio, CLOCK_SD_TRANSFER_4BIT);

  /* Clear any stale Function 1 enable before programming block size, as
   * the C6 latches its DMA windows on these writes.
   */

  ret = sdio_set_blocksize(sdio, C6_SDIO_FUNCTION, C6_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "block size: %d\n", ret);
      return ret;
    }

  ret = sdio_enable_function(sdio, C6_SDIO_FUNCTION);
  if (ret < 0)
    {
      syslog(LOG_ERR, "enable function: %d\n", ret);
      return ret;
    }

  /* Confirm the function actually became ready. */

  ret = sdio_io_rw_direct(sdio, false, 0, SDIO_CCCR_IORDY, 0, &byte);
  if (ret < 0 || (byte & (1u << C6_SDIO_FUNCTION)) == 0)
    {
      syslog(LOG_ERR, "Function 1 not ready: %d IORDY=%02x\n", ret, byte);
      return ret < 0 ? ret : -EIO;
    }

  ret = SDIO_ATTACH(sdio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "interrupt attach: %d\n", ret);
      return ret;
    }

  if (verbose)
    {
      printf("c6: SDIO enumerated, Function 1 ready, 4-bit/20MHz\n");
    }

  return OK;
}

/****************************************************************************
 * Name: hosted_send_host_config
 *
 * Description:
 *   Reply to the slave's INIT event with the host configuration.  The C6
 *   stays silent on the control path until it receives this, so the RPC
 *   endpoint never answers without it.  Mirrors send_slave_config() in
 *   transport_drv.c: an ESP_PRIV_EVENT_INIT event carrying five 1-byte TLVs,
 *   sent on ESP_PRIV_IF.
 *
 ****************************************************************************/

static int hosted_send_host_config(void)
{
  uint8_t event[2 + 5 * 3];
  uint8_t *pos = &event[2];
  uint8_t len = 0;

  /* Host capabilities: this port drives Wi-Fi and BT over SDIO only, with no
   * extra features to advertise.
   */

  *pos++ = ESP_HOSTED_HOST_CAPABILITIES;
  *pos++ = 1;
  *pos++ = 0;
  len += 3;

  /* Echo back the chip id the slave reported. */

  *pos++ = ESP_HOSTED_RCVD_CHIP_ID;
  *pos++ = 1;
  *pos++ = g_hosted.caps.chip_id;
  len += 3;

  /* No raw throughput test, no Wi-Fi TX throttling. */

  *pos++ = ESP_HOSTED_SLV_CONFIG_TEST_RAW_TP;
  *pos++ = 1;
  *pos++ = 0;
  len += 3;

  *pos++ = ESP_HOSTED_SLV_CONFIG_THROTTLE_HIGH;
  *pos++ = 1;
  *pos++ = 0;
  len += 3;

  *pos++ = ESP_HOSTED_SLV_CONFIG_THROTTLE_LOW;
  *pos++ = 1;
  *pos++ = 0;
  len += 3;

  event[0] = ESP_HOSTED_PRIV_EVENT_INIT;
  event[1] = len;

  /* Payload length covers the event type and length bytes as well. */

  return esp_hosted_send(ESP_HOSTED_IF_PRIV, 0, event, len + 2);
}

/****************************************************************************
 * Name: hosted_parse_init
 *
 * Description:
 *   Decode the INIT event payload (a sequence of TLVs) into the capability
 *   record.
 *
 ****************************************************************************/

static void hosted_parse_init(FAR const uint8_t *payload, uint16_t len)
{
  FAR const uint8_t *p     = payload;
  FAR const uint8_t *end   = payload + len;

  while (p + 2 <= end)
    {
      uint8_t tag   = p[0];
      uint8_t tlen  = p[1];
      FAR const uint8_t *value = p + 2;

      if (value + tlen > end)
        {
          break;
        }

      switch (tag)
        {
          case ESP_HOSTED_PRIV_FIRMWARE_CHIP_ID:
            if (tlen >= 1)
              {
                g_hosted.caps.chip_id = value[0];
              }
            break;

          case ESP_HOSTED_PRIV_CAPABILITY:
            if (tlen >= 1)
              {
                g_hosted.caps.capability = value[0];
              }
            break;

          case ESP_HOSTED_PRIV_TEST_RAW_TP:
            if (tlen >= 1)
              {
                g_hosted.caps.raw_tp = value[0];
              }
            break;

          case ESP_HOSTED_PRIV_RX_Q_SIZE:
            if (tlen >= 1)
              {
                g_hosted.caps.rx_q_size = value[0];
              }
            break;

          case ESP_HOSTED_PRIV_TX_Q_SIZE:
            if (tlen >= 1)
              {
                g_hosted.caps.tx_q_size = value[0];
              }
            break;

          default:
            break;
        }

      p = value + tlen;
    }

  g_hosted.caps.valid = true;
}

/****************************************************************************
 * Name: hosted_do_read_block
 *
 * Description:
 *   Read one to C6_RX_DMA_BLOCKS whole blocks into the DMA bounce buffer.
 *   Uses the multi-block mode supported by the fixed official
 *   host (H_SDIO_RX_BLOCK_ONLY_XFER) and the P4 IDMAC cache handling.
 *
 ****************************************************************************/

static int hosted_do_read_block(FAR struct sdio_dev_s *sdio,
                                uint32_t address, unsigned int blocks)
{
  sdio_eventset_t event;
  uint32_t arg;
  uint32_t response;
  int ret;

  /* Whole 512-byte blocks only. Never round a short tail into a block. */

  if (blocks == 0 || blocks > C6_RX_DMA_BLOCKS)
    {
      return -EINVAL;
    }

  arg = blocks |
        ((address & 0x1ffff) << 9) |
        (1u << 26) |
        (1u << 27) |
        ((uint32_t)C6_SDIO_FUNCTION << 28);

  SDIO_BLOCKSETUP(sdio, C6_SDIO_BLOCK_SIZE, blocks);
  SDIO_WAITENABLE(sdio,
                  SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR,
                  1000);

  ret = SDIO_DMARECVSETUP(sdio, g_dma_block,
                          blocks * C6_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      SDIO_CANCEL(sdio);
      return ret;
    }

  ret = SDIO_SENDCMD(sdio, SDIO_CMD53RD, arg);
  if (ret < 0)
    {
      SDIO_CANCEL(sdio);
      return ret;
    }

  event = SDIO_EVENTWAIT(sdio);
  ret = SDIO_RECVR5(sdio, SDIO_CMD53RD, &response);
  if (ret < 0)
    {
      return ret;
    }

  if ((event & (SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR)) != 0)
    {
      return (event & SDIOWAIT_TIMEOUT) != 0 ? -ETIMEDOUT : -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: hosted_do_read_tail
 *
 * Description:
 *   Read the final, non-block-aligned portion of an ESP-Hosted frame.  The
 *   slave pads this transfer to a 4-byte boundary; only the real bytes are
 *   copied by the caller.  This is the same split used by the reference
 *   ESP-Hosted SDIO driver.
 ****************************************************************************/

static int hosted_do_read_tail(FAR struct sdio_dev_s *sdio,
                               uint32_t address, uint32_t length)
{
  uint32_t transfer_len;

  transfer_len = (length + 3u) & ~3u;
  memset(g_dma_block, 0, C6_SDIO_BLOCK_SIZE);

  return sdio_io_rw_extended(sdio, false, C6_SDIO_FUNCTION, address, true,
                             g_dma_block, transfer_len, 0);
}

/****************************************************************************
 * Name: hosted_do_write_block
 *
 * Description:
 *   Perform one CMD53 byte-mode write from the DMA bounce buffer to the
 *   slave receive window.  The transfer length is the four-byte-aligned
 *   frame length, not a full SDIO block.
 *
 ****************************************************************************/

static int hosted_do_write_block(FAR struct sdio_dev_s *sdio,
                                 uint32_t address, uint32_t transfer_len)
{
  /* Use NuttX's standard CMD53 byte-mode path for short ESP-Hosted frames.
   * Passing nblocks=0 selects byte mode while preserving the real transfer
   * length and address.  The helper also performs the controller-specific
   * DMA setup, event wait, R5 handling, and abort cleanup.
   */

  return sdio_io_rw_extended(sdio, true, C6_SDIO_FUNCTION, address, true,
                             g_dma_block, transfer_len, 0);
}

/****************************************************************************
 * Name: hosted_pending_rx_bytes
 *
 * Description:
 *   Return the number of unread bytes the slave is holding, derived from
 *   the PACKET_LEN accumulator with wraparound handling.  Detects the all-
 *   ones bus fault signature from the official driver.
 *
 ****************************************************************************/

static int hosted_pending_rx_bytes(FAR uint32_t *pending)
{
  uint32_t regval;
  uint32_t len;
  int ret;

  ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_PACKET_LEN, &regval);
  if (ret < 0)
    {
      return ret;
    }

  if (regval == UINT32_MAX)
    {
      return -ENODEV;
    }

  len = regval & C6_RX_LEN_MASK;

  if (len >= g_hosted.rx_bytes)
    {
      len = (len + C6_RX_BYTE_MODULO - g_hosted.rx_bytes) % C6_RX_BYTE_MODULO;
    }
  else
    {
      len = (C6_RX_BYTE_MODULO - g_hosted.rx_bytes) + len;
    }

  *pending = len;
  return OK;
}

/****************************************************************************
 * Name: hosted_tx_buffers_free
 *
 * Description:
 *   Return the number of free slave receive buffers, derived from the token
 *   accumulator.  Also validates that at least `needed` are free.
 *
 ****************************************************************************/

static int hosted_tx_buffers_free(FAR uint32_t *free_count)
{
  uint32_t regval;
  uint32_t count;
  int ret;

  ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_TOKEN_RDATA, &regval);
  if (ret < 0)
    {
      return ret;
    }

  count = ((regval >> C6_TOKEN_SHIFT) & C6_TX_BUFFER_MASK);
  count = (count + C6_TX_BUFFER_MODULO - g_hosted.tx_buffers)
          % C6_TX_BUFFER_MODULO;

  *free_count = count;
  return OK;
}

/****************************************************************************
 * Name: hosted_dispatch
 *
 * Description:
 *   Validate a completed frame buffer and hand its payload to the
 *   registered interface callback.
 *
 ****************************************************************************/

static void hosted_dispatch(FAR uint8_t *frame, uint32_t framelen)
{
  FAR struct esp_hosted_header_s *hdr;
  esp_hosted_rx_cb_t cb;
  uint16_t payload_len;
  uint16_t payload_off;
  uint8_t if_type;

  if (framelen < ESP_HOSTED_HEADER_SIZE)
    {
      return;
    }

  hdr = (FAR struct esp_hosted_header_s *)frame;
  if_type = hdr->if_type & 0x0f;

  payload_len = hdr->len;
  payload_off = hdr->offset;

  if (payload_off < ESP_HOSTED_HEADER_SIZE ||
      payload_off > framelen ||
      (uint32_t)payload_off + payload_len > framelen)
    {
      syslog(LOG_ERR, "bad frame: if=%u len=%u off=%u total=%" PRIu32 "\n",
            if_type, payload_len, payload_off, framelen);
      return;
    }

  if (if_type == ESP_HOSTED_IF_PRIV &&
      hdr->pkt_type == ESP_HOSTED_PACKET_TYPE_EVENT &&
      payload_len >= 2 &&
      frame[payload_off] == ESP_HOSTED_PRIV_EVENT_INIT)
    {
      int ret;

      hosted_parse_init(frame + payload_off + 2, payload_len - 2);

      /* The slave will not answer on the control path until the host has
       * echoed its own INIT event back.  hosted_dispatch() runs outside the
       * bus lock, so sending from here is safe.
       */

      ret = hosted_send_host_config();
      if (ret < 0)
        {
          syslog(LOG_ERR, "host config reply failed: %d\n", ret);
        }

      return;
    }

  if (if_type >= ESP_HOSTED_IF_MAX) return;

  cb = g_hosted.rx_cb[if_type];
  if (cb != NULL)
    {
      cb(g_hosted.rx_arg[if_type], if_type, frame + payload_off,
         payload_len, hdr->pkt_type);
    }
}

/****************************************************************************
 * Name: hosted_dispatch_stream
 *
 * Description:
 *   PACKET_LEN describes a byte stream and may contain several consecutive
 *   ESP-Hosted frames.  Parse and dispatch every complete frame in the
 *   stream, matching the reference driver's streaming RX path.
 ****************************************************************************/

static void hosted_dispatch_stream(FAR uint8_t *stream, uint32_t streamlen)
{
  uint32_t offset = 0;

  while (streamlen - offset >= ESP_HOSTED_HEADER_SIZE)
    {
      FAR struct esp_hosted_header_s *hdr;
      uint32_t packet_len;

      hdr = (FAR struct esp_hosted_header_s *)(stream + offset);
      packet_len = (uint32_t)hdr->offset + (uint32_t)hdr->len;
      if (hdr->offset < ESP_HOSTED_HEADER_SIZE ||
          packet_len > streamlen - offset)
        {
          syslog(LOG_ERR, "bad stream frame: off=%u len=%u rem=%" PRIu32 "\n",
                 hdr->offset, hdr->len, streamlen - offset);
          return;
        }

      hosted_dispatch(stream + offset, packet_len);
      offset += packet_len;
    }

  if (offset != streamlen)
    {
      syslog(LOG_ERR, "trailing stream bytes: %" PRIu32 "\n",
             streamlen - offset);
    }
}

/****************************************************************************
 * Name: hosted_read_frame
 *
 * Description:
 *   Read one complete frame, block by block, into g_frame_buf.  The number
 *   of bytes to fetch was derived from PACKET_LEN before the read, and the
 *   read is padded up to a whole block as the official host does.
 *
 ****************************************************************************/

static int hosted_read_frame(FAR uint32_t *framelen)
{
  uint32_t pending;
  uint32_t data_left;
  uint32_t offset;
  uint32_t address;
  int ret;

  ret = hosted_pending_rx_bytes(&pending);
  if (ret < 0)
    {
      return ret;
    }

  if (pending == 0)
    {
      *framelen = 0;
      return OK;
    }

  if (pending > g_rx_peak)
    {
      g_rx_peak = pending;
    }

  if (pending > C6_RX_BATCH_SIZE)
    {
      syslog(LOG_ERR, "frame too large: %" PRIu32 "\n", pending);
      return -EMSGSIZE;
    }

  if (pending > ESP_HOSTED_MAX_FRAME)
    {
      g_rx_large_batches++;
    }

  /* Read exactly as the reference host does: full 512-byte blocks use
   * block-mode CMD53, while the final short portion uses byte mode.  A
   * block-mode read for a short frame can consume the next frame's window
   * and corrupt the transport on larger TCP/TLS responses.
   */

  data_left = pending;
  offset = 0;

  while (data_left >= C6_SDIO_BLOCK_SIZE)
    {
      unsigned int blocks = data_left / C6_SDIO_BLOCK_SIZE;
      uint32_t bytes;

      if (blocks > g_rx_block_limit)
        {
          blocks = g_rx_block_limit;
        }

      bytes = blocks * C6_SDIO_BLOCK_SIZE;
      address = C6_CMD53_END_ADDR - data_left;
      ret = hosted_do_read_block(g_hosted.sdio, address, blocks);
      if (ret < 0)
        {
          syslog(LOG_ERR, "block read at %" PRIx32 ": %d\n",
                 address, ret);
          return ret;
        }

      memcpy(g_frame_buf + offset, g_dma_block, bytes);
      offset += bytes;
      data_left -= bytes;
    }

  if (data_left != 0)
    {
      address = C6_CMD53_END_ADDR - data_left;
      ret = hosted_do_read_tail(g_hosted.sdio, address, data_left);
      if (ret < 0)
        {
          syslog(LOG_ERR, "tail read at %" PRIx32 ": %d\n", address, ret);
          return ret;
        }

      memcpy(g_frame_buf + offset, g_dma_block, data_left);
    }

  g_hosted.rx_bytes = (g_hosted.rx_bytes + pending) % C6_RX_BYTE_MODULO;
  *framelen = pending;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_initialize
 ****************************************************************************/

int esp_hosted_initialize(bool verbose)
{
  FAR struct sdio_dev_s *sdio;
  uint32_t regval;
  int ret;
  bool allocated = false;

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_hosted.up)
    {
      nxmutex_unlock(&g_hosted.lock);
      return OK;
    }

  if (g_frame_buf == NULL)
    {
      g_frame_buf = malloc(C6_RX_BATCH_SIZE);
      if (g_frame_buf == NULL)
        {
          ret = -ENOMEM;
          goto errout;
        }

      allocated = true;
    }

  if (verbose)
    {
      printf("c6: reset C6 on GPIO%d\n", C6_RESET_GPIO);
    }

  ret = hosted_reset_c6();
  if (ret < 0)
    {
      syslog(LOG_ERR, "C6 reset: %d\n", ret);
      goto errout;
    }

  /* ESP32-P4 currently has no public architecture header for this standard
   * NuttX SDIO entry point.
   */

  extern FAR struct sdio_dev_s *sdio_initialize(int slotno);

  sdio = sdio_initialize(1);
  if (sdio == NULL)
    {
      syslog(LOG_ERR, "sdio_initialize failed\n");
      ret = -EIO;
      goto errout;
    }

  g_hosted.sdio = sdio;

  ret = hosted_enumerate(sdio, verbose);
  if (ret < 0)
    {
      goto errout;
    }

  /* Open the data path.  Mirrors sdio_drv.c: ESP_OPEN_DATA_PATH to
   * HOST_TO_SLAVE_INTR, then wait for the INIT event.
   */

  ret = hosted_sdio_write_reg8(g_hosted.sdio, C6_REG_HOST_TO_SLAVE,
                               C6_HOST_TO_SLAVE_OPEN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "open data path: %d\n", ret);
      goto errout;
    }

  usleep(100000);

  /* Enable the new-packet interrupt so the DAT1 line is driven; polling
   * still works, this just keeps the line state consistent with the
   * official host.
   */

  ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_INT_ENA, &regval);
  if (ret == OK)
    {
      hosted_sdio_write_reg8(g_hosted.sdio, C6_REG_INT_ENA,
                             (uint8_t)((regval | C6_INT_NEW_PACKET) >> 24));
    }

  /* Snapshot both accumulators at open time.  The slave counters start
   * from zero after reset, so the host state starts at zero as well; the
   * first pending/free computation is relative to this instant.  The
   * already-queued INIT event stays pending until the first poll.
   */

  g_hosted.rx_bytes   = 0;
  g_hosted.tx_buffers = 0;

  g_hosted.up = true;

  if (verbose)
    {
      printf("c6: ESP-Hosted data path open\n");
    }

  nxmutex_unlock(&g_hosted.lock);
  return OK;

errout:
  g_hosted.up = false;
  if (allocated)
    {
      free(g_frame_buf);
      g_frame_buf = NULL;
    }
  nxmutex_unlock(&g_hosted.lock);
  return ret;
}

/****************************************************************************
 * Name: esp_hosted_register
 ****************************************************************************/

/* Lock order: RX ownership before bus. Callbacks may send, but must
 * not register callbacks or issue synchronous RPCs from dispatch context.
 */
static pthread_mutex_t g_rx_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL
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

int esp_hosted_register(uint8_t if_type, esp_hosted_rx_cb_t cb,
                        FAR void *arg)
{
  if (if_type >= ESP_HOSTED_IF_MAX)
    {
      return -EINVAL;
    }

  int ret = pthread_mutex_lock(&g_rx_dispatch_lock);
  if (ret != 0) return -ret;
#ifdef CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL
  if (if_type == ESP_HOSTED_IF_HCI && g_hci_claimed)
    {
      pthread_mutex_unlock(&g_rx_dispatch_lock);
      return -EBUSY;
    }
#endif
  g_hosted.rx_cb[if_type]  = cb;
  g_hosted.rx_arg[if_type] = arg;
  pthread_mutex_unlock(&g_rx_dispatch_lock);
  return OK;
}

/****************************************************************************
 * Name: esp_hosted_poll
 ****************************************************************************/

int esp_hosted_poll(void)
{
  uint32_t framelen = 0;
  int ret = pthread_mutex_trylock(&g_rx_dispatch_lock);
  if (ret == EBUSY) return 0;
  if (ret != 0) return -ret;

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0) goto out;
  if (!g_hosted.up)
    {
      ret = -ENOTCONN;
      nxmutex_unlock(&g_hosted.lock);
      goto out;
    }
  ret = hosted_read_frame(&framelen);
  nxmutex_unlock(&g_hosted.lock);
  if (ret < 0) goto out;
  if (framelen == 0)
    {
      ret = 0;
      goto out;
    }

  /* Keep frame ownership while allowing callbacks to acquire the bus. */
  hosted_dispatch_stream(g_frame_buf, framelen);
  ret = 1;
out:
  pthread_mutex_unlock(&g_rx_dispatch_lock);
  return ret;
}

/****************************************************************************
 * Name: esp_hosted_send
 ****************************************************************************/

int esp_hosted_send(uint8_t if_type, uint8_t if_num,
                    FAR const uint8_t *payload, uint16_t len)
{
  FAR struct esp_hosted_header_s *hdr;
  uint32_t framelen;
  uint32_t real_framelen;
  uint32_t total_blocks;
  uint32_t buffers_needed;
  uint32_t free_count;
  uint32_t address;
  uint32_t chunk;
  uint32_t offset;
  int attempt;
  int ret;

  if (!g_hosted.up)
    {
      return -ENOTCONN;
    }

  if (if_type >= ESP_HOSTED_IF_MAX)
    {
      return -EINVAL;
    }

  if (len == 0 || len > ESP_HOSTED_MAX_FRAME - ESP_HOSTED_HEADER_SIZE)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0)
    {
      return ret;
    }

  framelen = ESP_HOSTED_HEADER_SIZE + len;

  /* HCI frames arrive as complete H4 packets; the type byte travels in the
   * header's last field, exactly as the official host does.
   */

  if (if_type == ESP_HOSTED_IF_HCI && len >= 1)
    {
      len -= 1;
      framelen -= 1;
    }

  /* H4's type byte lives in the Hosted header, so exclude it from the
   * SDIO window length as well as from the payload length.
   */

  real_framelen = framelen;

  /* Wait until the slave has buffers for a frame of this size. */

  buffers_needed = (framelen + ESP_HOSTED_RX_BUFFER_SIZE - 1)
                   / ESP_HOSTED_RX_BUFFER_SIZE;

  for (attempt = 0; attempt < C6_TX_RETRIES; attempt++)
    {
      ret = hosted_tx_buffers_free(&free_count);
      if (ret < 0)
        {
          nxmutex_unlock(&g_hosted.lock);
          return ret;
        }

      if (free_count >= buffers_needed)
        {
          break;
        }

      nxmutex_unlock(&g_hosted.lock);
      usleep(C6_TX_RETRY_DELAY_MS * 1000);
      ret = nxmutex_lock(&g_hosted.lock);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (free_count < buffers_needed)
    {
      nxmutex_unlock(&g_hosted.lock);
      return -EAGAIN;
    }

  /* The slave reads the frame starting at ESP_SLAVE_CMD53_END_ADDR minus the
   * transfer length, so that address must be where the header lands.
   *
   * Round the length up to a multiple of four first, exactly as the official
   * host does in hosted_sdio_write_block() via H_SDIO_TX_LEN_TO_TRANSFER().
   * Without it an odd-sized frame produces an unaligned start address and the
   * slave's DMA never picks the frame up - it stops answering and the SDIO
   * link wedges until the C6 is reset.  The extra padding bytes are ignored
   * because the header carries the real length.
   */

  framelen = (framelen + 3) & ~3u;

  total_blocks = (framelen + C6_SDIO_BLOCK_SIZE - 1)
                 / C6_SDIO_BLOCK_SIZE;
  /* The slave's window is anchored by the real frame length.  The transfer
   * may be padded for alignment, but the header address must remain
   * END_ADDR - real_framelen, matching the official host driver.
   */
  address = C6_CMD53_END_ADDR - real_framelen;

  /* len still holds the real payload size; only the transfer is padded, so
   * copy loops must bound themselves by len and leave the padding zeroed.
   */

  /* Sequence numbers are per-interface and assigned under the lock. */

  g_hosted.seq_num[if_type]++;

  /* Build and write one block at a time: the bounce buffer holds a single
   * block, so it must be flushed to the slave before being reused.
   */

  for (chunk = 0; chunk < total_blocks; chunk++)
    {
      offset = chunk * C6_SDIO_BLOCK_SIZE;
      memset(g_dma_block, 0, C6_SDIO_BLOCK_SIZE);

      if (chunk == 0)
        {
          uint32_t copy = len;

          if (copy > C6_SDIO_BLOCK_SIZE - ESP_HOSTED_HEADER_SIZE)
            {
              copy = C6_SDIO_BLOCK_SIZE - ESP_HOSTED_HEADER_SIZE;
            }

          hdr = (FAR struct esp_hosted_header_s *)g_dma_block;
          hdr->if_type  = if_type & 0x0f;
          hdr->if_num   = if_num & 0x0f;
          hdr->flags    = 0;
          hdr->len      = len;
          hdr->offset   = ESP_HOSTED_HEADER_SIZE;
          hdr->checksum = 0;
          hdr->seq_num  = g_hosted.seq_num[if_type];

          if (if_type == ESP_HOSTED_IF_HCI)
            {
              /* The caller's first byte is the H4 packet type; it travels in
               * the header and the rest becomes the payload.
               */

              hdr->pkt_type = payload[0];
              memcpy(g_dma_block + ESP_HOSTED_HEADER_SIZE,
                     payload + 1, copy);
            }
          else
            {
              memcpy(g_dma_block + ESP_HOSTED_HEADER_SIZE, payload, copy);
            }
        }
      else
        {
          /* Later blocks carry payload only.  Bound by the real payload size
           * so the 4-byte transfer padding never reads past the caller's
           * buffer; hci_skip accounts for the H4 type byte already consumed
           * into the header.
           */

          uint32_t hci_skip = (if_type == ESP_HOSTED_IF_HCI) ? 1 : 0;
          uint32_t sent = offset - ESP_HOSTED_HEADER_SIZE;
          uint32_t copy;

          if (sent >= len)
            {
              copy = 0;
            }
          else
            {
              copy = len - sent;
              if (copy > C6_SDIO_BLOCK_SIZE)
                {
                  copy = C6_SDIO_BLOCK_SIZE;
                }
            }

          if (copy > 0)
            {
              memcpy(g_dma_block, payload + hci_skip + sent, copy);
            }
        }

      uint32_t transfer_len = C6_SDIO_BLOCK_SIZE;

      if (chunk == total_blocks - 1)
        {
          transfer_len = framelen - offset;
        }

      ret = hosted_do_write_block(g_hosted.sdio, address + offset,
                                  transfer_len);
      if (ret < 0)
        {
          syslog(LOG_ERR, "block write at %08" PRIx32 ": %d\n",
                 address + offset, ret);
          nxmutex_unlock(&g_hosted.lock);
          return ret;
        }
    }

  g_hosted.tx_buffers = (g_hosted.tx_buffers + buffers_needed)
                        % C6_TX_BUFFER_MODULO;

  nxmutex_unlock(&g_hosted.lock);
  return OK;
}

/****************************************************************************
 * Name: esp_hosted_probe_alive
 ****************************************************************************/

int esp_hosted_probe_alive(FAR uint8_t *cccr, FAR uint8_t *iordy)
{
  int ret;

  if (g_hosted.sdio == NULL)
    {
      return -ENOTCONN;
    }

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0)
    {
      return ret;
    }

  /* CMD52 against Function 0 works whenever the card's SDIO core is alive,
   * independent of any ESP-Hosted state, so it separates "C6 crashed" from
   * "C6 alive but not answering RPCs".
   */

  ret = sdio_io_rw_direct(g_hosted.sdio, false, 0, SDIO_CCCR_REV, 0, cccr);
  if (ret >= 0)
    {
      ret = sdio_io_rw_direct(g_hosted.sdio, false, 0, SDIO_CCCR_IORDY, 0,
                              iordy);
    }

  nxmutex_unlock(&g_hosted.lock);
  return ret;
}

/****************************************************************************
 * Name: esp_hosted_get_caps
 ****************************************************************************/

FAR const struct esp_hosted_caps_s *esp_hosted_get_caps(void)
{
  return &g_hosted.caps;
}

/****************************************************************************
 * Name: esp_hosted_debug_counters
 ****************************************************************************/

int esp_hosted_debug_counters(FAR uint32_t *tokens, FAR uint32_t *pending,
                              FAR uint32_t *intr)
{
  uint32_t regval;
  int ret;

  if (!g_hosted.up)
    {
      return -ENOTCONN;
    }

  ret = nxmutex_lock(&g_hosted.lock);
  if (ret < 0)
    {
      return ret;
    }

  printf("c6: RX peak=%lu large_batches=%lu capacity=%u\n",
         (unsigned long)g_rx_peak, (unsigned long)g_rx_large_batches,
         C6_RX_BATCH_SIZE);

  if (tokens != NULL)
    {
      ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_TOKEN_RDATA,
                                   &regval);
      if (ret < 0)
        {
          goto errout;
        }

      *tokens = regval;
    }

  if (pending != NULL)
    {
      ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_PACKET_LEN,
                                   &regval);
      if (ret < 0)
        {
          goto errout;
        }

      *pending = regval;
    }

  if (intr != NULL)
    {
      ret = hosted_sdio_read_reg32(g_hosted.sdio, C6_REG_INT_RAW, &regval);
      if (ret < 0)
        {
          goto errout;
        }

      *intr = regval;
    }

errout:
  nxmutex_unlock(&g_hosted.lock);
  return ret;
}
