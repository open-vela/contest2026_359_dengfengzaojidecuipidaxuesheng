"""Narrow patches for the pinned P4 storage stack; never deploy or reset."""


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('unsupported source: ' + before[:100])
    return text.replace(before, after)


def section(text, signature):
    start = text.index(signature)
    end = text.index('\n/****************************************************************************', start)
    return start, end, text[start:end]


def patch_sd(text):
    text = once(text, '''#if 0 /* Not used */
static const struct mmcsd_cmdinfo_s g_cmd10  =
{
  CMD10,  MMCSD_CMDRESP_R1, 0xff
};
#endif''', '''static const struct mmcsd_cmdinfo_s g_cmd10  =
{
  CMD10,  MMCSD_CMDRESP_R1, 0xff
};''')
    text = once(text, '  CMD12,  MMCSD_CMDRESP_R1, 0xff',
                '  CMD12,  MMCSD_CMDRESP_R1B, 0xff')
    text = once(text, '  uint8_t  csd[16];          /* Copy of card CSD */',
                '''  uint8_t  csd[16];          /* Copy of card CSD */
  uint8_t  cid[16];          /* Identity captured at initialization */
  bool     cidvalid;         /* Identity is safe to compare */
  bool     checkcid;         /* Revalidate identity after a transport error */
  bool     readstop;         /* CMD18 still needs a successful CMD12 */''')
    text = once(text, '''  ret = mmcsd_waitready(slot);
  if (ret != OK && cmd != &g_cmd0)
    {
      return ret;
    }''', '''  /* CMD12 interrupts an active read stream.  Clocking data while waiting
   * for idle can consume another block and must not prevent the stop.
   */

  if (cmd != &g_cmd12)
    {
      ret = mmcsd_waitready(slot);
      if (ret != OK && cmd != &g_cmd0)
        {
          return ret;
        }
    }''')

    # Keep the original deadlines. Add failure-only timing evidence so a card
    # response timeout can be distinguished from a scheduling gap on hardware.
    start, end, body = section(text, 'static int mmcsd_recvblock(')
    body = once(body, '  clock_t elapsed;\n', '''  clock_t elapsed;
  clock_t now;
  clock_t previous;
  clock_t maxgap = 0;
  uint32_t polls = 0;
''')
    body = once(body, '  start = START_TIME;\n', '  start = START_TIME;\n  previous = start;\n')
    body = once(body, '      elapsed = ELAPSED_TIME(start);\n', '''      now = clock_systime_ticks();
      elapsed = now - start;
      if (now - previous > maxgap)
        {
          maxgap = now - previous;
        }

      previous = now;
      polls++;
''')
    body = once(body, '  return ERROR;\n', '''  ferr("SDREAD token=%02x elapsed=%jd limit=%" PRIu32
       " polls=%" PRIu32 " maxgap=%jd\\n", token, (intmax_t)elapsed,
       slot->taccess, polls, (intmax_t)maxgap);
  return ERROR;
''')
    text = text[:start] + body + text[end:]

    helper = '''
/****************************************************************************
 * Name: mmcsd_read_recover
 *
 * Description:
 *   Finish a failed read stream and verify the original card before allowing
 *   another operation.  Caller holds both locks and selects the card.
 *   No reset or media initialization is performed.  EAGAIN means that no
 *   media change has been confirmed and the next operation may retry.
 ****************************************************************************/

static int mmcsd_read_recover(FAR struct mmcsd_slot_s *slot)
{
  uint8_t cid[16];
  uint32_t response;
  int ret;

  if (slot->state & (MMCSD_SLOTSTATUS_NOTREADY | MMCSD_SLOTSTATUS_NODISK))
    {
      return -ENODEV;
    }

  if (slot->readstop)
    {
      response = mmcsd_sendcmd(slot, &g_cmd12, 0);
      if (response != MMCSD_SPIR1_OK)
        {
          ferr("SDREAD stop response=%08" PRIx32 "\\n", response);
          return -EAGAIN;
        }

      slot->readstop = false;
    }

  if (!slot->checkcid)
    {
      return OK;
    }

  if (!slot->cidvalid)
    {
      return -ENODEV;
    }

  ret = mmcsd_getcid(slot, cid);
  if (ret < 0)
    {
      return -EAGAIN;
    }

  if (memcmp(cid, slot->cid, sizeof(cid)) != 0)
    {
      slot->state |= MMCSD_SLOTSTATUS_NOTREADY | MMCSD_SLOTSTATUS_MEDIACHGD;
      ferr("SDREAD media identity changed\\n");
      return -ENODEV;
    }

  slot->checkcid = false;
  return OK;
}
'''
    at = text.index('\n/****************************************************************************\n * Name: mmcsd_recvblock')
    text = text[:at] + helper + text[at:]

    start, end, body = section(text, 'static ssize_t mmcsd_read(FAR struct inode *inode, FAR unsigned char *buffer,')
    body = once(body, '  uint8_t response;\n', '  uint32_t response;\n')
    body = once(body, '  int i;\n', '  int i = 0;\n')
    at = body.index('retry:\n')
    body = body[:at] + '''retry:
  SPI_SELECT(spi, SPIDEV_MMCSD(0), true);
  ret = mmcsd_read_recover(slot);
  if (ret < 0)
    {
      goto errout;
    }

  response = mmcsd_sendcmd(slot, nsectors == 1 ? &g_cmd17 : &g_cmd18,
                           offset);
  if (response != MMCSD_SPIR1_OK)
    {
      /* A lost response does not prove that CMD18 was rejected. */

      slot->readstop = nsectors > 1;
      goto read_error;
    }

  slot->readstop = nsectors > 1;
  for (i = 0; i < nsectors; i++)
    {
      if (mmcsd_recvblock(slot, buffer, SECTORSIZE(slot)) != OK)
        {
          goto read_error;
        }

      buffer += SECTORSIZE(slot);
    }

  if (slot->readstop)
    {
      response = mmcsd_sendcmd(slot, &g_cmd12, 0);
      if (response != MMCSD_SPIR1_OK)
        {
          goto read_error;
        }

      slot->readstop = false;
    }

  SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
  mmcsd_unlock(slot);
  mmcsd_dumpbuffer("Read buffer", restore, nbytes);
  return nsectors;

read_error:
  slot->checkcid = true;
  ferr("SDREAD sector=%" PRIuOFF " count=%u index=%d retry=%d\\n",
       start_sector, nsectors, i, retry_count);
  ret = mmcsd_read_recover(slot);
  SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
  SPI_SEND(spi, 0xff);

  /* Retry the whole read once after a successful stop and matching CID.
   * Never retry an ambiguous write or reset a mounted card here.
   */

  if (ret == OK && retry_count++ == 0)
    {
      buffer = restore;
      goto retry;
    }

  mmcsd_unlock(slot);
  return ret == -ENODEV ? ret : -EIO;

errout:
  SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
  mmcsd_unlock(slot);
  return ret;
}
'''
    text = text[:start] + body + text[end:]

    # Writes must not start while an earlier failed read stream remains active.
    start, end, body = section(text, 'static ssize_t mmcsd_write(FAR struct inode *inode,')
    body = once(body, '''retry:
  SPI_SELECT(spi, SPIDEV_MMCSD(0), true);
''', '''retry:
  SPI_SELECT(spi, SPIDEV_MMCSD(0), true);
  ret = mmcsd_read_recover(slot);
  if (ret < 0)
    {
      SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
      mmcsd_unlock(slot);
      return ret;
    }
''')
    text = text[:start] + body + text[end:]

    start, end, body = section(text, 'static int mmcsd_geometry(FAR struct inode *inode,')
    body = once(body, '''  ret = mmcsd_getcsd(slot, csd);
''', '''  ret = mmcsd_read_recover(slot);
  if (ret == OK)
    {
      ret = mmcsd_getcsd(slot, csd);
    }

''')
    body = once(body, '''      mmcsd_unlock(slot);
      ferr("ERROR: mmcsd_getcsd returned %d\\n", ret);
      return ret;''', '''      slot->checkcid = true;
      mmcsd_unlock(slot);
      ferr("SDREAD geometry deferred: %d\\n", ret);
      return ret == -ENODEV ? ret : -EAGAIN;''')
    text = text[:start] + body + text[end:]

    start, end, body = section(text, 'static int mmcsd_mediainitialize(FAR struct mmcsd_slot_s *slot)\n{')
    body = once(body, '  slot->state |= MMCSD_SLOTSTATUS_NOTREADY;\n', '''  slot->state |= MMCSD_SLOTSTATUS_NOTREADY;
  slot->cidvalid = false;
  slot->checkcid = false;
  slot->readstop = false;
''')
    body = once(body, '  mmcsd_dmpcsd(csd, slot->type);\n', '''  result = mmcsd_getcid(slot, slot->cid);
  if (result != OK)
    {
      SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
      return -EIO;
    }

  slot->cidvalid = true;
  memcpy(slot->csd, csd, sizeof(slot->csd));
  mmcsd_dmpcsd(csd, slot->type);
''')
    return text[:start] + body + text[end:]


