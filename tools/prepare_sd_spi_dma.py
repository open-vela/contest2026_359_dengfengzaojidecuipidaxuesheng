"""Propagate a sticky lower-half DMA failure through the SPI block API."""
import hashlib
from pathlib import Path

SOURCE = 'drivers/mmcsd/mmcsd_spi.c'
READ = '''
#ifdef CONFIG_ESPRESSIF_SPI3_DMA
      /* ESPCLAW SD DMA RX FAULT: do not publish a failed sector. */

      if ((SPI_STATUS(spi, SPIDEV_MMCSD(0)) & SPI_STATUS_PRESENT) == 0)
        {
          return -EIO;
        }
#endif
'''
WRITE = '''
#ifdef CONFIG_ESPRESSIF_SPI3_DMA
  /* ESPCLAW SD DMA TX FAULT: never replay an uncertain write. */

  if ((SPI_STATUS(spi, SPIDEV_MMCSD(0)) & SPI_STATUS_PRESENT) == 0)
    {
      return -EIO;
    }
#endif
'''

HS_CALL = """
  /* ESPCLAW SD HIGH SPEED NEGOTIATION */

  result = mmcsd_highspeed(slot, csd);
  if (result < 0)
    {
      SPI_SELECT(spi, SPIDEV_MMCSD(0), false);
      return result;
    }
"""
HS_SOURCE = Path(__file__).with_name('sd_spi_highspeed.inc').read_text()
HS_BLOCK = '/* BEGIN ESPCLAW SD HIGH SPEED */\n\n' + HS_SOURCE + '\n/* END ESPCLAW SD HIGH SPEED */\n\n'
HS_INCLUDE = '#include <syslog.h> /* ESPCLAW SD HIGH SPEED */\n'


def apply(nuttx, enabled, highspeed=False):
    path = nuttx / SOURCE
    data = path.read_text()
    begin = '/* BEGIN ESPCLAW SD HIGH SPEED */'
    end = '/* END ESPCLAW SD HIGH SPEED */\n\n'
    assert data.count(begin) == data.count(end) <= 1
    if begin in data:
        first = data.index(begin)
        last = data.index(end, first) + len(end)
        data = data[:first] + data[last:]
    for block in [HS_CALL, HS_INCLUDE]:
        assert data.count(block) <= 1
        data = data.replace(block, '')
    if highspeed:
        include = '#include <nuttx/config.h>\n'
        assert data.count(include) == 1
        data = data.replace(include, include + HS_INCLUDE)
        function_anchor = '/****************************************************************************\n * Block Driver Operations'
        assert data.count(function_anchor) == 1
        data = data.replace(function_anchor, HS_BLOCK + function_anchor)
        call_anchor = '  mmcsd_decodecsd(slot, csd);\n  mmcsd_checkwrprotect(slot, csd);\n'
        assert data.count(call_anchor) == 1
        data = data.replace(call_anchor, call_anchor + HS_CALL)

    for block in [READ, WRITE]:
        assert data.count(block) <= 1
        data = data.replace(block, '')
    if enabled:
        for anchor, block in [
            ('      SPI_RECVBLOCK(spi, buffer, nbytes);\n', READ),
            ('  SPI_SNDBLOCK(spi, buffer, nbytes);\n', WRITE),
        ]:
            assert data.count(anchor) == 1, anchor
            data = data.replace(anchor, anchor + block)
    path.write_text(data)
    return dict(fault_propagation=enabled, cmd6_highspeed=highspeed, source=SOURCE,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())
