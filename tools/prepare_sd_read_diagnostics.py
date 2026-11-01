"""Opt-in, reversible SD SPI token-wait diagnostics in a prepared NuttX tree.

No retries, media initialization, clock changes, or write-path changes.  Match
the complete known receive function before touching the tree.  The normal
builder restores our exact diagnostic form when the option is omitted.
"""
import hashlib
from pathlib import Path


RELATIVE = 'drivers/mmcsd/mmcsd_spi.c'
MARKER = 'ESPCLAW SD READ TOKEN DIAGNOSTICS'
BASE = '''static int mmcsd_recvblock(FAR struct mmcsd_slot_s *slot,
                           FAR uint8_t *buffer, int nbytes)
{
  FAR struct spi_dev_s *spi = slot->spi;
  clock_t start;
  clock_t elapsed;
  uint8_t token;

  /* Wait up to the maximum to receive a valid data token.  taccess is the
   * time from when the command is sent until the first byte of data is
   * received.
   */

  start = START_TIME;
  do
    {
      token = SPI_SEND(spi, 0xff);
      elapsed = ELAPSED_TIME(start);
    }
  while (token == 0xff && elapsed < slot->taccess);

  if (token == MMCSD_SPIDT_STARTBLKSNGL)
    {
      /* Receive the block */

      SPI_RECVBLOCK(spi, buffer, nbytes);

      /* Discard the CRC */

      SPI_SEND(spi, 0xff);
      SPI_SEND(spi, 0xff);
      return OK;
    }

  ferr("ERROR: Did not receive data token (%02x)\\n", token);
  return ERROR;
}'''
DIAGNOSTIC = BASE.replace(
    '  uint8_t token;\n',
    '  uint8_t token;\n  uint32_t polls = 0;\n\n'
    '  /* ' + MARKER + ': failure-only report. */\n', 1).replace(
    '      token = SPI_SEND(spi, 0xff);\n',
    '      token = SPI_SEND(spi, 0xff);\n      polls++;\n', 1).replace(
    '  ferr("ERROR: Did not receive data token (%02x)\\n", token);',
    '  ferr("ERROR: Did not receive data token (%02x)"\n'
    '       " elapsed_ticks=%jd limit_ticks=%jd polls=%" PRIu32 "\\n",\n'
    '       token, (intmax_t)elapsed, (intmax_t)slot->taccess, polls);', 1)


def transform(source, enabled=False):
    """Fail closed on upstream changes or partial/duplicate instrumentation."""
    if (source.count('static int mmcsd_recvblock(') != 1 or
            source.count(BASE) + source.count(DIAGNOSTIC) != 1 or
            source.count(MARKER) != source.count(DIAGNOSTIC)):
        raise RuntimeError('Unrecognized SD SPI receive function; review first')
    current = DIAGNOSTIC if DIAGNOSTIC in source else BASE
    target = DIAGNOSTIC if enabled else BASE
    return source.replace(current, target, 1)


def apply(nuttx, enabled=False):
    path = Path(nuttx) / RELATIVE
    previous = path.read_bytes()
    updated = transform(previous.decode('utf-8'), enabled).encode('utf-8')
    if updated != previous:
        path.write_bytes(updated)
    return {'enabled': bool(enabled), 'source': RELATIVE,
            'sha256': hashlib.sha256(updated).hexdigest(),
            'receive_function_sha256': hashlib.sha256(
                (DIAGNOSTIC if enabled else BASE).encode()).hexdigest(),
            'read_retry_added': False, 'write_path_changed': False,
            'clock_or_timeout_changed': False}