def patch_fat(text):
    return once(text, '''              int errcode = inode->u.i_bops->geometry(inode, &geo);
              if (errcode == OK''', '''              int errcode = inode->u.i_bops->geometry(inode, &geo);

              /* The block driver may defer a media check after a transient
               * transport failure.  Fail this operation, but allow a later
               * check.  Never resurrect a mount already invalidated by a
               * confirmed media change or other permanent error.
               */

              if (errcode == -EAGAIN)
                {
                  return errcode;
                }

              if (errcode == OK''')


def patch_spi(text):
    return once(text, '''      spi_hal_user_start(priv->ctx);
      spi_hal_enable_data_line(priv->ctx->hw,
                               (!priv->timing_param->half_duplex && \\
                                trans.rcv_buffer) || trans.send_buffer,
                               !!trans.rcv_buffer);
''', '''      spi_hal_enable_data_line(priv->ctx->hw,
                               (!priv->timing_param->half_duplex && \\
                                trans.rcv_buffer) || trans.send_buffer,
                               !!trans.rcv_buffer);
      spi_hal_user_start(priv->ctx);
''')


def patch_font(text):
    assert text.count('fopen(GLASS_SD_FONT_PATH,"rb")') == 2
    text = text.replace('fopen(GLASS_SD_FONT_PATH,"rb")', 'fopen(GLASS_SD_FONT_PATH,"rbe")')
    text = once(text, '      file=fopen(GLASS_SD_FONT_PATH,"rbe");error=file?0:-errno;break;',
                '''      file = fopen(GLASS_SD_FONT_PATH, "rbe");
      error = file ? 0 : -errno;
      break;''')
    text = once(text, '    FILE *f=fopen(GLASS_SD_FONT_PATH,"rbe");struct stat st;',
                '''    FILE *f = fopen(GLASS_SD_FONT_PATH, "rbe");
    struct stat st;
''')
    text = once(text, 'if(--sd_fonts->open_count==0){fclose(sd_fonts->file);sd_fonts->file=NULL;}',
                '''if (--sd_fonts->open_count == 0 && sd_fonts->file)
    {
      fclose(sd_fonts->file);
      sd_fonts->file = NULL;
    }''')
    return once(text, '  if(s->io_failed&&s->state==2){s->state=3;s->error=-EIO;return -1;}', '''  if (s->io_failed && s->state == 2)
    {
      /* UI-thread only. Failed stream callbacks stop before using file.
       * Keep LVGL font objects alive until existing labels detach. Release
       * the unusable file so it cannot pin an invalid FAT mount.
       */

      if (s->file)
        {
          fclose(s->file);
          s->file = NULL;
        }

      s->state = 3;
      s->error = -EIO;
      return -1;
    }''')


PATCHES = {
    'nuttx/drivers/mmcsd/mmcsd_spi.c': patch_sd,
    'nuttx/fs/fat/fs_fat32util.c': patch_fat,
    'nuttx/arch/risc-v/src/common/espressif/esp_spi.c': patch_spi,
    'apps/system/desktop/glass_sd_font.c': patch_font,
}
