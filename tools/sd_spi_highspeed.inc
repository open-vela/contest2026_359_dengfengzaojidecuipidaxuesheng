/* Negotiation only; never changes voltage, bus width or card contents. */

static int mmcsd_highspeed(FAR struct mmcsd_slot_s *slot,
                          FAR const uint8_t *csd)
{
  static const struct mmcsd_cmdinfo_s command =
    {
      CMD6, MMCSD_CMDRESP_R1, 0xff
    };

  FAR struct spi_dev_s *spi = slot->spi;
  uint8_t status[64];
  uint32_t actual;
  int ret;

  if (!IS_SDV2(slot->type) || CONFIG_MMCSD_SPICLOCK <= 25000000)
    {
      return OK;
    }

  /* CSD class 10 declares SWITCH_FUNC support. */

  if ((csd[4] & 0x40) == 0)
    {
      return OK;
    }

  ret = mmcsd_sendcmd(slot, &command, 0x00fffff1);
  if (ret != MMCSD_SPIR1_OK)
    {
      return ret == MMCSD_SPIR1_ILLEGALCMD ? OK : -EIO;
    }

  if (mmcsd_recvblock(slot, status, sizeof(status)) != OK)
    {
      return -EIO;
    }

  if ((status[13] & 2) == 0 || status[17] > 1 ||
      (status[17] == 1 && (status[29] & 2) != 0))
    {
      return OK;
    }

  ret = mmcsd_sendcmd(slot, &command, 0x80fffff1);
  if (ret != MMCSD_SPIR1_OK ||
      mmcsd_recvblock(slot, status, sizeof(status)) != OK)
    {
      return -EIO;
    }

  if ((status[16] & 15) != 1 || status[17] > 1 ||
      (status[17] == 1 && (status[29] & 2) != 0))
    {
      return -EIO;
    }

  actual = SPI_SETFREQUENCY(spi, CONFIG_MMCSD_SPICLOCK > 50000000 ?
                                50000000 : CONFIG_MMCSD_SPICLOCK);
  if (actual == 0 || actual > 50000000)
    {
      return -EIO;
    }

  slot->spispeed = actual;
  syslog(LOG_INFO, "SD_SPI_HS selected=1 requested=%u actual=%lu\n",
         CONFIG_MMCSD_SPICLOCK, (unsigned long)actual);
  return OK;
}
